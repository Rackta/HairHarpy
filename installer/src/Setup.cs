// Setup.exe — installs / updates the native program. Payload: the program's folder (a zip: the exe, Qt, the MSVC runtime),
// the uninstaller, the icons, the example projects — embedded as resources. An installation of the HTML version is
// updated in place: its program files go, the projects, presets and autosaves stay.
// Silent mode (for tests): Setup.exe --silent --dir "<install folder>" [--no-desktop] [--no-startmenu] [--no-assoc]
// [--no-register] (nothing written to the registry: an installation for a check only)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using Microsoft.Win32;

class SetupForm : Form
{
    TextBox pathBox = new TextBox();
    Button browseBtn = new Button(), installBtn = new Button(), cancelBtn = new Button();
    CheckBox desktopChk = new CheckBox(), startChk = new CheckBox(), assocChk = new CheckBox();
    Label modeLbl = new Label(), statusLbl = new Label();
    ProgressBar bar = new ProgressBar();
    bool update;

    public SetupForm()
    {
        Text = AppInfo.Name + " Setup";
        Font = new Font("Segoe UI", 9f);
        AutoScaleMode = AutoScaleMode.Dpi;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(560, 380);
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        Panel head = new Panel { Dock = DockStyle.Top, Height = 112, BackColor = Color.FromArgb(28, 26, 30) };
        PictureBox logo = new PictureBox { Location = new Point(16, 12), Size = new Size(88, 88), SizeMode = PictureBoxSizeMode.Zoom };
        Stream ls = Setup.Res("logo.png");
        if (ls != null) logo.Image = Image.FromStream(ls);
        Label title = new Label { Text = AppInfo.Name, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 15f),
            Location = new Point(118, 26), AutoSize = true };
        Label ver = new Label { Text = "Version " + BuildInfo.Version + " · native (Vulkan)", ForeColor = Color.FromArgb(200, 190, 170),
            Location = new Point(121, 62), AutoSize = true };
        head.Controls.AddRange(new Control[] { logo, title, ver });

        modeLbl.SetBounds(16, 124, 528, 36);
        Label pl = new Label { Text = "Install folder:", Location = new Point(16, 166), AutoSize = true };
        pathBox.SetBounds(16, 186, 430, 24);
        browseBtn.Text = "Browse…"; browseBtn.SetBounds(454, 185, 90, 26);
        desktopChk.Text = "Create a desktop shortcut"; desktopChk.SetBounds(16, 220, 400, 22); desktopChk.Checked = true;
        startChk.Text = "Create a Start menu shortcut"; startChk.SetBounds(16, 244, 400, 22); startChk.Checked = true;
        assocChk.Text = "Open .hairmesh project files with the program (and show its icon on them)"; assocChk.SetBounds(16, 268, 520, 22); assocChk.Checked = true;
        bar.SetBounds(16, 298, 528, 10); bar.Visible = false;
        statusLbl.SetBounds(16, 314, 330, 44); statusLbl.ForeColor = Color.DimGray;
        installBtn.SetBounds(356, 336, 100, 30);
        cancelBtn.Text = "Cancel"; cancelBtn.SetBounds(464, 336, 80, 30);
        Controls.AddRange(new Control[] { head, modeLbl, pl, pathBox, browseBtn, desktopChk, startChk, assocChk, bar, statusLbl, installBtn, cancelBtn });
        AcceptButton = installBtn; CancelButton = cancelBtn;

        browseBtn.Click += delegate { Browse(); };
        cancelBtn.Click += delegate { Close(); };
        installBtn.Click += delegate { Run(); };
        pathBox.TextChanged += delegate { Refresh2(); };

        string found = Setup.FindExisting();
        pathBox.Text = found ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), AppInfo.Name);
        if (found != null)
        {   // keep the choices made at the previous install
            desktopChk.Checked = Setup.RegFlag("Desktop", true);
            startChk.Checked = Setup.RegFlag("StartMenu", true);
            assocChk.Checked = Setup.RegFlag("Assoc", true);
        }
        Refresh2();
        Shown += delegate { pathBox.Select(0, 0); installBtn.Focus(); };
    }

    void Refresh2()
    {
        string root = pathBox.Text.Trim();
        update = root.Length > 0 && AppInfo.IsInstallDir(root);
        installBtn.Text = update ? "Update" : "Install";
        string prev = Setup.RegString("Version");
        bool html = update && File.Exists(Path.Combine(root, AppInfo.SystemDir, AppInfo.OldMainFile));
        modeLbl.Text = update
            ? "The program is already installed in this folder" + (prev != null ? " (version " + prev + (html ? ", the HTML version" : "") + ")" : "") +
              ". It will be updated; your projects, presets and autosaves are kept."
            : "Choose where to install " + AppInfo.Name + ". A folder with the program name is created there.";
    }

    void Browse()
    {
        using (FolderBrowserDialog d = new FolderBrowserDialog())
        {
            d.Description = "Choose a folder. \"" + AppInfo.Name + "\" will be created inside it " +
                            "(or pick an existing \"" + AppInfo.Name + "\" folder to update it).";
            try
            {
                string cur = pathBox.Text.Trim();
                d.SelectedPath = Directory.Exists(cur) ? cur : Path.GetDirectoryName(cur);
            }
            catch { }
            if (d.ShowDialog(this) != DialogResult.OK) return;
            string p = d.SelectedPath;
            if (!AppInfo.IsInstallDir(p) && !string.Equals(Path.GetFileName(p.TrimEnd('\\')), AppInfo.Name, StringComparison.OrdinalIgnoreCase))
                p = Path.Combine(p, AppInfo.Name);
            pathBox.Text = p;
        }
    }

    void Run()
    {
        string root = pathBox.Text.Trim();
        if (root.Length == 0 || !Path.IsPathRooted(root))
        {
            MessageBox.Show(this, "Please choose a full folder path, e.g. C:\\Programs\\" + AppInfo.Name, Text,
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        // the program open from this folder: closed first (it asks about unsaved changes itself)
        var running = AppInfo.Running(Path.GetFullPath(root));
        if (running.Count > 0)
        {
            foreach (var p in running) p.Dispose();
            if (MessageBox.Show(this, AppInfo.Name + " is open. It will be closed to update it — save your work first.\n\nClose it now?", Text,
                    MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
            AppInfo.StopProgram(Path.GetFullPath(root));
        }
        bool wasUpdate = update;
        foreach (Control c in Controls) c.Enabled = false;
        bar.Enabled = true; bar.Visible = true; statusLbl.Enabled = true;
        UseWaitCursor = true;
        statusLbl.Text = wasUpdate ? "Updating…" : "Installing…";
        Application.DoEvents();
        try
        {
            Setup.Install(Path.GetFullPath(root), desktopChk.Checked, startChk.Checked, assocChk.Checked, (done, total, what) =>
            {
                bar.Maximum = Math.Max(1, total); bar.Value = Math.Min(done, bar.Maximum);
                statusLbl.Text = (wasUpdate ? "Updating… " : "Installing… ") + what; Application.DoEvents();
            });
        }
        catch (Exception e)
        {
            UseWaitCursor = false;
            foreach (Control c in Controls) c.Enabled = true;
            bar.Visible = false; statusLbl.Text = "";
            MessageBox.Show(this, "Installation failed:\n" + e.Message +
                "\n\nIf the program is open, close it and try again. Folders that need administrator rights " +
                "(e.g. Program Files) are not supported — choose a folder in your user profile.",
                Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        UseWaitCursor = false;
        statusLbl.Text = ""; bar.Visible = false;
        if (MessageBox.Show(this, AppInfo.Name + (wasUpdate ? " has been updated." : " has been installed.") +
                "\n\nStart the program now?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            try { Process.Start(new ProcessStartInfo(Path.Combine(root, AppInfo.SystemDir, AppInfo.ProgramExe)) { WorkingDirectory = Path.Combine(root, AppInfo.SystemDir) }); } catch { }
        Close();
    }
}

static class Setup
{
    public static Stream Res(string name)
    {
        return Assembly.GetExecutingAssembly().GetManifestResourceStream("payload." + name);
    }

    static void Extract(string name, string dest)
    {
        using (Stream s = Res(name))
        {
            if (s == null) throw new Exception("Setup is damaged: missing " + name);
            WriteFile(s, dest);
        }
    }
    static void WriteFile(Stream s, string dest)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(dest));
        string tmp = dest + ".new";
        using (FileStream f = File.Create(tmp)) s.CopyTo(f);
        if (File.Exists(dest)) { File.SetAttributes(dest, FileAttributes.Normal); File.Delete(dest); }
        File.Move(tmp, dest);
    }

    public static string RegString(string name) { return AppInfo.RegString(Registry.CurrentUser, AppInfo.RegKey, name) ?? AppInfo.RegString(Registry.CurrentUser, AppInfo.OldRegKey, name); }
    public static bool RegFlag(string name, bool def)
    {
        string v = RegString(name);
        return v == null ? def : v == "1";
    }

    // Existing installation: registry first, then the usual places and the folder Setup.exe is started from.
    public static string FindExisting()
    {
        string[] reg = { RegString("InstallPath"), AppInfo.RegString(Registry.CurrentUser, AppInfo.UninstallKey, "InstallLocation"),
            AppInfo.RegString(Registry.CurrentUser, AppInfo.OldUninstallKey, "InstallLocation") };
        foreach (string p in reg) if (!string.IsNullOrEmpty(p) && AppInfo.IsInstallDir(p)) return p;
        string here = Path.GetDirectoryName(Application.ExecutablePath);
        Environment.SpecialFolder[] places = { Environment.SpecialFolder.MyDocuments, Environment.SpecialFolder.Desktop,
            Environment.SpecialFolder.UserProfile, Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolder.ProgramFiles };
        var cands = new[] { here, Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs") }
            .Concat(places.Select(Environment.GetFolderPath));
        foreach (string c in cands)
        {
            if (string.IsNullOrEmpty(c)) continue;
            foreach (string n in new[] { AppInfo.Name, AppInfo.OldName })
            {
                string p = Path.Combine(c, n);
                if (AppInfo.IsInstallDir(p)) return p;
            }
        }
        return null;
    }

    public static void Install(string root, bool desktop, bool startMenu, bool assoc, Action<int, int, string> progress, bool register = true)
    {
        string sys = Path.Combine(root, AppInfo.SystemDir);
        Directory.CreateDirectory(root);
        foreach (string d in AppInfo.DataDirs) Directory.CreateDirectory(Path.Combine(sys, d));
        AppInfo.StopProgram(root);                                      // (a running one locks its files)

        // the program's files: the zip into System; files of the previous version that are gone — removed
        string listFile = Path.Combine(sys, AppInfo.FilesList);
        var old = File.Exists(listFile) ? new HashSet<string>(File.ReadAllLines(listFile).Where(l => l.Length > 0), StringComparer.OrdinalIgnoreCase) : new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var now = new List<string>();
        using (Stream zs = Res("program.zip"))
        {
            if (zs == null) throw new Exception("Setup is damaged: missing the program");
            using (ZipArchive z = new ZipArchive(zs, ZipArchiveMode.Read))
            {
                var files = z.Entries.Where(e => e.Name.Length > 0).ToList();
                int i = 0;
                foreach (ZipArchiveEntry e in files)
                {
                    string rel = e.FullName.Replace('/', '\\');
                    string dest = Path.GetFullPath(Path.Combine(sys, rel));
                    if (!AppInfo.IsInside(dest, sys)) continue;          // (never outside the folder)
                    using (Stream s = e.Open()) WriteFile(s, dest);
                    now.Add(rel);
                    if (progress != null && (++i % 8 == 0 || i == files.Count)) progress(i, files.Count, e.Name);
                }
            }
        }
        var keep = new HashSet<string>(now, StringComparer.OrdinalIgnoreCase);
        foreach (string rel in old) if (!keep.Contains(rel))
            try { string p = Path.Combine(sys, rel); if (AppInfo.IsInside(p, sys) && File.Exists(p)) { File.SetAttributes(p, FileAttributes.Normal); File.Delete(p); } } catch { }
        // (the HTML version's program and its leftovers: the native program replaces them)
        foreach (string f in new[] { AppInfo.OldMainFile, AppInfo.OldProgramExe, "HairMeshCreator.old.exe" })
            try { string p = Path.Combine(sys, f); if (File.Exists(p)) File.Delete(p); } catch { }
        foreach (string f in Directory.GetFiles(sys, AppInfo.OldMainFile + ".tmp.*")) try { File.Delete(f); } catch { }
        File.WriteAllLines(listFile, now);

        Extract("app.ico", Path.Combine(sys, AppInfo.AppIco));
        Extract("uninstall.ico", Path.Combine(sys, AppInfo.UninstallIco));
        Extract(AppInfo.UninstallExe, Path.Combine(root, AppInfo.UninstallExe));
        // example projects (File → Examples): resources "payload.example.<file name>" → System\Examples (replaced on update)
        string exDir = Path.Combine(sys, "Examples");
        foreach (string r in Assembly.GetExecutingAssembly().GetManifestResourceNames())
            if (r.StartsWith("payload.example.")) Extract(r.Substring("payload.".Length), Path.Combine(exDir, r.Substring("payload.example.".Length)));

        string program = Path.Combine(sys, AppInfo.ProgramExe);
        string appIco = Path.Combine(sys, AppInfo.AppIco);
        string uninst = Path.Combine(root, AppInfo.UninstallExe);
        const string desc = "3D modelling of stylized hair";
        AppInfo.CreateShortcut(Path.Combine(root, AppInfo.ShortcutName), program, sys, appIco, desc);
        if (desktop) AppInfo.CreateShortcut(AppInfo.DesktopLnk, program, sys, appIco, desc);
        else AppInfo.RemoveShortcutInto(AppInfo.DesktopLnk, root);
        if (startMenu) AppInfo.CreateShortcut(AppInfo.StartMenuLnk, program, sys, appIco, desc);
        else AppInfo.RemoveShortcutInto(AppInfo.StartMenuLnk, root);

        if (!register) return;
        AppInfo.RemoveOldEntries(root);                                 // (the name before 2.0: its shortcuts and keys replaced by these)
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(AppInfo.RegKey))
        {
            k.SetValue("InstallPath", root);
            k.SetValue("Version", BuildInfo.Version);
            k.SetValue("Desktop", desktop ? "1" : "0");
            k.SetValue("StartMenu", startMenu ? "1" : "0");
            k.SetValue("Assoc", assoc ? "1" : "0");
        }
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(AppInfo.UninstallKey))
        {
            k.SetValue("DisplayName", AppInfo.Name);
            k.SetValue("DisplayVersion", BuildInfo.Version);
            k.SetValue("Publisher", "catarackta");
            k.SetValue("DisplayIcon", appIco);
            k.SetValue("InstallLocation", root);
            k.SetValue("UninstallString", "\"" + uninst + "\"");
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            long bytes = new DirectoryInfo(root).GetFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
            k.SetValue("EstimatedSize", (int)(bytes / 1024), RegistryValueKind.DWord);
        }
        if (assoc)
        {
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\Classes\" + AppInfo.Ext))
                k.SetValue("", AppInfo.ProgId);
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\Classes\" + AppInfo.ProgId))
                k.SetValue("", AppInfo.Name + " project");
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\Classes\" + AppInfo.ProgId + @"\DefaultIcon"))
                k.SetValue("", "\"" + appIco + "\",0");
            // double-click on a project: the program opens it
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\Classes\" + AppInfo.ProgId + @"\shell\open\command"))
                k.SetValue("", "\"" + program + "\" \"%1\"");
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\Classes\" + AppInfo.Ext + @"\OpenWithProgids"))
                k.SetValue(AppInfo.ProgId, new byte[0], RegistryValueKind.None);
            // "Open with → Choose another app": Windows lists (and accepts when browsed to) only an exe registered
            // under Applications — without it picking HairHarpy.exe does nothing
            string app = @"Software\Classes\Applications\" + AppInfo.ProgramExe;
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(app))
                k.SetValue("FriendlyAppName", AppInfo.Name);
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(app + @"\DefaultIcon"))
                k.SetValue("", "\"" + appIco + "\",0");
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(app + @"\SupportedTypes"))
                k.SetValue(AppInfo.Ext, "");
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(app + @"\shell\open\command"))
                k.SetValue("", "\"" + program + "\" \"%1\"");
        }
        else if (AppInfo.AssocInside(root)) AppInfo.RemoveAssoc();
        AppInfo.NotifyShell();
    }

    [STAThread]
    static int Main(string[] args)
    {
        if (args.Contains("--silent"))
        {
            int i = Array.IndexOf(args, "--dir");
            string dir = i >= 0 && i + 1 < args.Length ? args[i + 1] : FindExisting();
            if (dir == null) return 2;
            try { Install(Path.GetFullPath(dir), !args.Contains("--no-desktop"), !args.Contains("--no-startmenu"), !args.Contains("--no-assoc"), null, !args.Contains("--no-register")); }
            catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
            return 0;
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
        return 0;
    }
}
