# Verifies the custom window chrome: NC hit-testing (drag / resize corners) and maximize bounds.
param(
    [string]$Exe = 'C:\fish\git\kevison-supermenu\settings\SuperMenu.Settings\bin\Debug\net10.0-windows\SuperMenu.Settings.exe'
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WinChromeTest {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, int flags);
    [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
'@
[WinChromeTest]::SetProcessDPIAware() | Out-Null

$existing = Get-Process -Name 'SuperMenu.Settings' -ErrorAction SilentlyContinue
foreach ($p in $existing) { try { $p.Kill(); $p.WaitForExit(2000) } catch { } }
$process = Start-Process -FilePath $Exe -PassThru
$deadline = (Get-Date).AddSeconds(15)
$handle = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
    if ($process.HasExited) { throw "Exited with $($process.ExitCode)" }
    $handle = $process.MainWindowHandle
    if ($handle -ne [IntPtr]::Zero) { break }
}
Start-Sleep -Seconds 2
[WinChromeTest]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object WinChromeTest+RECT
[WinChromeTest]::GetWindowRect($handle, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
Write-Host "Window: $($rect.Left),$($rect.Top) ${w}x${h}"

function HitTest([int]$screenX, [int]$screenY) {
    $lParam = [IntPtr](($screenY -shl 16) -bor ($screenX -band 0xFFFF))
    return [int][WinChromeTest]::SendMessage($handle, 0x0084, [IntPtr]::Zero, $lParam)
}
$margin = 18 * 1.5   # shell margin is 18 DIP, system DPI is 150 %
$results = @(
    @{ Name = 'titlebar centre → HTCAPTION';   X = [int]($rect.Left + $w / 2); Y = [int]($rect.Top + $margin + 20); Expect = 2 },
    @{ Name = 'traffic-light corner → HTCLIENT'; X = [int]($rect.Left + $margin + 25); Y = [int]($rect.Top + $margin + 26); Expect = 1 },
    @{ Name = 'left visible edge → HTLEFT';    X = [int]($rect.Left + $margin); Y = [int]($rect.Top + $h / 2); Expect = 10 },
    @{ Name = 'right visible edge → HTRIGHT';  X = [int]($rect.Right - $margin); Y = [int]($rect.Top + $h / 2); Expect = 11 },
    @{ Name = 'top visible edge → HTTOP';      X = [int]($rect.Left + $w / 2); Y = [int]($rect.Top + $margin); Expect = 12 },
    @{ Name = 'bottom visible edge → HTBOTTOM'; X = [int]($rect.Left + $w / 2); Y = [int]($rect.Bottom - $margin); Expect = 15 },
    @{ Name = 'top-left corner → HTTOPLEFT';   X = [int]($rect.Left + $margin + 2); Y = [int]($rect.Top + $margin + 2); Expect = 13 },
    @{ Name = 'content centre → HTCLIENT';     X = [int]($rect.Left + $w / 2); Y = [int]($rect.Top + $h / 2); Expect = 1 }
)
$failed = 0
foreach ($test in $results) {
    $actual = HitTest $test.X $test.Y
    $ok = $actual -eq $test.Expect
    if (-not $ok) { $failed++ }
    Write-Host ("{0,-40} expected={1,-2} actual={2,-2} {3}" -f $test.Name, $test.Expect, $actual, $(if ($ok) { 'PASS' } else { 'FAIL' }))
}

# Maximize via WM_SYSCOMMAND(SC_MAXIMIZE) and verify it respects the work area.
[WinChromeTest]::SendMessage($handle, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
$max = New-Object WinChromeTest+RECT
[WinChromeTest]::GetWindowRect($handle, [ref]$max) | Out-Null
$mon = [WinChromeTest]::MonitorFromWindow($handle, 0)
$mi = New-Object WinChromeTest+MONITORINFO
$mi.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mi)
[WinChromeTest]::GetMonitorInfo($mon, [ref]$mi) | Out-Null
$workW = $mi.rcWork.Right - $mi.rcWork.Left
$workH = $mi.rcWork.Bottom - $mi.rcWork.Top
$maxW = $max.Right - $max.Left
$maxH = $max.Bottom - $max.Top
$maxOk = ([math]::Abs($max.Left - $mi.rcWork.Left) -le 2) -and ([math]::Abs($max.Top - $mi.rcWork.Top) -le 2) -and ([math]::Abs($maxW - $workW) -le 2) -and ([math]::Abs($maxH - $workH) -le 2)
Write-Host ("maximized: {0},{1} {2}x{3}  workarea: {4},{5} {6}x{7}  {8}" -f $max.Left, $max.Top, $maxW, $maxH, $mi.rcWork.Left, $mi.rcWork.Top, $workW, $workH, $(if ($maxOk) { 'PASS' } else { 'FAIL' }))
if (-not $maxOk) { $failed++ }

$bmp = New-Object System.Drawing.Bitmap($maxW, $maxH)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($max.Left, $max.Top, 0, 0, (New-Object System.Drawing.Size($maxW, $maxH)))
$g.Dispose()
$bmp.Save('C:\fish\git\kevison-supermenu\settings\screens\maximized.png', [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

[WinChromeTest]::SendMessage($handle, 0x0112, [IntPtr]0xF120, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 600
$restored = New-Object WinChromeTest+RECT
[WinChromeTest]::GetWindowRect($handle, [ref]$restored) | Out-Null
$restoreOk = ([math]::Abs(($restored.Right - $restored.Left) - $w) -le 2) -and ([math]::Abs(($restored.Bottom - $restored.Top) - $h) -le 2)
Write-Host ("restored: {0}x{1}  {2}" -f ($restored.Right - $restored.Left), ($restored.Bottom - $restored.Top), $(if ($restoreOk) { 'PASS' } else { 'FAIL' }))
if (-not $restoreOk) { $failed++ }

if (-not $process.HasExited) { try { $process.Kill(); $process.WaitForExit(2000) } catch { } }
Write-Host "FAILURES: $failed"
exit $failed
