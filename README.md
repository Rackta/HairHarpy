# HairHarpy

A Windows and Linux program for modeling voluminous, stylized hairstyles with clean topology. The hair is constructed using curves and NURBS surfaces stretched between them, with mesh density and edge loops controlled via parameters—allowing for the creation of complex hairstyles without the need for time-consuming manual geometry editing.


## Download

Everything is on the [Releases](../../releases) page:

- **Windows**: `HairHarpy-<version>-Setup.exe` (the installer) or `HairHarpy-<version>-portable.zip`, hashes in
  `SHA256SUMS.txt`;
- **Linux**: `HairHarpy-<version>-x86_64.AppImage` (one file: `chmod +x` and run) or
  `HairHarpy-<version>-linux-x86_64.tar.gz` (a folder; `sh install.sh` adds it to the menu), hashes in
  `SHA256SUMS-linux.txt`.

The files are **not code-signed** (the author is one person, and a certificate costs hundreds of dollars a year), so
Windows SmartScreen warns on the first run. **[TRUST.md](TRUST.md)** ([по-русски](TRUST.ru.md)) explains what the
installer and the program do on your computer and how to check every word of it yourself — from comparing a hash to
building the program from this source code.

## Source code

This repository is the complete source of the program and its installer. You may read it, build it and use your build
for any work, including commercial — see [LICENSE.md](LICENSE.md). It is *source-available*, not open source:
redistributing the code or builds needs the author's permission.

- [BUILDING.md](BUILDING.md) — how to build it (free tools, about 10 minutes).
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) — the components it uses and their licenses.

Requirements: Windows 10 / 11 x64, or Linux x86_64 with glibc 2.35+ (Ubuntu 22.04, Fedora 36, Debian 12 and newer) and
X11 or XWayland; a graphics card with a Vulkan 1.1 driver (practically any from the last ten years).

## Author

catarackta — https://catarackta.netlify.app
