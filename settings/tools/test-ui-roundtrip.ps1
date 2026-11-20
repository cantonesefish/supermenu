# End-to-end UI test:
#   1. toggle 预留工作区 on 外观, press 应用, assert JSON changed;
#   2. navigate to 液态玻璃, choose the 透明 preset, set the tint hex, press 应用,
#      assert the preset parameters and tint were persisted.
param(
    [string]$Exe = 'C:\fish\git\kevison-supermenu\settings\SuperMenu.Settings\bin\Debug\net10.0-windows\SuperMenu.Settings.exe',
    [string]$SettingsPath = 'C:\fish\git\kevison-supermenu\settings\screens\ui-test\settings.json',
    [string]$ShotDir = 'C:\fish\git\kevison-supermenu\settings\screens'
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UiTestNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[UiTestNative]::SetProcessDPIAware() | Out-Null

$ae = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]::Descendants

function Find-ByName([string]$name, $controlType) {
    $conditions = New-Object System.Collections.Generic.List[System.Windows.Automation.Condition]
    $conditions.Add((New-Object System.Windows.Automation.PropertyCondition($ae::NameProperty, $name)))
    if ($null -ne $controlType) {
        $conditions.Add((New-Object System.Windows.Automation.PropertyCondition($ae::ControlTypeProperty, $controlType)))
    }
    $condition = if ($conditions.Count -eq 1) { $conditions[0] } else { New-Object System.Windows.Automation.AndCondition($conditions.ToArray()) }
    return $script:root.FindFirst($scope, $condition)
}

function Invoke-Element($element, [string]$patternName) {
    if ($null -eq $element) { throw "$patternName element not found" }
    switch ($patternName) {
        'Toggle' { $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
        'Select' { $element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
        'Invoke' { $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
        default { throw "unknown pattern $patternName" }
    }
}

function Save-Shot([string]$name) {
    $rect = New-Object UiTestNative+RECT
    [UiTestNative]::GetWindowRect($script:handle, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $path = Join-Path $ShotDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Screenshot: $path"
}

function Click-Apply {
    $apply = Find-ByName '应用' ([System.Windows.Automation.ControlType]::Button)
    Invoke-Element $apply 'Invoke'
    Start-Sleep -Milliseconds 800
}

$dir = Split-Path -Parent $SettingsPath
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
if (-not (Test-Path $ShotDir)) { New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null }

Get-Process -Name 'SuperMenu.Settings' -ErrorAction SilentlyContinue | ForEach-Object {
    try { $_.Kill(); $_.WaitForExit(2000) } catch { }
}
$env:SUPERMENU_SETTINGS_PATH = $SettingsPath

$process = Start-Process -FilePath $Exe -PassThru
$deadline = (Get-Date).AddSeconds(15)
$script:handle = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
    if ($process.HasExited) { throw "Exited with $($process.ExitCode)" }
    $script:handle = $process.MainWindowHandle
    if ($script:handle -ne [IntPtr]::Zero) { break }
}
Start-Sleep -Milliseconds 2500
[UiTestNative]::SetForegroundWindow($script:handle) | Out-Null
Start-Sleep -Milliseconds 300

$script:root = $ae::FromHandle($script:handle)
if ($null -eq $script:root) { throw 'UIAutomation root not found' }

$failures = 0
function Assert([string]$name, [bool]$ok, [string]$detail = '') {
    if (-not $ok) { $script:failures++ }
    Write-Host ("{0,-46} {1} {2}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $detail)
}

# ---------- Phase 1: toggle + apply ----------
$reserve = Find-ByName '预留工作区' ([System.Windows.Automation.ControlType]::Button)
$toggle = $reserve.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
$before = $toggle.Current.ToggleState
Invoke-Element $reserve 'Toggle'
Start-Sleep -Milliseconds 350
$after = $toggle.Current.ToggleState
Click-Apply
$json = Get-Content $SettingsPath -Raw | ConvertFrom-Json
Assert 'toggle 预留工作区 On → Off' (($before.ToString() -eq 'On') -and ($after.ToString() -eq 'Off'))
Assert 'apply persisted reserveWorkArea=false' ($json.reserveWorkArea -eq $false)
Save-Shot 'ui-01-applied-toast.png'

# ---------- Phase 2: glass preset + tint ----------
$navGlass = Find-ByName '液态玻璃' ([System.Windows.Automation.ControlType]::RadioButton)
Invoke-Element $navGlass 'Select'
Start-Sleep -Milliseconds 600
$presetClear = Find-ByName '透明' ([System.Windows.Automation.ControlType]::RadioButton)
Invoke-Element $presetClear 'Select'
Start-Sleep -Milliseconds 600
Save-Shot 'ui-02-glass-clear.png'

$tintBox = Find-ByName '玻璃色调十六进制值' ([System.Windows.Automation.ControlType]::Edit)
if ($null -ne $tintBox) {
    $tintBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('#FFCC88')
    Start-Sleep -Milliseconds 400
} else {
    Assert 'tint hex textbox found' $false
}
Click-Apply
$json2 = Get-Content $SettingsPath -Raw | ConvertFrom-Json
Assert 'preset persisted as clear' ($json2.glassPreset -eq 'clear') "glassPreset=$($json2.glassPreset)"
Assert 'clear preset lowered blur' ($json2.blur -lt 0.2) "blur=$($json2.blur)"
Assert 'clear preset raised dispersion' ($json2.dispersion -gt 0.5) "dispersion=$($json2.dispersion)"
Assert 'tint persisted as #FFCC88' ($json2.tint -eq '#FFCC88') "tint=$($json2.tint)"
Save-Shot 'ui-03-glass-applied.png'

if (-not $process.HasExited) { try { $process.Kill(); $process.WaitForExit(2000) } catch { } }
Write-Host "FAILURES: $failures"
exit $failures
