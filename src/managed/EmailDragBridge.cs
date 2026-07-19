using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows;
using GongSolutions.Wpf.DragDrop;

namespace MailbirdDnd.Managed
{
    public static class EmailDragBridge
    {
        private static readonly object LogLock = new object();
        private static int dragNumber;

        public static void Prepare(IDragInfo dragInfo)
        {
            try
            {
                if (dragInfo == null || dragInfo.Data == null)
                    return;

                List<object> conversations = GetConversations(dragInfo.Data);
                if (conversations.Count == 0)
                    return;

                string root = Environment.GetEnvironmentVariable("MAILBIRD_DND_STAGE_WIN");
                if (string.IsNullOrEmpty(root))
                    return;
                CleanupOldDirectories(root);
                string directory = Path.Combine(root, "email-" + Process.GetCurrentProcess().Id + "-" +
                    System.Threading.Interlocked.Increment(ref dragNumber) + "-" + DateTime.UtcNow.Ticks);
                Directory.CreateDirectory(directory);

                var paths = new List<string>();
                foreach (object conversation in conversations)
                {
                    object message = GetProperty(GetProperty(conversation, "Data"), "ActionMessage");
                    if (message == null)
                        continue;
                    MethodInfo getSource = message.GetType().GetMethod("GetSource",
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                        null, Type.EmptyTypes, null);
                    if (getSource == null)
                        continue;
                    byte[] source = getSource.Invoke(message, null) as byte[];
                    if (source == null || source.Length == 0)
                        continue;

                    string subject = GetProperty(message, "Subject") as string;
                    string path = UniquePath(directory, SafeFileName(subject));
                    File.WriteAllBytes(path, source);
                    paths.Add(path);
                }

                if (paths.Count == 0)
                {
                    TryDeleteDirectory(directory);
                    Log("email-drag: no message source available");
                    return;
                }

                var dataObject = new DataObject();
                dataObject.SetData(DataFormats.FileDrop, paths.ToArray());
                dragInfo.DataObject = dataObject;
                Log("email-drag: prepared " + paths.Count + " EML file(s)");
            }
            catch (TargetInvocationException exception)
            {
                Exception inner = exception.InnerException ?? exception;
                Log("email-drag: source failed: " + inner.GetType().FullName + ": " + inner.Message);
            }
            catch (Exception exception)
            {
                Log("email-drag: bridge failed: " + exception.GetType().FullName + ": " + exception.Message);
            }
        }

        private static List<object> GetConversations(object data)
        {
            var result = new List<object>();
            IEnumerable values = data as IEnumerable;
            if (values == null || data is string)
            {
                AddConversation(result, data);
                return result;
            }
            foreach (object value in values)
                AddConversation(result, value);
            return result;
        }

        private static void AddConversation(List<object> result, object value)
        {
            if (value == null)
                return;
            string name = value.GetType().FullName;
            if (name != null && name.IndexOf("FolderConversationWrapper", StringComparison.Ordinal) >= 0)
                result.Add(value);
        }

        private static object GetProperty(object value, string name)
        {
            if (value == null)
                return null;
            PropertyInfo property = value.GetType().GetProperty(name,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            return property == null ? null : property.GetValue(value, null);
        }

        private static string SafeFileName(string subject)
        {
            if (string.IsNullOrWhiteSpace(subject))
                subject = "email";
            foreach (char invalid in Path.GetInvalidFileNameChars())
                subject = subject.Replace(invalid, '_');
            subject = subject.Trim().TrimEnd('.', ' ');
            if (subject.Length == 0)
                subject = "email";
            if (subject.Length > 120)
                subject = subject.Substring(0, 120).TrimEnd('.', ' ');
            return subject + ".eml";
        }

        private static string UniquePath(string directory, string name)
        {
            string path = Path.Combine(directory, name);
            if (!File.Exists(path))
                return path;
            string stem = Path.GetFileNameWithoutExtension(name);
            string extension = Path.GetExtension(name);
            for (int suffix = 2; suffix < 10000; suffix++)
            {
                path = Path.Combine(directory, stem + " (" + suffix + ")" + extension);
                if (!File.Exists(path))
                    return path;
            }
            return Path.Combine(directory, Guid.NewGuid().ToString("N") + extension);
        }

        private static void TryDeleteDirectory(string directory)
        {
            try { Directory.Delete(directory, true); }
            catch { }
        }

        private static void CleanupOldDirectories(string root)
        {
            try
            {
                DateTime cutoff = DateTime.UtcNow.AddDays(-1);
                foreach (string directory in Directory.GetDirectories(root, "email-*"))
                {
                    if (Directory.GetLastWriteTimeUtc(directory) < cutoff)
                        TryDeleteDirectory(directory);
                }
            }
            catch { }
        }

        private static void Log(string message)
        {
            try
            {
                string root = Environment.GetEnvironmentVariable("MAILBIRD_DND_STAGE_WIN");
                if (string.IsNullOrEmpty(root))
                    return;
                Directory.CreateDirectory(root);
                lock (LogLock)
                    File.AppendAllText(Path.Combine(root, "managed.log"), message + Environment.NewLine);
            }
            catch { }
        }
    }
}
