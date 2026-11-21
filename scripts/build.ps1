#Requires -Version 5.1
<#
.SYNOPSIS
  Builds SuperMenu: native C++ bar, C# WPF settings app, and portable dist/.
.DESCRIPTION
  Requires MSYS2 UCRT64 (gcc, cmake, ninja) and the .NET 10 SDK.
  Writes dist/SuperMenuBar.exe, dist/Fonts, dist/Settings and docs.
.PARAMETER SkipSettings
  Build only the native menu bar (do not publish the WPF settings app).
.PARAMETER KeepRunning
  Do not stop a running SuperMenuBar. The final exe copy will fail if it is locked.
.PARAMETER Zip
  Also create dist/SuperMenu-<version>-win-x64.zip.
.PARAMETER Version
  Release version written to VERSION.txt and the zip name. Default 1.1.0.
#>
param(
    [switch]$SkipSettings,
    [switch]$KeepRunning,
    [switch]$Zip,
    [string]$Version = '1.1.0'
)

if ($Version -notmatch '^\d+\.\d+\.\d+([-.][0-9A-Za-z.-]+)?$') {
    throw "Invalid version '$Version'. Use a SemVer-like value, for example 1.1.0."
}

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$msys = 'C:\msys64\ucrt64'
$cmake = Join-Path $msys 'bin\cmake.exe'
$ninja = Join-Path $msys 'bin\ninja.exe'
$toolchain = Join-Path $root 'native\cmake\mingw-ucrt64.cmake'

if (-not (Test-Path $cmake)) {
    throw "MSYS2 UCRT64 cmake not found at $cmake. Install MSYS2 and mingw-w64-ucrt-x86_64-cmake first."
}
$env:PATH = "$msys\bin;$env:PATH"

Write-Host '== SuperMenu build ==' -ForegroundColor Cyan
$gccVersion = & (Join-Path $msys 'bin\g++.exe') --version | Select-Object -First 1
Write-Host "  $gccVersion"
Write-Host "  cmake: $(& $cmake --version | Select-Object -First 1)"

$running = @(Get-Process -Name SuperMenuBar -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    if ($KeepRunning) {
        Write-Warning 'SuperMenuBar is running; the final copy into dist may fail.'
    } else {
        Write-Host 'Stopping running SuperMenuBar...' -ForegroundColor Yellow
        $running | Stop-Process -Force
        Start-Sleep -Milliseconds 700
    }
}
$mediaHelpers = @(Get-Process -Name SuperMenu.Media -ErrorAction SilentlyContinue)
if ($mediaHelpers.Count -gt 0) {
    $mediaHelpers | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

# 1. Native
Write-Host '-- configure native' -ForegroundColor Cyan
& $cmake -S (Join-Path $root 'native') -B (Join-Path $root 'native\build') -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_BUILD_TYPE=Release"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

Write-Host '-- build native' -ForegroundColor Cyan
& $cmake --build (Join-Path $root 'native\build')
if ($LASTEXITCODE -ne 0) { throw 'native build failed' }

# 2. Settings
$settingsProject = Join-Path $root 'settings\SuperMenu.Settings\SuperMenu.Settings.csproj'
if (-not $SkipSettings) {
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw '.NET SDK (dotnet) is required to build the settings app.'
    }
    Write-Host '-- publish settings app' -ForegroundColor Cyan
    & dotnet publish $settingsProject -c Release -r win-x64 --self-contained false -o (Join-Path $dist 'Settings')
    if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }

    # WinRT SystemMediaTransportControls projection. This tiny helper is kept
    # out of the native process because MinGW has no C++/WinRT headers.
    Write-Host '-- publish media helper' -ForegroundColor Cyan
    $mediaProject = Join-Path $root 'media\SuperMenu.Media\SuperMenu.Media.csproj'
    & dotnet publish $mediaProject -c Release -r win-x64 --self-contained false -o (Join-Path $dist 'Media')
    if ($LASTEXITCODE -ne 0) { throw 'media helper publish failed' }
}

# 3. Assemble dist
Write-Host '-- assemble dist' -ForegroundColor Cyan
$expected = [IO.Path]::GetFullPath((Join-Path $root 'dist'))
$actual = [IO.Path]::GetFullPath($dist)
if (-not [string]::Equals($expected, $actual, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unexpected dist path'
}
if (Test-Path $dist) {
    # Keep the published Settings and Media folders that step 2 just wrote.
    Get-ChildItem -LiteralPath $dist -Force | Where-Object { $_.Name -notin @('Settings', 'Media') } | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Force $dist | Out-Null
}
New-Item -ItemType Directory -Force (Join-Path $dist 'Fonts') | Out-Null

Copy-Item (Join-Path $root 'native\build\SuperMenuBar.exe') $dist -Force
Copy-Item (Join-Path $root 'assets\fonts\HarmonyOS_Sans_SC_*.ttf') (Join-Path $dist 'Fonts') -Force
Copy-Item (Join-Path $root 'assets\fonts\LICENSE-HarmonyOS-Sans.txt') (Join-Path $dist 'Fonts') -Force
# Remove the previous interface font if an incremental dist still contains it.
Get-ChildItem (Join-Path $dist 'Fonts') -Filter 'MiSans*' -File -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem (Join-Path $dist 'Settings\Assets\Fonts') -Filter 'MiSans*' -File -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item (Join-Path $root 'assets\fonts\SegoeIcons.ttf') (Join-Path $dist 'Fonts') -Force
Copy-Item (Join-Path $root 'assets\supermenu.ico') $dist -Force
New-Item -ItemType Directory -Force (Join-Path $dist 'assets\logos') | Out-Null
Copy-Item (Join-Path $root 'assets\logos\*') (Join-Path $dist 'assets\logos') -Force
New-Item -ItemType Directory -Force (Join-Path $dist 'assets\icons\sf') | Out-Null
Copy-Item (Join-Path $root 'assets\icons\sf\*') (Join-Path $dist 'assets\icons\sf') -Force
New-Item -ItemType Directory -Force (Join-Path $dist 'assets\icons\filled') | Out-Null
Copy-Item (Join-Path $root 'assets\icons\filled\*') (Join-Path $dist 'assets\icons\filled') -Force
Copy-Item -Path (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $dist -Force
New-Item -ItemType Directory -Force (Join-Path $dist 'docs') | Out-Null
Copy-Item -Path (Join-Path $root 'docs\BUILD.md'), (Join-Path $root 'docs\ARCHITECTURE.md'), `
    (Join-Path $root 'docs\DESIGN.md'), (Join-Path $root 'docs\settings-schema.md'), `
    (Join-Path $root 'docs\VERIFICATION.md'), (Join-Path $root 'docs\TRAY.md') `
    -Destination (Join-Path $dist 'docs') -Force
Copy-Item -Path (Join-Path $root 'docs\screenshots') -Destination (Join-Path $dist 'docs\screenshots') -Recurse -Force
Set-Content -LiteralPath (Join-Path $dist 'VERSION.txt') -Value "SuperMenu $Version" -Encoding utf8

# 4. Verify
$exe = Join-Path $dist 'SuperMenuBar.exe'
if (-not (Test-Path $exe)) { throw 'SuperMenuBar.exe missing from dist' }
$size = [math]::Round((Get-Item $exe).Length / 1MB, 2)
Write-Host "  dist: $dist ($size MB bar)" -ForegroundColor Green
if (Test-Path (Join-Path $dist 'Settings\SuperMenu.Settings.exe')) {
    Write-Host '  settings: dist\Settings\SuperMenu.Settings.exe' -ForegroundColor Green
} else {
    Write-Warning 'Settings app missing from dist\Settings (built with -SkipSettings?)'
}
if (Test-Path (Join-Path $dist 'Media\SuperMenu.Media.exe')) {
    Write-Host '  media: dist\Media\SuperMenu.Media.exe' -ForegroundColor Green
} else {
    Write-Warning 'Media helper missing from dist\Media'
}

if ($Zip) {
    $zipPath = Join-Path $root "dist\SuperMenu-$Version-win-x64.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "  zip: $zipPath" -ForegroundColor Green
}

Write-Host 'Build complete. Run .\dist\SuperMenuBar.exe' -ForegroundColor Cyan
