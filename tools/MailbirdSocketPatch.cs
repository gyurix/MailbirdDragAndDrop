using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

internal static class MailbirdSocketPatch
{
    private const string TypeName = "Limilabs.Client.ClientBase";
    private const string BackupSuffix = ".mailbird-original";

    private static int Main(string[] args)
    {
        if (args.Length != 2 || (args[0] != "apply" && args[0] != "verify" && args[0] != "restore"))
        {
            Console.Error.WriteLine("Usage: MailbirdSocketPatch.exe {apply|verify|restore} /path/to/Mail.dll");
            return 2;
        }

        string mode = args[0];
        string path = Path.GetFullPath(args[1]);
        if (!File.Exists(path))
        {
            Console.Error.WriteLine("Assembly not found: " + path);
            return 1;
        }

        try
        {
            if (mode == "restore")
                return Restore(path);

            PatchState state = Inspect(path);
            if (mode == "verify")
            {
                Console.WriteLine(state.Message);
                return state.IsKnown ? 0 : 1;
            }

            if (state.IsPatched)
            {
                Console.WriteLine("Already patched: " + path);
                return 0;
            }
            if (!state.IsOriginal)
            {
                Console.Error.WriteLine("Refusing to patch: unexpected ClientBase constructor in " + path);
                Console.Error.WriteLine(state.Message);
                return 1;
            }

            string backup = path + BackupSuffix;
            if (!File.Exists(backup))
            {
                File.Copy(path, backup);
                Console.WriteLine("Original saved: " + backup);
            }

            Apply(path);
            PatchState result = Inspect(path);
            if (!result.IsPatched)
                throw new InvalidDataException("Patched assembly failed verification: " + result.Message);
            Console.WriteLine("Patched and verified: " + path);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }

    private static int Restore(string path)
    {
        string backup = path + BackupSuffix;
        if (!File.Exists(backup))
        {
            Console.Error.WriteLine("Backup not found: " + backup);
            return 1;
        }

        AtomicReplace(backup, path, keepSource: true);
        Console.WriteLine("Restored original assembly: " + path);
        return 0;
    }

    private static void Apply(string path)
    {
        AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path);
        TypeDefinition clientBase = assembly.MainModule.GetType(TypeName);
        if (clientBase == null)
            throw new InvalidDataException("Type not found: " + TypeName);

        MethodDefinition ctor = clientBase.Methods.FirstOrDefault(m => m.Name == ".ctor" && m.Parameters.Count == 0);
        if (ctor == null || !ctor.HasBody)
            throw new InvalidDataException("Default ClientBase constructor not found");

        Instruction socketNew = FindSocketConstructor(ctor, expectedParameterCount: 2);
        if (socketNew == null)
            throw new InvalidDataException("Original Socket(SocketType, ProtocolType) call not found");

        ILProcessor il = ctor.Body.GetILProcessor();
        Instruction family = socketNew.Previous.Previous;
        if (family == null || !IsLdci4(family, 1) || socketNew.Previous == null || !IsLdci4(socketNew.Previous, 6))
            throw new InvalidDataException("Unexpected ClientBase constructor instruction sequence");

        family.OpCode = OpCodes.Ldc_I4;
        family.Operand = 2; // AddressFamily.InterNetwork
        il.InsertAfter(family, il.Create(OpCodes.Ldc_I4_1)); // SocketType.Stream

        MethodReference ipv4Ctor = assembly.MainModule.ImportReference(
            typeof(System.Net.Sockets.Socket).GetConstructor(new[] {
                typeof(System.Net.Sockets.AddressFamily),
                typeof(System.Net.Sockets.SocketType),
                typeof(System.Net.Sockets.ProtocolType)
            }));
        il.Replace(socketNew, il.Create(OpCodes.Newobj, ipv4Ctor));

        string temp = path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            assembly.Write(temp);
            AtomicReplace(temp, path, keepSource: false);
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
            assembly.Dispose();
        }
    }

    private static PatchState Inspect(string path)
    {
        using (AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path))
        {
            TypeDefinition clientBase = assembly.MainModule.GetType(TypeName);
            if (clientBase == null)
                return new PatchState(false, false, "Type not found: " + TypeName);
            MethodDefinition ctor = clientBase.Methods.FirstOrDefault(m => m.Name == ".ctor" && m.Parameters.Count == 0);
            if (ctor == null || !ctor.HasBody)
                return new PatchState(false, false, "Default ClientBase constructor not found");

            if (FindSocketConstructor(ctor, 3) != null)
                return new PatchState(false, true, "IPv4 ClientBase constructor is present");
            if (FindSocketConstructor(ctor, 2) != null)
                return new PatchState(true, false, "Original dual-mode ClientBase constructor is present");
            return new PatchState(false, false, "Unexpected ClientBase constructor instruction sequence");
        }
    }

    private static Instruction FindSocketConstructor(MethodDefinition method, int expectedParameterCount)
    {
        foreach (Instruction instruction in method.Body.Instructions)
        {
            if (instruction.OpCode != OpCodes.Newobj) continue;
            MethodReference reference = instruction.Operand as MethodReference;
            if (reference == null || reference.Parameters.Count != expectedParameterCount) continue;
            if (reference.DeclaringType.FullName == "System.Net.Sockets.Socket") return instruction;
        }
        return null;
    }

    private static bool IsLdci4(Instruction instruction, int value)
    {
        if (instruction == null) return false;
        if (instruction.OpCode == OpCodes.Ldc_I4_M1) return value == -1;
        if (instruction.OpCode == OpCodes.Ldc_I4_0) return value == 0;
        if (instruction.OpCode == OpCodes.Ldc_I4_1) return value == 1;
        if (instruction.OpCode == OpCodes.Ldc_I4_2) return value == 2;
        if (instruction.OpCode == OpCodes.Ldc_I4_3) return value == 3;
        if (instruction.OpCode == OpCodes.Ldc_I4_4) return value == 4;
        if (instruction.OpCode == OpCodes.Ldc_I4_5) return value == 5;
        if (instruction.OpCode == OpCodes.Ldc_I4_6) return value == 6;
        if (instruction.OpCode == OpCodes.Ldc_I4_7) return value == 7;
        if (instruction.OpCode == OpCodes.Ldc_I4_8) return value == 8;
        return instruction.OpCode == OpCodes.Ldc_I4 && Convert.ToInt32(instruction.Operand) == value;
    }

    private static void AtomicReplace(string source, string destination, bool keepSource)
    {
        string temp = destination + ".replace-" + Guid.NewGuid().ToString("N");
        File.Copy(source, temp, true);
        try
        {
            File.Replace(temp, destination, null);
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
        if (!keepSource && File.Exists(source)) File.Delete(source);
    }

    private struct PatchState
    {
        internal readonly bool IsOriginal;
        internal readonly bool IsPatched;
        internal readonly string Message;
        internal bool IsKnown { get { return IsOriginal || IsPatched; } }
        internal PatchState(bool original, bool patched, string message)
        {
            IsOriginal = original; IsPatched = patched; Message = message;
        }
    }
}
