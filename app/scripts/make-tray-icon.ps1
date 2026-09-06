$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# Renders a multi-size REX tray icon into app/assets/rex-tray.ico
# (PNG-compressed ICO: 16,20,24,32,40,48,64,256).

$root = Split-Path -Parent $PSScriptRoot
$outFile = Join-Path $root 'assets\rex-tray.ico'
New-Item -ItemType Directory -Path (Split-Path $outFile) -Force | Out-Null

function New-RoundedRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$images = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [Math]::Max(1.0, [double]$s * 0.06)
    $w = [single]($s - 2 * $pad)
    $h = [single]($s - 2 * $pad)
    $rect = New-Object System.Drawing.RectangleF -ArgumentList @([single]$pad, [single]$pad, $w, $h)
    $path = New-RoundedRectPath $rect.X $rect.Y $rect.Width $rect.Height ([single]($rect.Width * 0.24))

    $c1 = [System.Drawing.Color]::FromArgb(255, 46, 91, 255)
    $c2 = [System.Drawing.Color]::FromArgb(255, 124, 58, 237)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45.0)
    $g.FillPath($brush, $path)
    $brush.Dispose()

    if ($s -ge 20) {
        $fontSize = [single]($s * 0.58)
        $font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = [System.Drawing.StringAlignment]::Center
        $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
        $g.DrawString('R', $font, [System.Drawing.Brushes]::White, $rect, $fmt)
        $fmt.Dispose()
        $font.Dispose()
    }

    $g.Dispose()
    $path.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += @{ size = $s; data = $ms.ToArray() }
    $ms.Dispose()
    $bmp.Dispose()
}

$fs = [System.IO.File]::Open($outFile, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([byte]0); $bw.Write([byte]0)          # reserved
$bw.Write([byte]1); $bw.Write([byte]0)          # type = icon
$bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = if ($img.size -ge 256) { 0 } else { $img.size }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)                          # color count
    $bw.Write([byte]0)                          # reserved
    $bw.Write([uint16]1)                        # planes
    $bw.Write([uint16]32)                       # bpp
    $bw.Write([uint32]$img.data.Length)
    $bw.Write([uint32]$offset)
    $offset += $img.data.Length
}
foreach ($img in $images) { $bw.Write($img.data) }
$bw.Close()
$fs.Close()

Write-Output "tray icon written: $outFile ($((Get-Item $outFile).Length) bytes, $($images.Count) sizes)"
