# Draws the UV shell-tag icons in the installed style (2000x2000, dark rounded tile, light-grey strokes, orange accent).
# Run: powershell -File tools\make_shell_icons.ps1   -> resources\images\uv_shell*.png
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'resources\images'
$grey = [System.Drawing.Color]::FromArgb(255, 207, 207, 207)
$orange = [System.Drawing.Color]::FromArgb(255, 240, 127, 0)
$brown = [System.Drawing.Color]::FromArgb(255, 86, 56, 24)
$bg = [System.Drawing.Color]::FromArgb(255, 32, 32, 32)
$W = 44

function New-Canvas {
  $bmp = New-Object System.Drawing.Bitmap 2000, 2000
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $r = 580
  $path.AddArc(0, 0, $r, $r, 180, 90); $path.AddArc(2000 - $r, 0, $r, $r, 270, 90)
  $path.AddArc(2000 - $r, 2000 - $r, $r, $r, 0, 90); $path.AddArc(0, 2000 - $r, $r, $r, 90, 90); $path.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush $bg), $path)
  return @($bmp, $g)
}
function Pen($c, $w = 44) { $p = New-Object System.Drawing.Pen $c, $w; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p.LineJoin = 'Round'; return $p }
function Pt($x, $y) { return New-Object System.Drawing.PointF $x, $y }
function Save($bmp, $name) { $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }
function Leaf($g, $pen, $cx, $cy, $flip) {   # a strand-like shell: tall, pointed ends, drawn about (cx, cy)
  $s = if ($flip) { -1 } else { 1 }
  $pts = @((Pt ($cx) ($cy - 420)), (Pt ($cx + $s * 170) ($cy - 120)), (Pt ($cx + $s * 120) ($cy + 220)), (Pt ($cx) ($cy + 420)), (Pt ($cx - $s * 80) ($cy + 150)), (Pt ($cx - $s * 130) ($cy - 150)))
  $g.DrawClosedCurve($pen, $pts, 0.35, 'Winding')
}

# --- lock: a padlock
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$g.DrawArc((Pen $grey), 700, 420, 600, 760, 180, 180)
$g.DrawLine((Pen $grey), 700, 800, 700, 960); $g.DrawLine((Pen $grey), 1300, 800, 1300, 960)
$g.FillRectangle((New-Object System.Drawing.SolidBrush $brown), 560, 960, 880, 640)
$g.DrawRectangle((Pen $orange), 560, 960, 880, 640)
$g.FillEllipse((New-Object System.Drawing.SolidBrush $orange), 940, 1190, 120, 120)
$g.DrawLine((Pen $orange 56), 1000, 1280, 1000, 1430)
Save $bmp 'uv_shelllock.png'

# --- sym: two mirrored shells across a dashed axis, an orange two-headed arrow
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$dash = Pen $grey 30; $dash.DashStyle = 'Dash'
$g.DrawLine($dash, 1000, 330, 1000, 1500)
Leaf $g (Pen $grey) 640 880 $false
Leaf $g (Pen $grey) 1360 880 $true
$po = Pen $orange 52
$g.DrawLine($po, 560, 1680, 1440, 1680)
$g.DrawLine($po, 560, 1680, 680, 1580); $g.DrawLine($po, 560, 1680, 680, 1780)
$g.DrawLine($po, 1440, 1680, 1320, 1580); $g.DrawLine($po, 1440, 1680, 1320, 1780)
Save $bmp 'uv_shellsym.png'

# --- free turn: a shell with an orange circular arrow round it
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
Leaf $g (Pen $grey) 1000 1000 $false
$po = Pen $orange 52
$g.DrawArc($po, 330, 330, 1340, 1340, 200, 250)
$g.DrawLine($po, 1560, 1060, 1480, 920); $g.DrawLine($po, 1560, 1060, 1690, 930)
Save $bmp 'uv_shellfree.png'

# --- group: two shells in a dashed orange frame with a G
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$fr = Pen $orange 38; $fr.DashStyle = 'Dash'
$g.DrawRectangle($fr, 330, 420, 1340, 1160)
$g.DrawRectangle((Pen $grey), 480, 600, 460, 780); $g.DrawRectangle((Pen $grey), 1060, 600, 460, 460)
$f = New-Object System.Drawing.Font 'Segoe UI', 230, ([System.Drawing.FontStyle]::Bold), 'Pixel'
$g.DrawString("G", $f, (New-Object System.Drawing.SolidBrush $orange), 1130, 1100)
Save $bmp 'uv_shellgroup.png'

# --- ungroup: the same shells apart, the frame cut by an orange slash
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$g.DrawRectangle((Pen $grey), 330, 620, 520, 760); $g.DrawRectangle((Pen $grey), 1150, 760, 520, 620)
$fr = Pen $orange 38; $fr.DashStyle = 'Dash'
$g.DrawRectangle($fr, 250, 460, 1500, 1080)
$g.DrawLine((Pen $orange 70), 560, 1500, 1440, 500)
Save $bmp 'uv_shellungroup.png'

# --- clear: a shell, struck out by an orange cross
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
Leaf $g (Pen $grey) 1000 1000 $false
$po = Pen $orange 70
$g.DrawLine($po, 600, 600, 1400, 1400); $g.DrawLine($po, 1400, 600, 600, 1400)
Save $bmp 'uv_shellclear.png'
Write-Output 'icons written'
