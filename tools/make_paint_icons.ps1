# Draws the Paint room's icons (its tools, the drawing modes, the tool settings' and the layers' commands) in the installed style —
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
function BrushShape($g, $ox, $oy, $k) {   # the paint brush: a handle, the metal band, the bristles (the accent)
  $g.ResetTransform(); $g.TranslateTransform($ox, $oy); $g.ScaleTransform($k, $k)
  $g.FillPolygon((Br $grey), (Pts @(397, 77, 423, 103, 294, 243, 257, 206))); $g.FillEllipse((Br $grey), 392, 72, 36, 36)
  $g.FillPolygon((Br $bg), (Pts @(240, 192, 310, 262, 302, 270, 232, 200)))
  $g.FillPolygon((Br $grey), (Pts @(232, 200, 302, 270, 260, 312, 190, 242)))
  $tip = Pts @(196, 248, 256, 308, 228, 342, 126, 380, 162, 278)
  $g.FillClosedCurve((Br $orange), $tip, 'Winding', 0.3); $g.DrawClosedCurve((Pen $grey 12), $tip, 0.3, 'Winding')
  $g.ResetTransform()
}

# ================= the tools =================
# Blur: a drop
$c = New-Canvas; $g = $c[1]
$p = New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddBezier(256, 84, 256, 170, 376, 210, 376, 300); $p.AddArc(136, 180, 240, 240, 0, 180); $p.AddBezier(136, 300, 136, 210, 256, 170, 256, 84); $p.CloseFigure()
$g.FillPath((Br $orange), $p); $g.DrawPath((Pen $grey 20), $p)
$g.DrawArc((Pen $bg 18), 188, 232, 136, 136, 105, 60)
Save $c 'pt_blur.png'

# Smudge: a finger and the paint it pulls
$c = New-Canvas; $g = $c[1]
$g.DrawCurve((Pen $orange 50), (Pts @(268, 290, 214, 356, 108, 392)), 0.5)
$g.DrawCurve((Pen $brown 22), (Pts @(322, 318, 268, 398, 176, 424)), 0.5)
$g.DrawLine((Pen $grey 84), 370, 116, 290, 232)
$g.DrawArc((Pen $bg 12), 262, 196, 60, 60, 20, 130)
Save $c 'pt_smudge.png'

# Recolor: the brush and a disc of two tones
$c = New-Canvas; $g = $c[1]
BrushShape $g 10 -6 0.78
$g.FillPie((Br $grey), 262, 262, 176, 176, 90, 180); $g.FillPie((Br $orange), 262, 262, 176, 176, 270, 180)
$g.DrawEllipse((Pen $grey 12), 262, 262, 176, 176)
Save $c 'pt_recolor.png'

# Piece: the place it was taken from and the piece carried away
$c = New-Canvas; $g = $c[1]
$blob = @(0, -84, 82, -52, 92, 40, 10, 92, -84, 52, -92, -30)
$g.DrawClosedCurve((Dash $dim 16), (Moved $blob 196 196), 0.5, 'Winding')
$q = Moved $blob 316 316
$g.FillClosedCurve((Br $bg), $q, 'Winding', 0.5); $g.FillClosedCurve((Br $orange), $q, 'Winding', 0.5); $g.DrawClosedCurve((Pen $grey 16), $q, 0.5, 'Winding')
Save $c 'pt_piece.png'

# Text
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Pen $grey 44), 128, 148, 368, 148); $g.DrawLine((Pen $grey 44), 248, 148, 248, 380); $g.DrawLine((Pen $grey 30), 200, 384, 296, 384)
$g.DrawLine((Pen $orange 24), 410, 236, 410, 392)
Save $c 'pt_text.png'

# Topological symmetry: two strips of polygons across the middle line, a twin marked on each
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Dash $dim 14), 256, 84, 256, 428)
foreach ($m in @(0, 1)) {
  $a = @(104, 196, 214, 150, 214, 256, 104, 256); $b = @(104, 196, 214, 150, 214, 362, 104, 316)
  if ($m) { for ($i = 0; $i -lt 8; $i += 2) { $a[$i] = 512 - $a[$i]; $b[$i] = 512 - $b[$i] } }
  $g.FillPolygon((Br $orange), (Pts $a)); $g.DrawPolygon((Pen $grey 16), (Pts $b))
  $g.DrawLine((Pen $grey 16), $a[6], 256, $a[4], 256)
}
Save $c 'pt_toposym.png'

# Freeze mask: a snowflake
$c = New-Canvas; $g = $c[1]
for ($i = 0; $i -lt 6; $i++) {
  $a = $i * [Math]::PI / 3 + [Math]::PI / 2
  $bx = 256 + 104 * [Math]::Cos($a); $by = 256 + 104 * [Math]::Sin($a)
  foreach ($d in @(1, -1)) { $b = $a + $d * [Math]::PI / 3.4; $g.DrawLine((Pen $orange 20), [single]$bx, [single]$by, [single]($bx + 58 * [Math]::Cos($b)), [single]($by + 58 * [Math]::Sin($b))) }
  $g.DrawLine((Pen $grey 22), 256, 256, [single](256 + 168 * [Math]::Cos($a)), [single](256 + 168 * [Math]::Sin($a)))
}
Save $c 'pt_freeze.png'

# The view: its roll (one button — drag either way, a click sets it upright), the camera reset
$c = New-Canvas; $g = $c[1]
$g.TranslateTransform(256, 274); $g.RotateTransform(14); $g.DrawRectangle((Pen $grey 20), -104, -72, 208, 144); $g.ResetTransform()
ArcArrow $g (Pen $orange 24) 256 274 180 270 56 46; ArcArrow $g (Pen $orange 24) 256 274 180 270 -56 46
Save $c 'pt_view_roll.png'
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 20), 104, 186, 304, 206); $g.DrawLines((Pen $grey 20), (Pts @(196, 186, 220, 140, 292, 140, 316, 186)))
ArcArrow $g (Pen $orange 22) 256 290 58 290 290 36
Save $c 'pt_view_reset.png'

# ================= the drawing modes =================
$c = New-Canvas; $g = $c[1]
$g.DrawCurve((Pen $orange 36), (Pts @(96, 340, 176, 196, 262, 300, 336, 176, 416, 250)), 0.5)
$g.FillEllipse((Br $grey), 384, 218, 64, 64)
Save $c 'pt_mode_stroke.png'

$c = New-Canvas; $g = $c[1]
$pl = @(106, 384, 196, 170, 316, 330, 406, 136)
$g.DrawLines((Pen $orange 30), (Pts $pl))
for ($i = 0; $i -lt 8; $i += 2) { $g.FillRectangle((Br $grey), $pl[$i] - 26, $pl[$i + 1] - 26, 52, 52) }
Save $c 'pt_mode_line.png'

$c = New-Canvas; $g = $c[1]
$q = Pts @(150, 150, 300, 110, 410, 190, 384, 322, 250, 392, 130, 330, 104, 230)
$g.FillClosedCurve((Br $orange), $q, 'Winding', 0.55); $g.DrawClosedCurve((Dash $grey 18), $q, 0.55, 'Winding')
Save $c 'pt_mode_lasso.png'

$c = New-Canvas; $g = $c[1]; $g.FillRectangle((Br $orange), 120, 150, 272, 212); $g.DrawRectangle((Dash $grey 18), 120, 150, 272, 212); Save $c 'pt_mode_rect.png'
$c = New-Canvas; $g = $c[1]; $g.FillEllipse((Br $orange), 104, 140, 304, 232); $g.DrawEllipse((Dash $grey 18), 104, 140, 304, 232); Save $c 'pt_mode_ellipse.png'

# Polygons: the mesh's own polygons, the ones passed over painted whole
$c = New-Canvas; $g = $c[1]
foreach ($q in @(@(0, 2), @(1, 1), @(2, 1), @(2, 0))) { $g.FillRectangle((Br $orange), 100 + 104 * $q[0], 100 + 104 * $q[1], 104, 104) }
for ($i = 0; $i -lt 4; $i++) { $v = 100 + 104 * $i; $g.DrawLine((Pen $grey 14), $v, 100, $v, 412); $g.DrawLine((Pen $grey 14), 100, $v, 412, $v) }
Save $c 'pt_mode_faces.png'

# ================= the tools' settings =================
$c = New-Canvas; $g = $c[1]
$pl = @(100, 340, 196, 176, 316, 336, 412, 172)
$g.DrawCurve((Pen $grey 28), (Pts $pl), 0.6)
for ($i = 0; $i -lt 8; $i += 2) { $g.FillEllipse((Br $orange), $pl[$i] - 32, $pl[$i + 1] - 32, 64, 64) }
Save $c 'pt_smooth.png'

# Flat: the line as it lies on the screen
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 24), 96, 112, 320, 216); $g.DrawLine((Pen $grey 24), 256, 328, 256, 396); $g.DrawLine((Pen $grey 24), 176, 398, 336, 398)
$g.DrawLine((Pen $orange 30), 152, 274, 360, 166)
Save $c 'pt_flat.png'

$c = New-Canvas; $g = $c[1]; $g.DrawLines((Pen $orange 48), (Pts @(124, 268, 222, 366, 392, 160))); Save $c 'pt_ok.png'
$c = New-Canvas; $g = $c[1]; $p = Pen $grey 44; $g.DrawLine($p, 152, 152, 360, 360); $g.DrawLine($p, 360, 152, 152, 360); Save $c 'pt_cancel.png'

# Through: the near wall and the far one, the region going through both
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Pen $grey 30), 186, 116, 186, 396); $g.DrawLine((Pen $dim 30), 326, 116, 326, 396)
Arrow $g (Pen $orange 28) 76 256 432 256 56
Save $c 'pt_through.png'

$c = New-Canvas; $g = $c[1]
$lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (Pt 96 0), (Pt 416 0), $orange, $grey
$g.FillRectangle($lg, 104, 150, 304, 212); $g.DrawRectangle((Pen $grey 18), 104, 150, 304, 212)
Save $c 'pt_gradient.png'

# Cut: the scissors
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Pen $grey 30), 196, 318, 338, 104); $g.DrawLine((Pen $grey 30), 316, 318, 174, 104)
$g.DrawEllipse((Pen $orange 26), 108, 300, 108, 108); $g.DrawEllipse((Pen $orange 26), 296, 300, 108, 108)
$g.FillEllipse((Br $bg), 242, 212, 28, 28)
Save $c 'pt_cut.png'


# Stencil: a sheet with shapes cut through it
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 22), 100, 124, 312, 264)
$g.FillEllipse((Br $orange), 150, 176, 100, 100); $g.FillPolygon((Br $orange), (Pts @(318, 196, 372, 330, 250, 330)))
Save $c 'pt_stencil.png'

# Strip: a pattern repeated along the stroke
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Pen $dim 16), 76, 256, 436, 256)
foreach ($x in @(136, 256, 376)) { $g.FillEllipse((Br $bg), $x - 56, 196, 112, 120); $g.DrawEllipse((Pen $orange 26), $x - 46, 206, 92, 100) }
Save $c 'pt_strip.png'

# Across the middle line: the layer carried from one side onto the other
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 20), 92, 186, 120, 140); $g.DrawRectangle((Dash $grey 18), 300, 186, 120, 140)
$g.DrawLine((Dash $dim 14), 256, 96, 256, 416)
Arrow $g (Pen $orange 28) 152 256 352 256 54
Save $c 'pt_transfer.png'

# Show: an eye
$c = New-Canvas; $g = $c[1]
$p = New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddBezier(80, 256, 170, 130, 342, 130, 432, 256); $p.AddBezier(432, 256, 342, 382, 170, 382, 80, 256); $p.CloseFigure()
$g.DrawPath((Pen $grey 22), $p); $g.FillEllipse((Br $orange), 196, 196, 120, 120)
Save $c 'pt_eye.png'

# ================= the freeze mask =================
# To 100 %: a soft mask made solid
$c = New-Canvas; $g = $c[1]
$g.DrawEllipse((Dash $dim 18), 92, 92, 328, 328); $g.FillEllipse((Br $orange), 152, 152, 208, 208); $g.DrawEllipse((Pen $grey 20), 152, 152, 208, 208)
Save $c 'pt_solid.png'

# Hard brush: a tip with a hard edge
$c = New-Canvas; $g = $c[1]
$g.FillEllipse((Br $grey), 168, 168, 176, 176); $g.DrawEllipse((Pen $orange 18), 132, 132, 248, 248)
$p = Pen $dim 20; $g.DrawLine($p, 256, 70, 256, 104); $g.DrawLine($p, 256, 408, 256, 442); $g.DrawLine($p, 70, 256, 104, 256); $g.DrawLine($p, 408, 256, 442, 256)
Save $c 'pt_hardtip.png'

# From layer: what the layer has painted, taken into the mask
$c = New-Canvas; $g = $c[1]
$g.TranslateTransform(256, 256); $g.ScaleTransform(1.18, 1.18); $g.TranslateTransform(-256, -256)
Sheet $g (Pen $grey 18) 256 $bg; $g.FillEllipse((Br $orange), 190, 222, 132, 68)
$g.ResetTransform()
Save $c 'pt_from_layer.png'

# An adjustment in the region: the two halves of a disc
$c = New-Canvas; $g = $c[1]
$g.FillPie((Br $orange), 116, 116, 280, 280, 270, 180); $g.DrawEllipse((Pen $grey 22), 116, 116, 280, 280); $g.DrawLine((Pen $grey 16), 256, 116, 256, 396)
Save $c 'pt_adjust.png'

# Keep the mask: a bookmark
$c = New-Canvas; $g = $c[1]
$q = Pts @(164, 96, 348, 96, 348, 416, 256, 340, 164, 416)
$g.FillPolygon((Br $brown), $q); $g.DrawPolygon((Pen $grey 22), $q); Plus $g 256 210 50 26
Save $c 'pt_keep.png'

# ================= the layers =================
# Smart rename: a name tag with a spark
$c = New-Canvas; $g = $c[1]
$g.DrawPolygon((Pen $grey 20), (Pts @(92, 170, 310, 170, 420, 256, 310, 342, 92, 342)))
$star = @(); for ($k = 0; $k -lt 8; $k++) { $r = 26; if ($k % 2 -eq 0) { $r = 74 }; $an = - [Math]::PI / 2 + $k * [Math]::PI / 4; $star += (Pt (204 + $r * [Math]::Cos($an)) (256 + $r * [Math]::Sin($an))) }
$g.FillPolygon((Br $orange), [System.Drawing.PointF[]]$star)
$g.FillEllipse((Br $grey), 322, 238, 36, 36)
Save $c 'pt_smartname.png'

# Rename: a name tag with the text cursor in it
$c = New-Canvas; $g = $c[1]
$g.DrawPolygon((Pen $grey 20), (Pts @(92, 170, 310, 170, 420, 256, 310, 342, 92, 342)))
$p = Pen $orange 24; $g.DrawLine($p, 210, 214, 210, 298); $g.DrawLine($p, 178, 206, 242, 206); $g.DrawLine($p, 178, 306, 242, 306)
Save $c 'pt_rename.png'

# Favourite: a star
$c = New-Canvas; $g = $c[1]
$star = @(); for ($k = 0; $k -lt 10; $k++) { $r = 72; if ($k % 2 -eq 0) { $r = 170 }; $an = - [Math]::PI / 2 + $k * [Math]::PI / 5; $star += (Pt (256 + $r * [Math]::Cos($an)) (268 + $r * [Math]::Sin($an))) }
$g.FillPolygon((Br $orange), [System.Drawing.PointF[]]$star); $g.DrawPolygon((Pen $grey 16), [System.Drawing.PointF[]]$star)
Save $c 'pt_star.png'

# Light and shade: a sun, half lit
$c = New-Canvas; $g = $c[1]
$g.FillPie((Br $orange), 176, 176, 160, 160, 90, 180); $g.FillPie((Br $brown), 176, 176, 160, 160, 270, 180); $g.DrawEllipse((Pen $grey 16), 176, 176, 160, 160)
for ($k = 0; $k -lt 8; $k++) { $an = $k * [Math]::PI / 4; $g.DrawLine((Pen $grey 24), [single](256 + 124 * [Math]::Cos($an)), [single](256 + 124 * [Math]::Sin($an)), [single](256 + 170 * [Math]::Cos($an)), [single](256 + 170 * [Math]::Sin($an))) }
Save $c 'pt_light.png'

# All layers as one file: a page with the layers in it
$c = New-Canvas; $g = $c[1]
Page $g (Pen $grey 20) 136 84 240 344 $bg
foreach ($y in @(240, 298, 356)) { $g.DrawLine((Pen $orange 24), 184, $y, 328, $y) }
Save $c 'pt_psd.png'

# ================= every room =================
# More: the commands used rarely, in a menu behind this button (ui::moreButton)
$c = New-Canvas; $g = $c[1]
foreach ($x in @(136, 256, 376)) { $g.FillEllipse((Br $grey), $x - 38, 218, 76, 76) }
Save $c 'ic_more.png'

Write-Output "icons written to $out"
