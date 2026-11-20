# Generates SuperMenu.Settings/Assets/SuperMenu.ico without external tooling.
# Requires Windows PowerShell 5.1+ / pwsh with System.Drawing (Windows only).
param(
    [string]$Output = (Join-Path $PSScriptRoot '..\SuperMenu.Settings\Assets\SuperMenu.ico'),
    [string]$Preview = (Join-Path $PSScriptRoot 'icon-preview.png')
)

Add-Type -AssemblyName System.Drawing

function New-RoundedPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float]($R * 2)
    $p.AddArc($X, $Y, $d, $d, 180, 90)
    $p.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $p.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $p.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap {
    param([int]$Size, [int]$Super = 4)
    $s = $Size * $Super
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Squircle body
    $pad = [float]($s * 0.035)
    $body = New-RoundedPath $pad $pad ($s - 2 * $pad) ($s - 2 * $pad) ([float]($s * 0.225))
    $rect = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, [System.Drawing.Color]::FromArgb(255, 32, 35, 48), [System.Drawing.Color]::FromArgb(255, 13, 14, 20), 55.0)
    $g.FillPath($bg, $body)
    $bg.Dispose()

    # Soft colour glows (orange top-right, blue bottom-left) clipped to the squircle.
    $oldClip = $g.Clip
    $g.SetClip($body, [System.Drawing.Drawing2D.CombineMode]::Intersect)
    $glow1 = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glow1.AddEllipse([float]($s * 0.50), [float](-$s * 0.22), [float]($s * 0.85), [float]($s * 0.85))
    $pg1 = New-Object System.Drawing.Drawing2D.PathGradientBrush($glow1)
    $pg1.CenterColor = [System.Drawing.Color]::FromArgb(210, 255, 138, 46)
    $pg1.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 255, 138, 46))
    $g.FillPath($pg1, $glow1)
    $pg1.Dispose(); $glow1.Dispose()

    $glow2 = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glow2.AddEllipse([float](-$s * 0.30), [float]($s * 0.42), [float]($s * 0.95), [float]($s * 0.95))
    $pg2 = New-Object System.Drawing.Drawing2D.PathGradientBrush($glow2)
    $pg2.CenterColor = [System.Drawing.Color]::FromArgb(190, 46, 140, 255)
    $pg2.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 46, 140, 255))
    $g.FillPath($pg2, $glow2)
    $pg2.Dispose(); $glow2.Dispose()

    # Bottom darkening so the glass bar reads on top.
    $shadeRect = New-Object System.Drawing.RectangleF(0, [float]($s * 0.35), $s, [float]($s * 0.65))
    $shade = New-Object System.Drawing.Drawing2D.LinearGradientBrush($shadeRect, [System.Drawing.Color]::FromArgb(0, 0, 0, 0), [System.Drawing.Color]::FromArgb(120, 0, 0, 0), 90.0)
    $g.FillRectangle($shade, $shadeRect)
    $shade.Dispose()
    $g.Clip = $oldClip

    # Glass menu bar capsule.
    $bx = [float]($s * 0.135); $by = [float]($s * 0.185); $bw = [float]($s * 0.73); $bh = [float]($s * 0.255)
    $bar = New-RoundedPath $bx $by $bw $bh ([float]($bh / 2))
    $barRect = New-Object System.Drawing.RectangleF($bx, $by, $bw, $bh)
    $barBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($barRect, [System.Drawing.Color]::FromArgb(150, 255, 255, 255), [System.Drawing.Color]::FromArgb(46, 255, 255, 255), 90.0)
    $g.FillPath($barBrush, $bar)
    $barBrush.Dispose()
    $barPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(165, 255, 255, 255), [float]($s * 0.012))
    $g.DrawPath($barPen, $bar)
    $barPen.Dispose()

    # Content inside the bar: a small apple-ish dot + menu ticks.
    $dot = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235, 255, 255, 255))
    $g.FillEllipse($dot, [float]($bx + $bh * 0.30), [float]($by + $bh * 0.33), [float]($bh * 0.30), [float]($bh * 0.30))
    $tickPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(225, 255, 255, 255), [float]($s * 0.026))
    $tickPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $tickPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    for ($i = 0; $i -lt 3; $i++) {
        $tx = [float]($bx + $bh * 0.62 + $i * $bw * 0.145)
        $g.DrawLine($tickPen, $tx, [float]($by + $bh * 0.38), $tx, [float]($by + $bh * 0.62))
    }
    # Right cluster: two tiny dots.
    for ($i = 0; $i -lt 2; $i++) {
        $ex = [float]($bx + $bw - $bh * 0.42 - $i * $bw * 0.085)
        $g.FillEllipse($dot, $ex, [float]($by + $bh * 0.40), [float]($bh * 0.20), [float]($bh * 0.20))
    }
    $dot.Dispose(); $tickPen.Dispose(); $bar.Dispose()

    # Bottom colour band: a muted teal/blue wave, kept gentle so it reads at 32 px.
    $wave = New-Object System.Drawing.Drawing2D.GraphicsPath
    $wave.AddBezier(0, [float]($s * 0.88), [float]($s * 0.34), [float]($s * 0.72), [float]($s * 0.66), [float]($s * 0.99), $s, [float]($s * 0.80))
    $wave.AddLine([float]$s, [float]$s, 0, [float]$s)
    $wave.AddLine(0, [float]$s, 0, [float]($s * 0.88))
    $wave.CloseFigure()
    $oldClip2 = $g.Clip
    $g.SetClip($body, [System.Drawing.Drawing2D.CombineMode]::Intersect)
    $waveBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF(0, [float]($s * 0.6), $s, [float]($s * 0.4))), [System.Drawing.Color]::FromArgb(120, 47, 212, 194), [System.Drawing.Color]::FromArgb(45, 40, 120, 255), 20.0)
    $g.FillPath($waveBrush, $wave)
    $waveBrush.Dispose()
    $g.Clip = $oldClip2
    $wave.Dispose()

    # Soft specular sheen along the top-left edge.
    $sheen = New-Object System.Drawing.Drawing2D.GraphicsPath
    $sheen.AddEllipse([float](-$s * 0.25), [float](-$s * 0.55), [float]($s * 0.95), [float]($s * 0.85))
    $oldClip3 = $g.Clip
    $g.SetClip($body, [System.Drawing.Drawing2D.CombineMode]::Intersect)
    $pg3 = New-Object System.Drawing.Drawing2D.PathGradientBrush($sheen)
    $pg3.CenterColor = [System.Drawing.Color]::FromArgb(52, 255, 255, 255)
    $pg3.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.FillPath($pg3, $sheen)
    $pg3.Dispose()
    $g.Clip = $oldClip3
    $sheen.Dispose()

    # Hairline inner border.
    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(46, 255, 255, 255), [float]($s * 0.014))
    $g.DrawPath($borderPen, $body)
    $borderPen.Dispose()
    $body.Dispose()

    $g.Dispose()
    if ($Super -gt 1) {
        $small = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $sg = [System.Drawing.Graphics]::FromImage($small)
        $sg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $sg.Clear([System.Drawing.Color]::Transparent)
        $sg.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, $Size, $Size)))
        $sg.Dispose(); $bmp.Dispose()
        return $small
    }
    return $bmp
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$pngBytes = @{}
foreach ($size in $sizes) {
    $bmp = New-IconBitmap -Size $size -Super 4
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBytes[$size] = $ms.ToArray()
    if ($size -eq 256) { $bmp.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png) }
    $ms.Dispose(); $bmp.Dispose()
}

# ICO container with PNG entries (Vista+ format).
$outDir = Split-Path -Parent $Output
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$fs = [System.IO.File]::Create($Output)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)              # reserved
$bw.Write([UInt16]1)              # type: icon
$bw.Write([UInt16]$sizes.Count)   # count
$offset = 6 + 16 * $sizes.Count
foreach ($size in $sizes) {
    $data = $pngBytes[$size]
    $bw.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
    $bw.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
    $bw.Write([byte]0)            # color palette
    $bw.Write([byte]0)            # reserved
    $bw.Write([UInt16]1)          # planes
    $bw.Write([UInt16]32)         # bpp
    $bw.Write([UInt32]$data.Length)
    $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($size in $sizes) { $bw.Write($pngBytes[$size]) }
$bw.Flush(); $bw.Dispose(); $fs.Dispose()
Write-Host "Wrote $Output ($((Get-Item $Output).Length) bytes, $($sizes.Count) sizes)"
Write-Host "Wrote $Preview"
