using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

internal static class PatchGong
{
    private const string BridgeType = "MailbirdDnd.Managed.EmailDragBridge";

    public static int Main(string[] args)
    {
        if (args.Length != 4)
        {
            Console.Error.WriteLine("usage: PatchGong GONG_DLL BRIDGE_DLL OUTPUT_DLL WPF_DIR");
            return 2;
        }

        string gongPath = Path.GetFullPath(args[0]);
        string bridgePath = Path.GetFullPath(args[1]);
        string outputPath = Path.GetFullPath(args[2]);
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(Path.GetDirectoryName(gongPath));
        resolver.AddSearchDirectory(Path.GetDirectoryName(bridgePath));
        resolver.AddSearchDirectory(Path.GetFullPath(args[3]));
        resolver.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(args[3])));

        using (var gong = AssemblyDefinition.ReadAssembly(gongPath,
                   new ReaderParameters { AssemblyResolver = resolver, ReadWrite = false }))
        using (var bridge = AssemblyDefinition.ReadAssembly(bridgePath,
                   new ReaderParameters { AssemblyResolver = resolver }))
        {
            TypeDefinition dragDrop = gong.MainModule.GetType("GongSolutions.Wpf.DragDrop.DragDrop");
            MethodDefinition mouseMove = dragDrop == null ? null : dragDrop.Methods.SingleOrDefault(
                method => method.Name == "DragSource_PreviewMouseMove");
            FieldDefinition dragInfo = dragDrop == null ? null : dragDrop.Fields.SingleOrDefault(
                field => field.Name == "m_DragInfo");
            TypeDefinition bridgeType = bridge.MainModule.GetType(BridgeType);
            MethodDefinition prepare = bridgeType == null ? null : bridgeType.Methods.SingleOrDefault(
                method => method.Name == "Prepare" && method.Parameters.Count == 1);
            if (mouseMove == null || dragInfo == null || prepare == null || !mouseMove.HasBody)
                throw new InvalidOperationException("required drag loop or Prepare method missing");

            MethodReference importedPrepare = gong.MainModule.ImportReference(prepare);
            bool alreadyPatched = mouseMove.Body.Instructions.Any(instruction =>
            {
                MethodReference method = instruction.Operand as MethodReference;
                return method != null && method.Name == "Prepare" &&
                       method.DeclaringType.FullName == BridgeType;
            });
            if (alreadyPatched)
            {
                gong.Write(outputPath);
                Console.WriteLine("Gong drag loop already patched");
                return 0;
            }
            Instruction startCall = mouseMove.Body.Instructions.FirstOrDefault(instruction =>
            {
                MethodReference method = instruction.Operand as MethodReference;
                return method != null && method.Name == "StartDrag" &&
                       method.DeclaringType.FullName == "GongSolutions.Wpf.DragDrop.IDragSource";
            });
            if (startCall == null)
                throw new InvalidOperationException("IDragSource.StartDrag call missing");

            ILProcessor il = mouseMove.Body.GetILProcessor();
            Instruction loadDragInfo = il.Create(OpCodes.Ldsfld, dragInfo);
            Instruction callPrepare = il.Create(OpCodes.Call, importedPrepare);
            il.InsertAfter(startCall, loadDragInfo);
            il.InsertAfter(loadDragInfo, callPrepare);
            mouseMove.Body.MaxStackSize = Math.Max(mouseMove.Body.MaxStackSize, 1);
            gong.Write(outputPath);
        }

        Console.WriteLine("patched Gong drag loop");
        return 0;
    }
}
