// Uninstall.exe — lives in <root>. Removes shortcuts, registry entries and the whole <root> folder.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using Microsoft.Win32;

static class Uninstaller
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        string self = Application.ExecutablePath;
        string root = Path.GetDirectoryName(self);
        if (!AppInfo.IsInstallDir(root))
        {
            MessageBox.Show("This folder does not look like an installation of " + AppInfo.Name + ":\n" + root +
                "\n\nNothing was removed.", AppInfo.Name, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        string sys = Path.Combine(root, AppInfo.SystemDir);
        int userFiles = 0;
        foreach (string d in new[] { "Projects", "Presets", "Autosaves" })
        {
            string p = Path.Combine(sys, d);
            if (Directory.Exists(p)) userFiles += Directory.GetFiles(p, "*", SearchOption.AllDirectories).Length;
        }
        string msg = "Remove " + AppInfo.Name + " from this computer?\n\nThe folder\n" + root +
            "\nwill be deleted together with the shortcuts and the .hairmesh file icon.";
        if (userFiles > 0)
            msg += "\n\nWARNING: " + userFiles + " file(s) in System\\Projects, Presets and Autosaves will be deleted too. " +
                   "Copy them somewhere else first if you need them.";
        if (MessageBox.Show(msg, "Uninstall " + AppInfo.Name, MessageBoxButtons.YesNo,
                userFiles > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
            return;

        Environment.CurrentDirectory = Path.GetTempPath();
        if (AppInfo.Running(root).Count > 0 &&
            MessageBox.Show(AppInfo.Name + " is open. It will be closed — save your work first.\n\nClose it now?", "Uninstall " + AppInfo.Name,
                MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
        AppInfo.StopProgram(root);

        AppInfo.RemoveShortcutInto(AppInfo.DesktopLnk, root);
        AppInfo.RemoveShortcutInto(AppInfo.StartMenuLnk, root);
        if (AppInfo.AssocInside(root)) AppInfo.RemoveAssoc();
        AppInfo.RemoveOldEntries(root);
        try
        {
            if (AppInfo.IsInside(AppInfo.RegString(Registry.CurrentUser, AppInfo.UninstallKey, "InstallLocation"), root))
                Registry.CurrentUser.DeleteSubKeyTree(AppInfo.UninstallKey, false);
            if (AppInfo.IsInside(AppInfo.RegString(Registry.CurrentUser, AppInfo.RegKey, "InstallPath"), root))
                Registry.CurrentUser.DeleteSubKeyTree(AppInfo.RegKey, false);
        }
        catch { }
        AppInfo.NotifyShell();

        // everything except this exe now; the folder itself after this process exits
        string selfN = AppInfo.Norm(self);
        foreach (string d in Directory.GetDirectories(root))
            try { Directory.Delete(d, true); } catch { }
        foreach (string f in Directory.GetFiles(root).Where(f => AppInfo.Norm(f) != selfN))
            try { File.SetAttributes(f, FileAttributes.Normal); File.Delete(f); } catch { }

        MessageBox.Show(AppInfo.Name + " has been removed.", "Uninstall " + AppInfo.Name,
            MessageBoxButtons.OK, MessageBoxIcon.Information);

        Process.Start(new ProcessStartInfo("cmd.exe",
            "/c ping 127.0.0.1 -n 3 >nul & rd /s /q \"" + root + "\"")
        { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Path.GetTempPath(), WindowStyle = ProcessWindowStyle.Hidden });
    }
}
