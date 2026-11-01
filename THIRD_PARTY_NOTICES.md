# Third-party components

HairHarpy is built with the components below. Each one stays under its own license.

| Component | Version | Used for | License | Source |
|---|---|---|---|---|
| Qt (Core, Gui, Widgets, Network, Svg) | 6.8.3 | the interface, the window, the Vulkan window | LGPL-3.0 | https://download.qt.io/archive/qt/6.8/6.8.3/single/ |
| Eigen | 3.4.0 | the UV solvers (sparse matrices) | MPL-2.0 (built with `EIGEN_MPL2_ONLY`) | https://gitlab.com/libeigen/eigen/-/tree/3.4.0 |
| ufbx | 0.21.5 | FBX import | MIT or public domain (Unlicense), at your choice | https://github.com/ufbx/ufbx |
| Vulkan-Headers | 1.4.363 | the Vulkan API declarations (build time) | Apache-2.0 / MIT | https://github.com/KhronosGroup/Vulkan-Headers |
| glslang | 16.5.0 | compiling the shaders (build time only, not shipped) | BSD-3-Clause and others | https://github.com/KhronosGroup/glslang |
| Microsoft Visual C++ runtime | 14.x | the C++ runtime DLLs next to the program | Microsoft Visual Studio redistributable terms | https://visualstudio.microsoft.com/license-terms/ |

The Vulkan loader (`vulkan-1.dll`, on Linux `libvulkan.so.1`) is not shipped: it comes with your graphics driver.

**Linux packages.** Instead of the Visual C++ runtime they carry, in `usr/lib`, the Qt 6.8.3 gcc 64-bit libraries
(with ICU, under the Unicode License) and the few system libraries Qt's xcb plugin needs that a distribution may lack
(xcb-util, libxkbcommon: MIT / X11 licenses), copied by [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy). The
AppImage starts with the [AppImage runtime](https://github.com/AppImage/type2-runtime) (MIT). The Qt libraries there
may be replaced as described below (Qt 6.8.3, gcc 64-bit).

## Qt and the LGPL-3.0

Qt is linked dynamically: its libraries are the `Qt6*.dll` files and the plugin folders next to `HairHarpy.exe`. You
may replace them with your own build of Qt 6.8.3 (the same version, MSVC 2022 x64). The full source code of that Qt
version is at the link above. The LGPL-3.0 text: https://www.gnu.org/licenses/lgpl-3.0.html (it builds on the
GPL-3.0: https://www.gnu.org/licenses/gpl-3.0.html).

## Eigen and the MPL-2.0

Eigen is used unchanged, as headers. The MPL-2.0 text: https://www.mozilla.org/MPL/2.0/

## ufbx (MIT)

Copyright (c) 2020 Samuli Raivio. Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The
above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Vulkan-Headers

Copyright The Khronos Group Inc. Licensed under the Apache License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0)
or the MIT License, at your choice.
