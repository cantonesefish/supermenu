#Requires -Version 5.1
<#
.SYNOPSIS
  Grabs the primary screen to docs/screenshots/ (development QA helper).
#>
param(
    [string]$Name = 'screenshot',
    [int]$TopHeight = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'docs\screenshots'
New-Item -ItemType Directory -Force $dir | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bmp.Size)
$path = Join-Path $dir "$Name.png"
if ($TopHeight -gt 0) {
    $crop = New-Object System.Drawing.Bitmap($bounds.Width, $TopHeight)
    $gc = [System.Drawing.Graphics]::FromImage($crop)
    $gc.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, $bounds.Width, $TopHeight)),
        (New-Object System.Drawing.Rectangle(0, 0, $bounds.Width, $TopHeight)), [System.Drawing.GraphicsUnit]::Pixel)
    $crop.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose(); $gc.Dispose()
} else {
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}
$bmp.Dispose(); $g.Dispose()
Write-Host "wrote $path"
