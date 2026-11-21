# Draws the reference canvas's icons (tools, modes, the reference / layer panel) in the installed style: a dark rounded
# tile, light-grey strokes, a purple accent (the UV checker's). Drawn at 512 px, saved at 128 px.
# Run: powershell -File tools\make_canvas_icons.ps1   -> resources\images\rc_*.png
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'resources\images'
$grey = [System.Drawing.Color]::FromArgb(255, 207, 207, 207)
$dim = [System.Drawing.Color]::FromArgb(255, 120, 120, 120)
$orange = [System.Drawing.Color]::FromArgb(255, 148, 102, 190)   # (the accent: the purple of the UV checker — the drawing tools have their own colour)
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
function BrushShape($g, $ox, $oy, $k, $stroke = $true) {   # the paint brush: a handle, the metal band, the bristles (the accent) and the stroke it leaves
  $g.ResetTransform(); $g.TranslateTransform($ox, $oy); $g.ScaleTransform($k, $k)
  if ($stroke) { $g.DrawCurve((Pen $orange 24), (Pts @(96, 432, 190, 404, 290, 438, 420, 404)), 0.5) }
  $g.FillPolygon((Br $grey), (Pts @(397, 77, 423, 103, 294, 243, 257, 206))); $g.FillEllipse((Br $grey), 392, 72, 36, 36)
  $g.FillPolygon((Br $bg), (Pts @(240, 192, 310, 262, 302, 270, 232, 200)))
  $g.FillPolygon((Br $grey), (Pts @(232, 200, 302, 270, 260, 312, 190, 242)))
  $tip = Pts @(196, 248, 256, 308, 228, 342, 126, 380, 162, 278)
  $g.FillClosedCurve((Br $orange), $tip, 'Winding', 0.3); $g.DrawClosedCurve((Pen $grey 12), $tip, 0.3, 'Winding')
  $g.ResetTransform()
}
function Folder($g, $pen, $x, $y, $w, $h) {
  $p = Pts @($x, ($y + 30), $x, ($y + $h), ($x + $w), ($y + $h), ($x + $w), ($y + 30), ($x + $w * 0.45), ($y + 30), ($x + $w * 0.36), $y, ($x + 16), $y)
  $g.DrawPolygon($pen, $p); $g.DrawLine($pen, $x, ($y + 60), ($x + $w), ($y + 60))
}

# ================= the tools =================
$c = New-Canvas; BrushShape $c[1] 0 0 1.0; Save $c 'rc_brush.png'

$c = New-Canvas; $g = $c[1]
$g.FillPolygon((Br $orange), (Pts @(192, 228, 302, 338, 232, 408, 122, 298)))
$g.DrawPolygon((Pen $grey), (Pts @(310, 118, 420, 228, 232, 408, 122, 298)))
$g.DrawLine((Pen $grey), 192, 228, 302, 338)
$g.DrawLine((Pen $dim 16), 250, 430, 420, 430)
Save $c 'rc_eraser.png'

function Cursor($g, $x, $y, $k) {   # the pointer's arrow, its tip at (x, y)
  $a = Pts @(0, 0, 0, 190, 46, 146, 82, 222, 116, 206, 82, 132, 146, 130)
  for ($i = 0; $i -lt $a.Count; $i++) { $a[$i] = Pt ($x + $a[$i].X * $k) ($y + $a[$i].Y * $k) }
  $g.FillPolygon((Br $grey), $a); $pb = Pen $bg (12 * $k); $pb.LineJoin = 'Round'; $g.DrawPolygon($pb, $a); $g.FillPolygon((Br $grey), $a)
}
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Dash $orange 22), 100, 100, 270, 270)
Cursor $g 236 210 1.0
Save $c 'rc_select.png'


$c = New-Canvas; $g = $c[1]
$b = Pts @(130, 240, 270, 110, 400, 240, 260, 370)
$g.FillPolygon((Br $brown), (Pts @(130, 240, 400, 240, 260, 370)))
$g.DrawPolygon((Pen $grey), $b); $g.DrawLine((Pen $grey 16), 130, 240, 400, 240)
$g.DrawArc((Pen $grey 16), 170, 60, 150, 150, 200, 160)
$g.FillClosedCurve((Br $orange), (Pts @(410, 280, 440, 360, 410, 400, 380, 360)), 'Winding', 0.5)
Save $c 'rc_fill.png'

$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey), 96, 200, 210, 210)
$g.FillEllipse((Br $orange), 220, 100, 196, 196)
$g.DrawEllipse((Pen $grey 16), 220, 100, 196, 196)
Save $c 'rc_shapes.png'

$c = New-Canvas; $g = $c[1]; $pg = Pen $grey 22
foreach ($e in @(@(256, 96), @(256, 416), @(96, 256), @(416, 256))) { Arrow $g $pg 256 256 $e[0] $e[1] 52 }
$g.FillEllipse((Br $orange), 220, 220, 72, 72)
Save $c 'rc_move.png'

$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Dash $grey 18), 130, 150, 252, 252)
foreach ($q in @(@(130, 150), @(382, 150), @(130, 402), @(382, 402))) { $g.FillRectangle((Br $orange), $q[0] - 28, $q[1] - 28, 56, 56) }
$g.DrawLine((Pen $grey 14), 256, 150, 256, 86); $g.FillEllipse((Br $orange), 232, 62, 48, 48)
Save $c 'rc_transform.png'

$c = New-Canvas; $g = $c[1]
$g.DrawLine((Dash $dim 14), 70, 300, 442, 300)
$pin = New-Object System.Drawing.Drawing2D.GraphicsPath
$pin.AddArc(166, 86, 180, 180, 150, 240); $pin.AddLine(334, 220, 256, 330); $pin.AddLine(256, 330, 178, 220); $pin.CloseFigure()
$g.FillPath((Br $orange), $pin); $g.DrawPath((Pen $grey 16), $pin)
$g.FillEllipse((Br $bg), 222, 142, 68, 68)
$g.FillEllipse((Br $grey), 236, 380, 40, 40)
Save $c 'rc_marker.png'

# ================= the selection's modes =================
$c = New-Canvas; $c[1].DrawRectangle((Dash $grey 20), 110, 140, 292, 232); Save $c 'rc_sel_rect.png'
$c = New-Canvas; $c[1].DrawEllipse((Dash $grey 20), 100, 130, 312, 252); Save $c 'rc_sel_ellipse.png'
$c = New-Canvas; $g = $c[1]
$g.DrawClosedCurve((Dash $grey 20), (Pts @(150, 140, 300, 110, 410, 190, 360, 300, 240, 290, 130, 330, 110, 230)), 0.55, 'Winding')
$g.DrawCurve((Pen $orange 18), (Pts @(140, 320, 170, 380, 130, 420)), 0.6)
Save $c 'rc_sel_lasso.png'
$c = New-Canvas; $g = $c[1]
$poly = Pts @(130, 170, 300, 110, 400, 230, 330, 380, 170, 350)
$g.DrawPolygon((Dash $grey 20), $poly)
foreach ($q in $poly) { $g.FillRectangle((Br $orange), $q.X - 22, $q.Y - 22, 44, 44) }
Save $c 'rc_sel_polylasso.png'
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Pen $grey 30), 130, 400, 300, 230)
$star = @(); for ($k = 0; $k -lt 10; $k++) { $r = if ($k % 2 -eq 0) { 92 } else { 38 }; $an = -[Math]::PI / 2 + $k * [Math]::PI / 5; $star += (Pt (340 + $r * [Math]::Cos($an)) (180 + $r * [Math]::Sin($an))) }
$g.FillPolygon((Br $orange), [System.Drawing.PointF[]]$star); $g.DrawPolygon((Pen $grey 10), [System.Drawing.PointF[]]$star)
foreach ($d in @(@(170, 150, 30), @(440, 320, 26), @(220, 250, 20))) { $g.FillEllipse((Br $grey), $d[0] - $d[2] / 2, $d[1] - $d[2] / 2, $d[2], $d[2]) }
Save $c 'rc_sel_wand.png'

$c = New-Canvas; $g = $c[1]; $g.FillRectangle((Br $orange), 150, 150, 212, 212); $g.DrawRectangle((Pen $grey 16), 150, 150, 212, 212); Save $c 'rc_sel_new.png'
$c = New-Canvas; $g = $c[1]
$g.FillRectangle((Br $dim), 100, 100, 200, 200); $g.FillRectangle((Br $orange), 212, 212, 200, 200)
$g.DrawRectangle((Pen $grey 14), 100, 100, 200, 200); $g.DrawRectangle((Pen $grey 14), 212, 212, 200, 200)
$p = Pen $bg 22; $g.DrawLine($p, 312, 312, 380, 312); $g.DrawLine($p, 346, 278, 346, 346)
Save $c 'rc_sel_add.png'
$c = New-Canvas; $g = $c[1]
$g.FillRectangle((Br $orange), 100, 100, 220, 220); $g.FillRectangle((Br $bg), 212, 212, 200, 200)
$g.DrawRectangle((Pen $grey 14), 100, 100, 220, 220); $g.DrawRectangle((Dash $grey 14), 212, 212, 200, 200)
$g.DrawLine((Pen $orange 22), 280, 352, 370, 352)
Save $c 'rc_sel_sub.png'
$c = New-Canvas; $g = $c[1]; $g.FillRectangle((Br $brown), 110, 110, 292, 292); $g.DrawRectangle((Dash $orange 20), 110, 110, 292, 292); Save $c 'rc_sel_all.png'
$c = New-Canvas; $g = $c[1]; $g.DrawRectangle((Dash $grey 18), 110, 110, 292, 292); $p = Pen $orange 26; $g.DrawLine($p, 190, 190, 322, 322); $g.DrawLine($p, 322, 190, 190, 322); Save $c 'rc_sel_none.png'
$c = New-Canvas; $g = $c[1]; $g.FillPolygon((Br $orange), (Pts @(110, 402, 402, 110, 402, 402))); $g.DrawRectangle((Dash $grey 18), 110, 110, 292, 292); Save $c 'rc_sel_invert.png'

# ================= the shapes' modes =================
$c = New-Canvas; $c[1].DrawRectangle((Pen $grey 24), 120, 150, 272, 212); Save $c 'rc_shape_rect.png'
$c = New-Canvas; $c[1].DrawEllipse((Pen $grey 24), 104, 140, 304, 232); Save $c 'rc_shape_ellipse.png'
$c = New-Canvas; $g = $c[1]; $g.DrawLine((Pen $grey 24), 130, 382, 382, 130); $g.FillEllipse((Br $orange), 98, 350, 64, 64); $g.FillEllipse((Br $orange), 350, 98, 64, 64); Save $c 'rc_shape_line.png'
$c = New-Canvas; $g = $c[1]; $g.FillRectangle((Br $orange), 130, 150, 252, 212); $g.DrawRectangle((Pen $grey 14), 130, 150, 252, 212); Save $c 'rc_shape_filled.png'
$c = New-Canvas; $g = $c[1]; $g.DrawRectangle((Pen $orange 30), 130, 150, 252, 212); Save $c 'rc_shape_outline.png'

# ================= the move's modes =================
$c = New-Canvas; $g = $c[1]; Sheet $g (Pen $grey 18) 230 $brown; Arrow $g (Pen $orange 22) 150 380 362 380 46; Save $c 'rc_move_layer.png'
$c = New-Canvas; $g = $c[1]; foreach ($y in @(170, 230, 290)) { Sheet $g (Pen $grey 16) $y $bg }; Arrow $g (Pen $orange 22) 150 420 362 420 46; Save $c 'rc_move_all.png'

# ================= the brush sets =================
$c = New-Canvas; $g = $c[1]; BrushShape $g 10 30 0.86 $false; Plus $g 372 372 56; Save $c 'rc_brush_new.png'
$c = New-Canvas; $g = $c[1]
foreach ($d in @(@(140, 150, 70), @(240, 120, 90), @(350, 160, 60))) { $g.FillEllipse((Br $grey), $d[0] - $d[2] / 2, $d[1] - $d[2] / 2, $d[2], $d[2]) }
Arrow $g (Pen $orange 24) 256 220 256 380 50
$g.DrawLines((Pen $grey 20), (Pts @(110, 330, 110, 420, 402, 420, 402, 330)))
Save $c 'rc_brush_import.png'
$c = New-Canvas; $g = $c[1]; Folder $g (Pen $grey 20) 90 140 300 240; Plus $g 380 380 50; Save $c 'rc_set_new.png'

# ================= the reference =================
$c = New-Canvas; $g = $c[1]
$g.FillEllipse((Br $dim), 120, 130, 220, 260)
$g.FillRectangle((Br $orange), 230, 170, 170, 220); $g.DrawRectangle((Pen $grey 16), 230, 170, 170, 220)
Save $c 'rc_front.png'
$c = New-Canvas; $g = $c[1]
$g.FillRectangle((Br $orange), 230, 170, 170, 220); $g.DrawRectangle((Pen $grey 16), 230, 170, 170, 220)
$g.FillEllipse((Br $bg), 104, 114, 252, 292); $g.FillEllipse((Br $dim), 120, 130, 220, 260)
Save $c 'rc_behind.png'
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $dim 16), 150, 90, 270, 200); $g.FillRectangle((Br $bg), 92, 160, 290, 230)
$g.DrawRectangle((Pen $grey 18), 92, 160, 290, 230)
$g.DrawLines((Pen $grey 16), (Pts @(120, 360, 200, 270, 250, 320, 300, 250, 360, 360)))
$g.FillEllipse((Br $orange), 136, 190, 60, 60)
Save $c 'rc_library.png'
$c = New-Canvas; $g = $c[1]; Page $g (Pen $dim 16) 110 90 200 250 $bg; Page $g (Pen $grey 18) 200 170 200 250 $bg; Plus $g 300 300 44; Save $c 'rc_canvas_copy.png'
$c = New-Canvas; $g = $c[1]
foreach ($y in @(200, 260)) { Sheet $g (Pen $grey 16) $y $bg }
$g.DrawArc((Pen $orange 22), 150, 300, 200, 130, 200, 230)
$po = Pen $orange 22; $g.DrawLine($po, 168, 330, 150, 392); $g.DrawLine($po, 168, 330, 222, 360)
Save $c 'rc_offset_reset.png'

# ================= the layers =================
$c = New-Canvas; $g = $c[1]; Page $g (Pen $grey 20) 120 100 230 300 $bg; Plus $g 360 360 52; Save $c 'rc_layer_new.png'
$c = New-Canvas; $g = $c[1]; Page $g (Pen $grey 20) 120 100 230 300 $orange; $g.FillEllipse((Br $bg), 300, 300, 120, 120); Plus $g 360 360 44; Save $c 'rc_fill_layer.png'

$c = New-Canvas; $g = $c[1]; Folder $g (Pen $grey 20) 90 130 300 240; Plus $g 380 380 50; Save $c 'rc_group_new.png'
$c = New-Canvas; $g = $c[1]; $g.DrawRectangle((Pen $grey 20), 100, 130, 312, 252); $g.FillEllipse((Br $orange), 186, 186, 140, 140); Save $c 'rc_mask.png'
$c = New-Canvas; $g = $c[1]
Sheet $g (Pen $dim 16) 160 $bg; Sheet $g (Pen $grey 18) 330 $brown
Arrow $g (Pen $orange 24) 256 150 256 300 46
Save $c 'rc_merge.png'
$c = New-Canvas; $g = $c[1]; Sheet $g (Pen $grey 16) 330 $bg; Arrow $g (Pen $orange 26) 256 290 256 100 60; Save $c 'rc_layer_up.png'
$c = New-Canvas; $g = $c[1]; Sheet $g (Pen $grey 16) 180 $bg; Arrow $g (Pen $orange 26) 256 220 256 412 60; Save $c 'rc_layer_down.png'
$c = New-Canvas; $g = $c[1]; Page $g (Pen $grey 18) 110 90 200 250 $bg; Page $g (Pen $orange 20) 200 170 200 250 $bg; Save $c 'rc_layer_dup.png'
$c = New-Canvas; $g = $c[1]
$pg = Pen $grey 20
$g.DrawLine($pg, 120, 150, 392, 150); $g.DrawLines($pg, (Pts @(210, 150, 226, 104, 286, 104, 302, 150)))
$g.DrawPolygon($pg, (Pts @(150, 150, 362, 150, 340, 410, 172, 410)))
$po = Pen $orange 18; foreach ($x in @(212, 256, 300)) { $g.DrawLine($po, $x, 200, $x, 360) }
Save $c 'rc_delete.png'
$c = New-Canvas; $g = $c[1]
$g.DrawArc((Pen $grey 22), 176, 96, 160, 200, 180, 180)
$g.DrawLine((Pen $grey 22), 176, 196, 176, 236); $g.DrawLine((Pen $grey 22), 336, 196, 336, 236)
$g.FillRectangle((Br $brown), 140, 236, 232, 176); $g.DrawRectangle((Pen $orange 18), 140, 236, 232, 176)
$g.FillEllipse((Br $orange), 236, 296, 40, 40)
Save $c 'rc_lock.png'
# ================= the transform =================
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Dash $dim 14), 256, 90, 256, 420)
$g.DrawPolygon((Pen $grey 18), (Pts @(220, 140, 220, 380, 90, 380)))
$g.FillPolygon((Br $orange), (Pts @(292, 140, 292, 380, 422, 380))); $g.DrawPolygon((Pen $grey 14), (Pts @(292, 140, 292, 380, 422, 380)))
Save $c 'rc_flip_h.png'
$c = New-Canvas; $g = $c[1]
$g.DrawLine((Dash $dim 14), 90, 256, 420, 256)
$g.DrawPolygon((Pen $grey 18), (Pts @(140, 220, 380, 220, 380, 90)))
$g.FillPolygon((Br $orange), (Pts @(140, 292, 380, 292, 380, 422))); $g.DrawPolygon((Pen $grey 14), (Pts @(140, 292, 380, 292, 380, 422)))
Save $c 'rc_flip_v.png'
$c = New-Canvas; $g = $c[1]
$g.DrawRectangle((Pen $grey 18), 186, 186, 140, 140)
$g.DrawArc((Pen $orange 22), 110, 110, 292, 292, 200, 290)
$po = Pen $orange 22; $g.DrawLine($po, 128, 210, 112, 140); $g.DrawLine($po, 128, 210, 196, 186)
Save $c 'rc_xf_reset.png'
Write-Output "icons written to $out"
