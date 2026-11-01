# Builds the native version: MSVC environment (vcvars64) + CMake + Ninja.
#   .\build.ps1            — Release into build\Release
#   .\build.ps1 -Debug     — Debug (Vulkan validation layers on) into build\Debug
#   .\build.ps1 -Run       — and start it (optionally with -Project <file.hairmesh>)
#   .\build.ps1 -Package   — Release + dist\HairHarpy (everything a clean Windows needs to run it) + a .zip
#   .\build.ps1 -Publish   — with -Package: the build for publishing, into build\Publish (with the session recording, as every build)
param([switch]$Debug, [switch]$Run, [switch]$Package, [switch]$Publish, [string]$Project = "", [string]$QtDir = "C:\Qt\6.8.3\msvc2022_64")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cfg = if ($Debug) { "Debug" } else { "Release" }
$bld = Join-Path $root "build\$cfg"
if ($Publish) { $bld = Join-Path $root "build\Publish" }
$devTools = "ON"   # (the session recording ships in the release too, since 2.0.0: TRUST.md describes it)

# the MSVC environment, once per shell
if (-not $env:VCToolsInstallDir) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  # (-requires misses some installs: any instance with MSVC inside will do)
  $vs = & $vswhere -all -products * -property installationPath | Where-Object { Test-Path (Join-Path $_ "VC\Tools\MSVC") } | Select-Object -First 1
  if (-not $vs) { throw "Visual Studio with C++ tools not found" }
  # (vcvars64.bat isn't always installed; VsDevCmd.bat is)
  $dev = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
  $env:Path = (Split-Path $vswhere) + ";" + $env:Path               # (VsDevCmd calls vswhere by name)
  cmd /c "`"$dev`" -arch=x64 -host_arch=x64 -no_logo >nul 2>nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
}
# (the Vulkan SDK is optional: without it CMake fetches the headers and a shader compiler)
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine') }

cmake -S $root -B $bld -G Ninja "-DCMAKE_BUILD_TYPE=$cfg" "-DCMAKE_PREFIX_PATH=$QtDir" "-DHM_DEVTOOLS=$devTools"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $bld
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if ($Package -and -not $Debug) {
  # the program's folder for other users: the exe, Qt (windeployqt), the MSVC runtime app-local (no VC++ Redistributable
  # to install), nothing of the build. The Vulkan loader is the graphics driver's.
  $dist = Join-Path $root "dist\HairHarpy"
  if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
  New-Item -ItemType Directory -Force $dist | Out-Null
  Copy-Item (Join-Path $bld "HairHarpy.exe") $dist
  & (Join-Path $QtDir "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler `
    --no-opengl-sw --no-compiler-runtime --skip-plugin-types tls,networkinformation,generic (Join-Path $dist "HairHarpy.exe") | Out-Null
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
  $crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
  if (-not $crt) { throw "MSVC runtime not found in $env:VCToolsRedistDir" }
  Copy-Item (Join-Path $crt.FullName "*.dll") $dist
  # the license, the third-party notices (Qt's LGPL asks for them beside its DLLs), what the program does — public\ here,
  # the root in the published source
  foreach ($f in "LICENSE.md", "THIRD_PARTY_NOTICES.md", "TRUST.md", "TRUST.ru.md") {
    $p = @((Join-Path $root "public\$f"), (Join-Path $root $f)) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($p) { Copy-Item $p $dist } }
  $zip = Join-Path $root "dist\HairHarpy.zip"
  if (Test-Path $zip) { Remove-Item $zip }
  Compress-Archive -Path $dist -DestinationPath $zip
  Write-Host "Package: $dist ($([math]::Round(((Get-ChildItem $dist -Recurse -File | Measure-Object Length -Sum).Sum)/1MB,1)) MB), $zip"
}

if ($Run) {
  $exe = Join-Path $bld "HairHarpy.exe"
  if ($Project) { Start-Process $exe -ArgumentList "`"$Project`"" } else { Start-Process $exe }
}
