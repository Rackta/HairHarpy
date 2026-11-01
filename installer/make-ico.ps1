# PNG (any size, square) -> multi-size .ico (PNG-compressed entries 16..256 px).
# Usage: make-ico.ps1 -Png <in.png> -Ico <out.ico>
param([Parameter(Mandatory)][string]$Png, [Parameter(Mandatory)][string]$Ico)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [Drawing.Image]::FromFile($Png)
$sizes = 256, 128, 64, 48, 32, 24, 16
$blobs = foreach ($s in $sizes) {
    $bmp = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$src.Dispose()
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $b = $blobs[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$b.Length); $w.Write([UInt32]$offset)
    $offset += $b.Length
}
foreach ($b in $blobs) { $w.Write($b) }
$w.Flush()
[IO.File]::WriteAllBytes($Ico, $out.ToArray())
Write-Host "ico: $Ico ($($out.Length) bytes)"
