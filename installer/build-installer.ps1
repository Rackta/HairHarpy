# Builds "Native\dist\HairHarpy-<version>-Setup.exe": the native program (build.ps1 -Package — the exe,
# Qt, the MSVC runtime) zipped into Setup.exe with the uninstaller, the icons (<Hair_Modeller>\Icons) and the example
# projects (Native\Examples\*.hairmesh, if any). Setup is C# compiled by the .NET Framework's csc (part of Windows).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File build-installer.ps1 [-Version 2.0.0] [-SkipBuild] [-Publish] [-Install]
# (-Install: then update the installation on this PC — <Hair_Modeller>\HairHarpy — silently)
param([string]$Version, [switch]$SkipBuild, [switch]$Publish, [switch]$Install)   # (-Publish: the build for publishing, plus the portable zip, the hashes and the symbols)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$native = (Resolve-Path (Join-Path $here '..')).Path
$R = (Resolve-Path (Join-Path $native '..')).Path                 # Hair_Modeller
$name = 'HairHarpy'
$verFile = Join-Path $here 'version.txt'
if ($Version) { Set-Content $verFile $Version -Encoding ASCII }
if (-not (Test-Path $verFile)) { Set-Content $verFile '2.0.0' -Encoding ASCII }
$ver = (Get-Content $verFile -TotalCount 1).Trim()
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

# the program's folder
if (-not $SkipBuild) { if ($Publish) { & (Join-Path $native 'build.ps1') -Package -Publish } else { & (Join-Path $native 'build.ps1') -Package }; if ($LASTEXITCODE) { throw 'build failed' } }
$dist = Join-Path $native 'dist\HairHarpy'
if (-not (Test-Path (Join-Path $dist 'HairHarpy.exe'))) { throw "No program in $dist (run build.ps1 -Package)" }

$src = Join-Path $here 'src'
$obj = Join-Path $here 'obj'
if (Test-Path $obj) { Remove-Item $obj -Recurse -Force }
New-Item -ItemType Directory $obj | Out-Null
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath (Join-Path $obj 'program.zip') -CompressionLevel Optimal

# icons + logo (installer\icons — copies of <Hair_Modeller>\Icons, so the published source builds on its own)
$icons = Join-Path $here 'icons'
& (Join-Path $here 'make-ico.ps1') -Png (Join-Path $icons 'HarpyIco.png') -Ico (Join-Path $obj 'app.ico')
& (Join-Path $here 'make-ico.ps1') -Png (Join-Path $icons 'HarpyIcoUninstal.png') -Ico (Join-Path $obj 'uninstall.ico')
Add-Type -AssemblyName System.Drawing
$img = [Drawing.Image]::FromFile((Join-Path $icons 'HarpyIco.png'))
$bmp = New-Object Drawing.Bitmap 192, 192
$g = [Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'; $g.DrawImage($img, 0, 0, 192, 192); $g.Dispose()
$bmp.Save((Join-Path $obj 'logo.png'), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); $img.Dispose()

Set-Content (Join-Path $obj 'BuildInfo.cs') "static class BuildInfo { public const string Version = `"$ver`"; public const string Date = `"$(Get-Date -Format yyyy-MM-dd)`"; }" -Encoding UTF8

$common = @('/nologo', '/target:winexe', '/optimize+', '/platform:anycpu', "/win32manifest:$src\app.manifest",
    '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:Microsoft.CSharp.dll', '/r:System.Core.dll', '/r:System.IO.Compression.dll')
function Compile($out, $ico, $files, $extra) {
    & $csc @common "/win32icon:$ico" "/out:$out" @extra @files
    if ($LASTEXITCODE -ne 0) { throw "csc failed: $out" }
}
Compile "$obj\Uninstall.exe" "$obj\uninstall.ico" @("$src\Common.cs", "$src\Uninstall.cs") @()
$res = @("/resource:$obj\program.zip,payload.program.zip", "/resource:$obj\Uninstall.exe,payload.Uninstall.exe",
    "/resource:$obj\app.ico,payload.app.ico", "/resource:$obj\uninstall.ico,payload.uninstall.ico", "/resource:$obj\logo.png,payload.logo.png")
# example projects: Native\Examples (Setup puts them into System\Examples — the File window's Examples)
$exDir = Join-Path $native 'Examples'
if (Test-Path $exDir) { Get-ChildItem $exDir -File -Filter *.hairmesh | ForEach-Object { $res += "/resource:$($_.FullName),payload.example.$($_.Name)" } }
New-Item -ItemType Directory -Force (Join-Path $native 'dist') | Out-Null
$setup = Join-Path $native "dist\$name-$ver-Setup.exe"   # (no spaces: GitHub turns them into dots in release files)
Compile $setup "$obj\app.ico" @("$src\Common.cs", "$src\Setup.cs", "$obj\BuildInfo.cs") $res
Write-Host "Built: $setup ($([math]::Round((Get-Item $setup).Length/1MB,1)) MB, version $ver, examples: $(@($res | Where-Object { $_ -like '*payload.example.*' }).Count))"
if ($Publish) {
    # the release's other files: the program without an installer, and the hashes to check the downloads against (TRUST.md)
    $portable = Join-Path $native "dist\$name-$ver-portable.zip"
    Copy-Item (Join-Path $native 'dist\HairHarpy.zip') $portable -Force
    $sums = Join-Path $native 'dist\SHA256SUMS.txt'
    @($setup, $portable) | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_ -Algorithm SHA256).Hash.ToLower(), (Split-Path $_ -Leaf) } | Set-Content $sums -Encoding ASCII
    # the build's HairHarpy.pdb/.map, kept (not released): a crash dump a user sends is read against them
    $sym = Join-Path $R "Release V2\v$ver\Symbols"
    New-Item -ItemType Directory -Force $sym | Out-Null
    foreach ($f in 'HairHarpy.pdb', 'HairHarpy.map') { $p = Join-Path $native "build\Publish\$f"; if (Test-Path $p) { Copy-Item $p $sym -Force } else { Write-Warning "missing $p" } }
    Write-Host "Release files: $portable, $sums; symbols: $sym"
}
if ($Install) {
    if (Get-Process HairHarpy, HairMeshCreator -ErrorAction SilentlyContinue) { throw 'Close the program first: it is running' }
    $dir = Join-Path $R $name
    # (the installation under the name before 2.0: its folder renamed — projects, presets, autosaves move with it)
    $old = Join-Path $R 'Stylized HairMesh Creator'
    if (-not (Test-Path $dir) -and (Test-Path $old)) { Rename-Item -LiteralPath $old -NewName $name; Write-Host "Renamed: $old -> $dir" }
    $p = Start-Process $setup -ArgumentList '--silent', '--no-desktop', '--dir', "`"$dir`"" -Wait -PassThru
    if ($p.ExitCode) { throw "Setup failed ($($p.ExitCode))" }
    Write-Host "Installed: $dir"
}
