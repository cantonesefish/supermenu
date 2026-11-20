# Captures the SuperMenu settings window to a PNG for visual verification.
# Usage: pwsh -File capture.ps1 -Exe <path> -Out <png> [-Page appearance|glass|menubar|about] [-SettingsPath <json>]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Page = 'appearance',
    [string]$SettingsPath = '',
    [int]$WaitMs = 3200
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Win32Capture]::SetProcessDPIAware() | Out-Null

$existing = Get-Process -Name 'SuperMenu.Settings' -ErrorAction SilentlyContinue
foreach ($p in $existing) { try { $p.Kill(); $p.WaitForExit(2000) } catch { } }

if ($SettingsPath -ne '') {
    $env:SUPERMENU_SETTINGS_PATH = $SettingsPath
} else {
    Remove-Item Env:SUPERMENU_SETTINGS_PATH -ErrorAction SilentlyContinue
}

$arguments = @('--page', $Page)
$process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
$deadline = (Get-Date).AddMilliseconds($WaitMs * 2)
$handle = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
    if ($process.HasExited) { throw "SuperMenu.Settings exited with code $($process.ExitCode) during startup" }
    $handle = $process.MainWindowHandle
    if ($handle -ne [IntPtr]::Zero) { break }
}
if ($handle -eq [IntPtr]::Zero) { throw 'Main window handle not found' }

Start-Sleep -Milliseconds $WaitMs
[Win32Capture]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 600

$rect = New-Object Win32Capture+RECT
if (-not [Win32Capture]::GetWindowRect($handle, [ref]$rect)) { throw 'GetWindowRect failed' }
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host "Window rect: $($rect.Left),$($rect.Top) ${width}x${height}"

$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($width, $height)))
$graphics.Dispose()

$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host "Saved $Out"

$alive = -not $process.HasExited
if (-not $process.HasExited) {
    try { $process.Kill(); $process.WaitForExit(2000) } catch { }
}
Write-Host "Process alive before kill: $alive"
