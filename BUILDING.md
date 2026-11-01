# Building HairHarpy from source

*Коротко по-русски — в конце файла.*

Everything here is free. The build takes about 10 minutes the first time (CMake downloads a few dependencies) and a
couple of minutes after that.

## What you need (Windows 10 / 11, x64)

1. **Visual Studio 2022** (Community is free) with the workload **"Desktop development with C++"**. It brings MSVC,
   CMake and Ninja.
2. **Qt 6.8.3** for **MSVC 2022 64-bit**, from the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss)
   (open-source edition, free account). Pick *Qt 6.8.3 → MSVC 2022 64-bit* and, under *Additional Libraries*, nothing
   else is required. The default place is `C:\Qt\6.8.3\msvc2022_64`; another place is passed with `-QtDir`.
3. **Git** (CMake uses it to fetch ufbx and the Vulkan headers).
4. Optional: the **Vulkan SDK**. Without it CMake downloads the Vulkan headers and the glslang shader compiler (pinned
   versions, the compiler checked by its SHA-256). The SDK is needed only for the validation layers of Debug builds.

## Build the program

In PowerShell, in the repository folder:

```powershell
.\build.ps1 -Package -Publish
```

- `-Publish` builds what we release, into `build\Publish\`. Without it the same program goes into `build\Release\`.
  Both include the session recording (see TRUST.md); `-DHM_DEVTOOLS=OFF` to CMake builds the program without it.
- `-Package` puts the program with everything it needs into `dist\HairHarpy\` (and `dist\HairHarpy.zip`). That folder
  runs on any Windows 10 / 11 x64 with a Vulkan 1.1 graphics driver.
- Another Qt folder: `.\build.ps1 -Package -Publish -QtDir "D:\Qt\6.8.3\msvc2022_64"`.

Run `dist\HairHarpy\HairHarpy.exe`. That's it: this is the same program as in the release, built by you.

## Build the installer (optional)

```powershell
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Publish
```

It builds the program (as above), then compiles the installer with `csc.exe` from the .NET Framework 4 that is part of
Windows — nothing to install. The result is `dist\HairHarpy-<version>-Setup.exe`, plus `dist\SHA256SUMS.txt` and the
portable zip.

## Build on Linux (x86_64)

The release's Linux files are built by [`.github/workflows/linux.yml`](.github/workflows/linux.yml) on Ubuntu 22.04;
the same script builds them on your computer.

1. Qt 6.8.3 **Desktop gcc 64-bit** from the Qt Online Installer (by default `~/Qt/6.8.3/gcc_64`), or with
   [aqtinstall](https://github.com/miurahr/aqtinstall): `aqt install-qt linux desktop 6.8.3 linux_gcc_64`.
2. The packages (Debian / Ubuntu names):
   ```sh
   sudo apt install cmake ninja-build g++ git curl file glslang-tools libgl1-mesa-dev libegl1-mesa-dev \
     libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
     libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libfontconfig1
   ```
3. In the repository folder:
   ```sh
   bash installer/linux/build-linux.sh --qt ~/Qt/6.8.3/gcc_64
   ```
   It builds the program into `build/Linux/`, bundles Qt with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
   (downloaded into `build/linux-tools/`) and writes `dist/HairHarpy-<version>-x86_64.AppImage`,
   `dist/HairHarpy-<version>-linux-x86_64.tar.gz` and `dist/SHA256SUMS-linux.txt`.

Just the program, without packaging: `cmake -S . -B build/Linux -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_PREFIX_PATH=~/Qt/6.8.3/gcc_64 && cmake --build build/Linux`, then run `build/Linux/HairHarpy`. A Vulkan 1.1
driver is needed (Mesa's RADV / ANV, or NVIDIA's).

## Comparing with our release

C++ builds are not bit-for-bit reproducible (the compiler writes timestamps and paths), so your `HairHarpy.exe` will not
have the same hash as ours. What you can compare:
- the behaviour — it's the same program;
- the files in the program folder: the same names; the Qt and MSVC runtime DLLs are the official ones from Qt and
  Microsoft (they are signed by them — check *Properties → Digital Signatures*);
- the installer: `installer/src/*.cs` is all of its code, and its payload is a plain zip of the program folder.

## Layout

| Folder | What |
|---|---|
| `src/scene` | the scene: curves, walls, cocoons, the topology mesh, UV |
| `src/view` | the viewport: Vulkan rendering, tools, input |
| `src/app` | windows and panels |
| `src/io` | projects, import, export |
| `src/ui` | shared interface pieces |
| `shaders` | GLSL, compiled to SPIR-V at build time |
| `resources` | images, the FAQ, the Blender / Maya bridge add-ons |
| `installer` | the installer and the release scripts |
| `tools` | console checks (`hm_selftest`, `hm_uvtest`) |

---

## Коротко по-русски

1. Установите Visual Studio 2022 (Community) с «Разработкой классических приложений на C++», Qt 6.8.3 для
   MSVC 2022 64-bit (`C:\Qt\6.8.3\msvc2022_64`) и Git.
2. В папке репозитория в PowerShell: `.\build.ps1 -Package -Publish`.
3. Запустите `dist\HairHarpy\HairHarpy.exe`. Это та же программа, что и в релизе, только собранная вами.

Установщик (не обязательно): `powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Publish`.

Linux: Qt 6.8.3 gcc 64-bit, пакеты из раздела «Build on Linux», затем
`bash installer/linux/build-linux.sh --qt ~/Qt/6.8.3/gcc_64` — в `dist/` появятся AppImage, tar.gz и хеши.
