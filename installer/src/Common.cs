// Shared by Setup.exe and Uninstall.exe of the native version (compiled into each of them). C# 5 (csc from .NET
// Framework 4, part of Windows). The layout is the HTML version's: <root>\System holds the program and its data folders,
// so an installation of the HTML version is updated in place (its projects, presets and autosaves kept).
using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;

static class AppInfo
{
    public const string Name = "HairHarpy";
    public const string OldName = "Stylized HairMesh Creator"; // (the name before 2.0: its folder, shortcuts and registry entries taken over)
    public const string SystemDir = "System";              // <root>\System: the program itself + its data folders
    public const string ProgramExe = "HairHarpy.exe";
    public const string OldProgramExe = "HairMeshCreator.exe"; // (the program before 2.0, and the HTML version's launcher)
    public const string OldMainFile = "hair-modeler.html";  // (the HTML version's program — gone on an update)
    public const string UninstallExe = "Uninstall.exe";
    public const string FilesList = "program-files.txt";   // (System: the program's own files — an update drops the ones gone)
    public const string AppIco = @"Resources\app.ico";       // relative to System
    public const string UninstallIco = @"Resources\uninstall.ico";
    public static readonly string[] DataDirs = { "Projects", "Presets", "Autosaves", "Resources", "Examples" };

    public const string RegKey = @"Software\HairHarpy";
    public const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\HairHarpy";
    public const string ProgId = "HairHarpy.Project";
    public const string OldRegKey = @"Software\StylizedHairMeshCreator";
    public const string OldUninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\StylizedHairMeshCreator";
    public const string OldProgId = "StylizedHairMesh.Project";
    public const string Ext = ".hairmesh";

    public static string ShortcutName { get { return Name + ".lnk"; } }
    public static string DesktopLnk { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutName); } }
    public static string StartMenuLnk { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), ShortcutName); } }
    public static string OldDesktopLnk { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), OldName + ".lnk"); } }
    public static string OldStartMenuLnk { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), OldName + ".lnk"); } }

    // A folder is an installation of the program (this one or the HTML version) if it has System\<program> or the old page.
    public static bool IsInstallDir(string root)
    {
        try
        {
            string sys = Path.Combine(root, SystemDir);
            return File.Exists(Path.Combine(sys, ProgramExe)) || File.Exists(Path.Combine(sys, OldProgramExe)) || File.Exists(Path.Combine(sys, OldMainFile));
        }
        catch { return false; }
    }

    public static string Norm(string p)
    {
        try { return Path.GetFullPath(p).TrimEnd('\\').ToLowerInvariant(); } catch { return (p ?? "").ToLowerInvariant(); }
    }

    public static bool IsInside(string path, string root)
    {
        if (string.IsNullOrEmpty(path)) return false;
        string a = Norm(path), r = Norm(root);
        return a == r || a.StartsWith(r + "\\");
    }

    public static void CreateShortcut(string lnk, string target, string workDir, string icon, string description)
    {
        Type t = Type.GetTypeFromProgID("WScript.Shell");
        dynamic shell = Activator.CreateInstance(t);
        try
        {
            dynamic sc = shell.CreateShortcut(lnk);
            sc.TargetPath = target;
            sc.WorkingDirectory = workDir;
            sc.IconLocation = icon + ",0";
            sc.Description = description;
            sc.Save();
            Marshal.FinalReleaseComObject(sc);
        }
        finally { Marshal.FinalReleaseComObject(shell); }
    }

    public static string ShortcutTarget(string lnk)
    {
        if (!File.Exists(lnk)) return null;
        try
        {
            Type t = Type.GetTypeFromProgID("WScript.Shell");
            dynamic shell = Activator.CreateInstance(t);
            dynamic sc = shell.CreateShortcut(lnk);
            string target = sc.TargetPath;
            Marshal.FinalReleaseComObject(sc);
            Marshal.FinalReleaseComObject(shell);
            return target;
        }
        catch { return null; }
    }

    // Remove a shortcut only if it points into this installation (never touch someone else's shortcut).
    public static void RemoveShortcutInto(string lnk, string root)
    {
        if (IsInside(ShortcutTarget(lnk), root)) { try { File.Delete(lnk); } catch { } }
    }

    public static string RegString(RegistryKey hive, string key, string name)
    {
        try
        {
            using (RegistryKey k = hive.OpenSubKey(key))
                return k == null ? null : k.GetValue(name) as string;
        }
        catch { return null; }
    }

    // The .hairmesh association is ours if its ProgID icon lives inside root.
    public static bool AssocInside(string root)
    {
        string ico = RegString(Registry.CurrentUser, @"Software\Classes\" + ProgId + @"\DefaultIcon", "");
        return ico != null && IsInside(ico.Trim('"').Split(new[] { "\"," }, StringSplitOptions.None)[0].Trim('"'), root);
    }

    public static void RemoveAssoc()
    {
        try
        {
            string ext = RegString(Registry.CurrentUser, @"Software\Classes\" + Ext, "");
            if (ext == ProgId || ext == OldProgId)
                Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\" + Ext, false);
            Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\" + ProgId, false);
            Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\Applications\" + ProgramExe, false);
        }
        catch { }
        RemoveOldAssoc();
    }
    // what the name before 2.0 left for this installation (or for a folder gone — renamed into it): its shortcuts, its
    // registry keys, its association
    public static void RemoveOldEntries(string root)
    {
        foreach (string lnk in new[] { OldDesktopLnk, OldStartMenuLnk })
        {   // (into this folder, or onto the old program that is gone — its folder renamed)
            string t = ShortcutTarget(lnk);
            if (IsInside(t, root) || (t != null && string.Equals(Path.GetFileName(t), OldProgramExe, StringComparison.OrdinalIgnoreCase) && !File.Exists(t)))
                try { File.Delete(lnk); } catch { }
        }
        try { string l = Path.Combine(root, OldName + ".lnk"); if (File.Exists(l)) File.Delete(l); } catch { }
        foreach (string[] k in new[] { new[] { OldRegKey, "InstallPath" }, new[] { OldUninstallKey, "InstallLocation" } })
        {
            string p = RegString(Registry.CurrentUser, k[0], k[1]);
            if (p == null && RegString(Registry.CurrentUser, k[0], "DisplayName") == null && RegString(Registry.CurrentUser, k[0], "Version") == null) continue;
            if (p == null || IsInside(p, root) || !Directory.Exists(p))
                try { Registry.CurrentUser.DeleteSubKeyTree(k[0], false); } catch { }
        }
        string ico = RegString(Registry.CurrentUser, @"Software\Classes\" + OldProgId + @"\DefaultIcon", "");
        if (ico != null) { string f = ico.Trim('"').Split(new[] { "\"," }, StringSplitOptions.None)[0].Trim('"'); if (IsInside(f, root) || !File.Exists(f)) RemoveOldAssoc(); }
    }
    // the association made under the name before 2.0 (its ProgID, its exe under Applications)
    public static void RemoveOldAssoc()
    {
        try
        {
            Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\" + OldProgId, false);
            Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\Applications\" + OldProgramExe, false);
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\Classes\" + Ext + @"\OpenWithProgids", true))
                if (k != null) k.DeleteValue(OldProgId, false);
        }
        catch { }
    }

    // The program of this installation (or the HTML version's launcher) running: closed before its files are replaced
    // or deleted — asked first by Setup (unsaved work), here it is stopped
    public static System.Collections.Generic.List<System.Diagnostics.Process> Running(string root)
    {
        var list = new System.Collections.Generic.List<System.Diagnostics.Process>();
        foreach (var p in System.Diagnostics.Process.GetProcessesByName(Path.GetFileNameWithoutExtension(ProgramExe))
                     .Concat(System.Diagnostics.Process.GetProcessesByName(Path.GetFileNameWithoutExtension(OldProgramExe))))
        {
            try { if (IsInside(p.MainModule.FileName, root)) { list.Add(p); continue; } } catch { }
            p.Dispose();
        }
        return list;
    }
    public static void StopProgram(string root)
    {
        foreach (var p in Running(root))
        {
            try { if (!p.CloseMainWindow() || !p.WaitForExit(3000)) { p.Kill(); p.WaitForExit(5000); } }
            catch { }
            finally { p.Dispose(); }
        }
    }

    [DllImport("shell32.dll")]
    static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    public static void NotifyShell() { SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero); } // SHCNE_ASSOCCHANGED
}
