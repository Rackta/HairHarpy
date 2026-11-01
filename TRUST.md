# Can I trust the installer?

*Русская версия: [TRUST.ru.md](TRUST.ru.md)*

HairHarpy is made by one person. A code-signing certificate costs hundreds of dollars a year, so the installer and the
program are **not signed**. Because of that, Windows SmartScreen shows "Windows protected your PC" the first time you
run them. That warning means "unknown publisher", not "a virus was found".

You don't have to take our word for anything. This page lists what you can check yourself, from a quick look to
building the program from source.

## 1. Check that the file is the one we published (1 minute)

Every release has a `SHA256SUMS.txt` file next to the downloads. Compare it with the hash of the file you downloaded:

```powershell
Get-FileHash "HairHarpy Setup 2.0.0.exe" -Algorithm SHA256
```

The hash must match the line in `SHA256SUMS.txt` exactly. If it doesn't, the file was changed or damaged on the way:
don't run it.

## 2. Scan it (1 minute)

Upload the installer to [VirusTotal](https://www.virustotal.com/) — it checks the file with about 70 antivirus engines.
The release notes link the report for each release. An unsigned program from a small author may get one or two
"generic" or "ML" detections from less known engines. That is common for unsigned files. Detections by name from
several major engines are not.

## 3. Don't install anything: use the portable version

`HairHarpy-<version>-portable.zip` is the same program without an installer. Unpack it anywhere and run
`HairHarpy.exe`. It doesn't touch the registry outside its own settings key (see below) and doesn't create shortcuts.
To remove it, delete the folder and, if you want, its settings (section 5).

## 4. Read the code, or build it yourself

The full source code is in this repository. Everything the release does is built from it:

- `src/` — the program (C++ / Qt / Vulkan);
- `installer/src/` — the installer and the uninstaller (C#, about 600 lines: easy to read in full);
- `installer/build-installer.ps1` — the script that packs the release.

[BUILDING.md](BUILDING.md) explains how to build the program and the installer on your own computer with free tools.
Your build is a program you made yourself from code you can read. You don't have to trust our files at all.

## 5. What the installer and the program do on your computer

**The installer**
- Needs no administrator rights and asks for none. It installs into a folder in your user profile (by default
  `Documents\HairHarpy`).
- Writes only:
  - the program folder you chose (`System\` — the program, its libraries, your Projects, Presets, Autosaves;
    `Uninstall.exe`; a shortcut);
  - a Start menu shortcut and a desktop shortcut, if you tick them;
  - these registry keys, all in your own user's part of the registry (`HKEY_CURRENT_USER`):
    - `Software\HairHarpy` — the install folder, the version, your choices in the installer;
    - `Software\Microsoft\Windows\CurrentVersion\Uninstall\HairHarpy` — the entry in "Installed apps";
    - `Software\Classes\.hairmesh`, `Software\Classes\HairHarpy.Project`, `Software\Classes\Applications\HairHarpy.exe` —
      opening `.hairmesh` projects with a double-click, if you tick it.
- Doesn't install services, drivers, scheduled tasks, startup entries or background processes. Doesn't touch system
  folders.
- `Uninstall.exe` removes the folder, the shortcuts and the keys above. It warns you first if your projects are inside.

**The program**
- **Doesn't connect to the internet.** No telemetry, no statistics, no update checks, no accounts.
- Its only network use is **Bridge → Send**: when you press it, the meshes go over HTTP to `127.0.0.1` — your own
  computer — where the Blender or Maya bridge add-on receives them. Nothing leaves your computer.
- The links "About the author", "Support the author" and the links in the FAQ open in your browser **only when you
  click them**.
- It starts other programs only when you ask it to: RizomUV (UV editing, the path you set) and itself (Reload).
- It writes:
  - the files you save or export, where you save them;
  - autosaves, presets and projects into `System\` in the program folder (the portable version:
    `%APPDATA%\Catarackta\HairHarpy`);
  - its settings into `HKEY_CURRENT_USER\Software\Catarackta\HairHarpy`;
  - exchange files for RizomUV into `%TEMP%\HairHarpy`, when you send a mesh there;
  - **only while you record a session** (the **● Rec** button, pressed again to stop): a folder
    `Documents\HairHarpy Sessions\session_<date>` with a log of the session — the tools, actions and buttons you used,
    the program's messages, moments the window froze, the project's name and path, the camera, the names and polygon
    counts of the selected and shown meshes — and your notes (**✎ Note** / F9): your text and a picture of the program's
    window with your drawing over it. No mesh data and no other files go into it;
  - **if the program crashes**: a crash dump `crash_<time>.dmp` into `Documents\HairHarpy Sessions` (into the session's
    folder while recording). It is a snapshot of the program's memory at that moment, so it can hold parts of the open
    project.

  The program sends none of this anywhere. The folder appears only when something is written into it; you decide
  whether to send a session folder to the author, and you can delete it at any time.

**The bridge add-ons** (optional, `resources/bridge/`) are plain Python files you can read before installing. The
Blender add-on listens on `127.0.0.1` only. The Maya bridge copies itself into your Maya scripts folder and adds one
line to `userSetup.py` so it starts with Maya — the file says so at its top.

## 6. Checking it yourself while it runs

If you want to see it with your own eyes:
- [Process Monitor](https://learn.microsoft.com/sysinternals/downloads/procmon) shows every file and registry key the
  installer and the program touch;
- [TCPView](https://learn.microsoft.com/sysinternals/downloads/tcpview) or Resource Monitor → Network shows that the
  program makes no internet connections;
- Windows Firewall can block `HairHarpy.exe` from the network entirely. The program works the same, only Bridge →
  Send stops (it needs the local connection to Blender or Maya).

## 7. Linux

The Linux files (`HairHarpy-<version>-x86_64.AppImage`, `HairHarpy-<version>-linux-x86_64.tar.gz`) are built on
GitHub from this repository by [`.github/workflows/linux.yml`](.github/workflows/linux.yml) with
`installer/linux/build-linux.sh`; the full build log is public on the repository's **Actions** page. Check the hashes
with `sha256sum -c SHA256SUMS-linux.txt`.

- Neither needs root. The **AppImage** is the program in one file: run it, delete it to remove it.
- The **tar.gz** is the same program as a folder. Its `install.sh` (a short shell script, read it first) writes only:
  the program folder (by default `~/.local/share/HairHarpy`), a menu entry `~/.local/share/applications/hairharpy.desktop`,
  its icon `~/.local/share/icons/hicolor/256x256/apps/hairharpy.png` and, for opening `.hairmesh` files,
  `~/.local/share/mime/packages/hairharpy.xml`. `uninstall.sh` removes exactly these.
- The program behaves as described in section 5, with Linux places: settings in `~/.config/Catarackta/HairHarpy.conf`;
  projects, presets and autosaves in `~/.local/share/Catarackta/HairHarpy`; RizomUV exchange files in
  `/tmp/HairHarpy`; session recordings in `~/Documents/HairHarpy Sessions`. The Linux build writes no crash dumps.
- To see it yourself: `strace -f -e trace=network` or `ss -tp` show that its only connection is the bridge's
  `127.0.0.1`.

## Questions

If something here doesn't match what you see, please open an issue in this repository. A wrong statement on this page
is a bug, and we'll fix it.
