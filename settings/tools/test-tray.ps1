# End-to-end UI test for the 系统托盘 card and the 右侧图标间距 slider.
#
# The app is launched against a TEMPORARY settings file (SUPERMENU_SETTINGS_PATH); the real
# %LOCALAPPDATA%\SuperMenu\settings.json is never touched. The test:
#   1. asserts the 菜单栏 page shows Windows' 33 notification-area records (9 promoted / 24 hidden),
#   2. forces one hidden icon into the top bar + moves the spacing slider to 20, presses 应用,
#      and asserts trayShow/rightIconSpacing/trayHide/38 keys in the temporary JSON,
#   3. forces one promoted icon into the hidden panel and asserts trayHide,
#   4. presses 恢复系统默认 and 应用, then asserts both override lists are empty again,
#   5. presses 刷新图标 and asserts the list is rebuilt,
#   6. saves screenshots of the 网络 row, the spacing slider and the tray card for visual review.
param(
    [string]$Exe = $(Join-Path (Split-Path -Parent $PSScriptRoot) 'SuperMenu.Settings\bin\Debug\net10.0-windows\SuperMenu.Settings.exe'),
    [string]$SettingsPath = $(Join-Path $env:TEMP 'supermenu-tray-ui\settings.json'),
    [string]$ShotDir = $(Join-Path (Split-Path -Parent $PSScriptRoot) 'screens')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TrayTestNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[TrayTestNative]::SetProcessDPIAware() | Out-Null

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

function Find-TextContaining([string]$pattern) {
    $condition = New-Object System.Windows.Automation.PropertyCondition($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
    foreach ($element in $script:root.FindAll($scope, $condition)) {
        if ($element.Current.Name -match $pattern) { return $element }
    }
    return $null
}

function Get-TrayToggles {
    $condition = New-Object System.Windows.Automation.PropertyCondition($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    return @($script:root.FindAll($scope, $condition) | Where-Object { $_.Current.AutomationId -like 'TrayToggle-*' })
}

function Get-ToggleState($element) {
    $pattern = $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    return $pattern.Current.ToggleState.ToString()
}

function Toggle-Element($element) {
    $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
}

function Invoke-Element($element) {
    if ($null -eq $element) { throw 'element not found' }
    $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Get-Summary([string]$pattern) {
    $element = Find-TextContaining $pattern
    if ($null -eq $element) { return $null }
    return $element.Current.Name
}

function Save-Shot([string]$name) {
    $rect = New-Object TrayTestNative+RECT
    [TrayTestNative]::GetWindowRect($script:handle, [ref]$rect) | Out-Null
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($width, $height)))
    $graphics.Dispose()
    $path = Join-Path $ShotDir $name
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host "Screenshot: $path"
}

function Scroll-Element([string]$name, [int]$percent) {
    $element = Find-ByName $name $null
    if ($null -eq $element) { throw "scroll element not found: $name" }
    $pattern = $element.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern)
    $pattern.SetScrollPercent([System.Windows.Automation.ScrollPattern]::NoScroll, $percent)
}

function Click-Apply {
    Invoke-Element (Find-ByName '应用' ([System.Windows.Automation.ControlType]::Button))
    Start-Sleep -Milliseconds 900
}

function Read-Settings {
    return Get-Content -LiteralPath $SettingsPath -Raw | ConvertFrom-Json
}

function Test-IdListSorted([string]$value) {
    if ([string]::IsNullOrEmpty($value)) { return $true }
    $ids = @($value -split ';' | ForEach-Object { [uint64]$_ })
    for ($i = 1; $i -lt $ids.Count; $i++) {
        if ($ids[$i] -lt $ids[$i - 1]) { return $false }
    }
    return $true
}

$settingsDirectory = Split-Path -Parent $SettingsPath
if (Test-Path $settingsDirectory) { Remove-Item $settingsDirectory -Recurse -Force }
New-Item -ItemType Directory -Force -Path $settingsDirectory | Out-Null
if (-not (Test-Path $ShotDir)) { New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null }

Get-Process -Name 'SuperMenu.Settings' -ErrorAction SilentlyContinue | ForEach-Object {
    try { $_.Kill(); $_.WaitForExit(2000) } catch { }
}
$env:SUPERMENU_SETTINGS_PATH = $SettingsPath

$process = Start-Process -FilePath $Exe -ArgumentList @('--page', 'menubar') -PassThru
$deadline = (Get-Date).AddSeconds(20)
$script:handle = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
    if ($process.HasExited) { throw "SuperMenu.Settings exited with code $($process.ExitCode)" }
    $script:handle = $process.MainWindowHandle
    if ($script:handle -ne [IntPtr]::Zero) { break }
}
if ($script:handle -eq [IntPtr]::Zero) { throw 'Main window handle not found' }

Start-Sleep -Milliseconds 2200
[TrayTestNative]::SetForegroundWindow($script:handle) | Out-Null
Start-Sleep -Milliseconds 400
$script:root = $ae::FromHandle($script:handle)
if ($null -eq $script:root) { throw 'UIAutomation root not found' }

$failures = 0
function Assert([string]$name, [bool]$ok, [string]$detail = '') {
    if (-not $ok) { $script:failures++ }
    Write-Host ("{0,-58} {1} {2}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $detail)
}

# ---------- Phase 1: catalog list ----------
$toggles = @()
for ($attempt = 0; $attempt -lt 40; $attempt++) {
    $toggles = Get-TrayToggles
    if ($toggles.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
}
$onCount = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'On' }).Count
$offCount = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'Off' }).Count
Assert 'tray list shows 33 Windows icons' ($toggles.Count -eq 33) "count=$($toggles.Count)"
Assert 'initial state 9 promoted / 24 hidden' (($onCount -eq 9) -and ($offCount -eq 24)) "on=$onCount off=$offCount"

$summary = Get-Summary '^共 \d+ 个图标'
Assert 'summary reports 9 shown / 24 hidden' ($null -ne $summary -and $summary -match '^共 33 个图标 · 顶栏显示 9 · 隐藏面板 24$') "$summary"

# 网络 row rename + hint (lead follow-up requirement, showWifi key unchanged).
$networkToggle = Find-ByName '网络' ([System.Windows.Automation.ControlType]::Button)
$networkHint = Find-TextContaining '^自动显示有线以太网或'
Assert '右侧图标 Wi-Fi row renamed to 网络' ($null -ne $networkToggle) 'AutomationProperties.Name=网络'
Assert '网络 row hint mentions Ethernet/Wi-Fi' ($null -ne $networkHint) $(if ($networkHint) { $networkHint.Current.Name } else { 'missing' })

# ---------- Phase 2: force a hidden icon into the top bar + spacing slider ----------
$hiddenToggle = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'Off' })[0]
$forcedShowId = $hiddenToggle.Current.AutomationId -replace '^TrayToggle-', ''
$forcedShowLabel = $hiddenToggle.Current.Name
Toggle-Element $hiddenToggle
Start-Sleep -Milliseconds 350
Assert 'toggling a hidden icon ON' ((Get-ToggleState $hiddenToggle) -eq 'On') "$forcedShowLabel ($forcedShowId)"
Assert 'dirty pill appears' ($null -ne (Find-TextContaining '^有未应用的更改$')) ''
$summary = Get-Summary '^共 \d+ 个图标'
Assert 'summary follows the override' ($null -ne $summary -and $summary -match '顶栏显示 10 · 隐藏面板 23') "$summary"

$spacingSlider = Find-ByName '右侧图标间距' ([System.Windows.Automation.ControlType]::Slider)
$sliderOk = $null -ne $spacingSlider
if ($sliderOk) {
    $range = $spacingSlider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    $sliderOk = ($range.Current.Minimum -eq 2) -and ($range.Current.Maximum -eq 24) -and ($range.Current.Value -eq 12)
    $range.SetValue(20)
    Start-Sleep -Milliseconds 350
}
Assert '右侧图标间距 slider (2–24, default 12) → 20' $sliderOk ''

Click-Apply
$json = Read-Settings
$jsonKeys = @($json.PSObject.Properties.Name)
Assert 'apply persisted 38 schema keys' ($jsonKeys.Count -eq 38) "keys=$($jsonKeys.Count)"
Assert 'apply persisted rightIconSpacing=20' ($json.rightIconSpacing -eq 20) "rightIconSpacing=$($json.rightIconSpacing)"
Assert 'apply persisted trayShow=forced id' ($json.trayShow -eq $forcedShowId) "trayShow=$($json.trayShow)"
Assert 'trayHide stays empty' ($json.trayHide -eq '') "trayHide=$($json.trayHide)"
Assert 'trayShow id list sorted' (Test-IdListSorted $json.trayShow) "trayShow=$($json.trayShow)"
Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ShotDir 'task6-settings-overrides.json') -Force
Save-Shot 'task6-tray-applied.png'

# ---------- Phase 3: force a promoted icon into the hidden panel ----------
$toggles = Get-TrayToggles
$promotedToggle = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'On' -and $_.Current.AutomationId -notlike "*$forcedShowId" })[0]
if ($null -eq $promotedToggle) { $promotedToggle = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'On' })[0] }
$forcedHideId = $promotedToggle.Current.AutomationId -replace '^TrayToggle-', ''
$forcedHideLabel = $promotedToggle.Current.Name
Toggle-Element $promotedToggle
Start-Sleep -Milliseconds 350
Assert 'toggling a promoted icon OFF' ((Get-ToggleState $promotedToggle) -eq 'Off') "$forcedHideLabel ($forcedHideId)"
Click-Apply
$json = Read-Settings
$hideIds = @($json.trayHide -split ';')
$showIds = @($json.trayShow -split ';')
Assert 'apply persisted trayHide=forced id' ($hideIds -contains $forcedHideId) "trayHide=$($json.trayHide)"
Assert 'both override lists sorted' ((Test-IdListSorted $json.trayShow) -and (Test-IdListSorted $json.trayHide)) "show=$($json.trayShow) hide=$($json.trayHide)"
Assert 'show override kept while hiding another' ($showIds -contains $forcedShowId) "trayShow=$($json.trayShow)"

# ---------- Phase 4: 恢复系统默认 ----------
Invoke-Element (Find-ByName '恢复系统默认' ([System.Windows.Automation.ControlType]::Button))
Start-Sleep -Milliseconds 500
$toggles = Get-TrayToggles
$onCount = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'On' }).Count
$offCount = @($toggles | Where-Object { (Get-ToggleState $_) -eq 'Off' }).Count
Assert '恢复系统默认 restores 9 promoted / 24 hidden' (($toggles.Count -eq 33) -and ($onCount -eq 9) -and ($offCount -eq 24)) "on=$onCount off=$offCount"
Click-Apply
$json = Read-Settings
Assert 'apply persisted empty trayShow/trayHide' (($json.trayShow -eq '') -and ($json.trayHide -eq '')) "trayShow='$($json.trayShow)' trayHide='$($json.trayHide)'"

# ---------- Phase 5: 刷新图标 ----------
Invoke-Element (Find-ByName '刷新图标' ([System.Windows.Automation.ControlType]::Button))
Start-Sleep -Milliseconds 900
$toggles = Get-TrayToggles
Assert '刷新图标 keeps the 33-icon list' ($toggles.Count -eq 33) "count=$($toggles.Count)"
$summary = Get-Summary '^共 \d+ 个图标'
Assert 'refresh restores Windows defaults' ($null -ne $summary -and $summary -match '^共 33 个图标 · 顶栏显示 9 · 隐藏面板 24$') "$summary"

# ---------- Phase 6: screenshots of the new UI ----------
Scroll-Element '设置页面滚动区' 24
Start-Sleep -Milliseconds 500
Save-Shot 'task6-network-and-spacing.png'
Scroll-Element '设置页面滚动区' 57
Start-Sleep -Milliseconds 500
Save-Shot 'task6-tray-card.png'
Scroll-Element '托盘图标列表滚动区' 100
Start-Sleep -Milliseconds 500
Save-Shot 'task6-tray-list-bottom.png'

Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ShotDir 'task6-settings.json') -Force

if (-not $process.HasExited) { try { $process.Kill(); $process.WaitForExit(2000) | Out-Null } catch { } }
Write-Host "FAILURES: $failures"
exit $failures
