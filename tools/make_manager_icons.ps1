# Draws the Paint room manager window icons (pm_*) in the same style as make_paint_icons.ps1.
# the same as tools\make_canvas_icons.ps1: a dark rounded tile, light-grey strokes, the purple accent. Drawn at 512 px, saved at 128 px.
# Run: powershell -File tools\make_paint_icons.ps1   -> resources\images\pt_*.png
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'resources\images'
$grey = [System.Drawing.Color]::FromArgb(255, 207, 207, 207)
$dim = [System.Drawing.Color]::FromArgb(255, 120, 120, 120)
$orange = [System.Drawing.Color]::FromArgb(255, 148, 102, 190)   # (the accent: the drawing tools' purple)
$brown = [System.Drawing.Color]::FromArgb(255, 60, 42, 82)
$bg = [System.Drawing.Color]::FromArgb(255, 32, 32, 32)
$S = 512

function New-Canvas {
  $bmp = New-Object System.Drawing.Bitmap $S, $S
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $r = 150
  $path.AddArc(0, 0, $r, $r, 180, 90); $path.AddArc($S - $r, 0, $r, $r, 270, 90)
  $path.AddArc($S - $r, $S - $r, $r, $r, 0, 90); $path.AddArc(0, $S - $r, $r, $r, 90, 90); $path.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush $bg), $path)
  return @($bmp, $g)
}
function Pen($c, $w = 20) { $p = New-Object System.Drawing.Pen $c, $w; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p.LineJoin = 'Round'; return $p }
function Dash($c, $w = 18) { $p = Pen $c $w; $p.DashStyle = 'Custom'; $p.DashPattern = @(1.6, 1.6); $p.DashCap = 'Round'; return $p }
function Br($c) { return New-Object System.Drawing.SolidBrush $c }
function Pt($x, $y) { return New-Object System.Drawing.PointF $x, $y }
function Pts($a) { $o = @(); for ($i = 0; $i -lt $a.Count; $i += 2) { $o += (Pt $a[$i] $a[$i + 1]) }; return [System.Drawing.PointF[]]$o }
function Moved($a, $dx, $dy) { $o = @(); for ($i = 0; $i -lt $a.Count; $i += 2) { $o += ($a[$i] + $dx); $o += ($a[$i + 1] + $dy) }; return (Pts $o) }
function Save($c, $name) {
  $c[1].Dispose()
  $small = New-Object System.Drawing.Bitmap 128, 128
  $g = [System.Drawing.Graphics]::FromImage($small); $g.InterpolationMode = 'HighQualityBicubic'; $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent); $g.DrawImage($c[0], 0, 0, 128, 128); $g.Dispose()
  $small.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png); $small.Dispose(); $c[0].Dispose()
}
function Arrow($g, $pen, $x1, $y1, $x2, $y2, $head = 46) {   # a line with an arrowhead at (x2, y2)
  $g.DrawLine($pen, $x1, $y1, $x2, $y2)
  $a = [Math]::Atan2($y2 - $y1, $x2 - $x1)
  foreach ($d in @(2.5, -2.5)) { $g.DrawLine($pen, $x2, $y2, $x2 - $head * [Math]::Cos($a + $d / 3), $y2 - $head * [Math]::Sin($a + $d / 3)) }
}
function ArcArrow($g, $pen, $cx, $cy, $r, $start, $sweep, $head = 48) {   # an arc with an arrowhead where it ends (sweep under 0: anticlockwise)
  $g.DrawArc($pen, [single]($cx - $r), [single]($cy - $r), [single](2 * $r), [single](2 * $r), [single]$start, [single]$sweep)
  $e = ($start + $sweep) * [Math]::PI / 180; $x = $cx + $r * [Math]::Cos($e); $y = $cy + $r * [Math]::Sin($e)
  $s = 1; if ($sweep -lt 0) { $s = -1 }
  $t = [Math]::Atan2($s * [Math]::Cos($e), - $s * [Math]::Sin($e))   # (the way the arc runs at its end)
  foreach ($d in @(0.55, -0.55)) { $g.DrawLine($pen, [single]$x, [single]$y, [single]($x - $head * [Math]::Cos($t + $d)), [single]($y - $head * [Math]::Sin($t + $d))) }
}
function Plus($g, $cx, $cy, $r = 46, $w = 24) { $p = Pen $orange $w; $g.DrawLine($p, $cx - $r, $cy, $cx + $r, $cy); $g.DrawLine($p, $cx, $cy - $r, $cx, $cy + $r) }
function Sheet($g, $pen, $cy, $fill = $null) {   # a layer seen at an angle: a flat diamond
  $p = Pts @(256, ($cy - 80), 420, $cy, 256, ($cy + 80), 92, $cy)
  if ($fill) { $g.FillPolygon((Br $fill), $p) }
  $g.DrawPolygon($pen, $p)
}
function Page($g, $pen, $x, $y, $w, $h, $fill = $null) {   # a page with a folded corner
  $f = [Math]::Min($w, $h) * 0.28
  $p = Pts @($x, $y, ($x + $w - $f), $y, ($x + $w), ($y + $f), ($x + $w), ($y + $h), $x, ($y + $h))
  if ($fill) { $g.FillPolygon((Br $fill), $p) }
  $g.DrawPolygon($pen, $p); $g.DrawLines($pen, (Pts @(($x + $w - $f), $y, ($x + $w - $f), ($y + $f), ($x + $w), ($y + $f))))
}

# 2D: the texture — a flat sheet with a UV island on it
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 22), 100, 100, 312, 312)
$isl = Pts @(170, 340, 196, 196, 300, 160, 350, 250, 290, 350)
$g.FillClosedCurve((Br $orange), $isl, 'Winding', 0.4); $g.DrawClosedCurve((Pen $grey 12), $isl, 0.4, 'Winding')
Save $c 'pm_tex2d.png'

# the model: a head with a lock of hair over it
$c = New-Canvas; $g = $c[1]
$g.DrawEllipse((Pen $grey 22), 150, 130, 212, 252)
$p = New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddBezier(128, 290, 120, 120, 300, 70, 392, 180); $p.AddBezier(392, 180, 300, 160, 230, 190, 196, 250); $p.AddBezier(196, 250, 186, 300, 170, 340, 128, 410); $p.CloseFigure()
$g.FillPath((Br $orange), $p); $g.DrawPath((Pen $grey 12), $p)
Save $c 'pm_model.png'

# hide / show: an eye half crossed out
$c = New-Canvas; $g = $c[1]
$p = New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddBezier(80, 256, 170, 130, 342, 130, 432, 256); $p.AddBezier(432, 256, 342, 382, 170, 382, 80, 256); $p.CloseFigure()
$g.DrawPath((Pen $grey 22), $p); $g.FillEllipse((Br $grey), 206, 206, 100, 100)
$g.DrawLine((Pen $bg 52), 120, 420, 400, 96); $g.DrawLine((Pen $orange 26), 120, 420, 400, 96)
Save $c 'pm_vis.png'

# the visibility's undo / redo: an arrow round an eye
foreach ($dir in @('undo', 'redo')) {
  $c = New-Canvas; $g = $c[1]
  if ($dir -eq 'undo') { ArcArrow $g (Pen $orange 30) 256 280 150 40 -240 70 } else { ArcArrow $g (Pen $orange 30) 256 280 150 140 240 70 }
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $p.AddBezier(176, 270, 210, 220, 302, 220, 336, 270); $p.AddBezier(336, 270, 302, 320, 210, 320, 176, 270); $p.CloseFigure()
  $g.DrawPath((Pen $grey 16), $p); $g.FillEllipse((Br $grey), 232, 246, 48, 48)
  Save $c ("pm_$dir.png")
}
# the manager as a panel of its own over the viewport, bottom right — or back into the left column: a screen, a panel in its corner
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 20), 70, 96, 372, 300)
$g.FillRectangle((Br $orange), 262, 246, 150, 124); $g.DrawRectangle((Pen $grey 12), 262, 246, 150, 124)
Arrow $g (Pen $grey 22) 130 150 236 224 44
Save $c 'pm_float.png'
Write-Output "icons written to $out"
