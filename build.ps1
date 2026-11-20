#Requires -Version 5.1
<#
.SYNOPSIS
  Build, verify and package a SuperMenu release.

.DESCRIPTION
  One-stop release script:
    1. (optionally) clean build output;
    2. build native bar + settings + media helper through scripts/build.ps1;
    3. run the settings --selftest and fail on errors;
    4. validate the assembled dist/;
    5. create release/SuperMenu-<version>-win-x64.zip and a SHA-256 file.

.PARAMETER Version
  Release version used in VERSION.txt, the release folder and zip name.

.PARAMETER Clean
  Remove native/build, dist and release before building.

.PARAMETER SkipBuild
  Do not compile; verify and repackage the existing dist/.

.PARAMETER SkipZip
  Build and verify, but do not create a release zip.

.EXAMPLE
  .\build.ps1 -Version 1.0.0 -Clean

.EXAMPLE
  .\build.ps1 -SkipBuild
#>
param(
    [string]$Version = '1.0.0',
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$SkipZip
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$releaseRoot = Join-Path $root 'release'
$stageName = "SuperMenu-$Version-win-x64"
$stage = Join-Path $releaseRoot $stageName
$zipPath = Join-Path $releaseRoot "$stageName.zip"
$hashPath = Join-Path $releaseRoot "$stageName.sha256"
$buildScript = Join-Path $root 'scripts\build.ps1'

if ($Version -notmatch '^\d+\.\d+\.\d+([-.][0-9A-Za-z.-]+)?$') {
    throw "Invalid version '$Version'. Use a SemVer-like value, for example 1.0.0."
}
if (-not (Test-Path $buildScript)) {
    throw "Build script not found: $buildScript"
}

function Assert-UnderRoot([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    $rootFull = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to touch path outside the repository: $full"
    }
    return $full
}

function Stop-SuperMenuProcesses {
    foreach ($name in @('SuperMenuBar', 'SuperMenu.Media', 'SuperMenu.Settings')) {
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        if ($processes.Count -gt 0) {
            Write-Host "Stopping $name ($($processes.Count) process(es))..." -ForegroundColor Yellow
            $processes | Stop-Process -Force
            Start-Sleep -Milliseconds 400
        }
    }
}

Write-Host '== SuperMenu release build ==' -ForegroundColor Cyan
Write-Host "  version: $Version"
Write-Host "  root:    $root"

if ($Clean) {
    Stop-SuperMenuProcesses
    foreach ($path in @((Join-Path $root 'native\build'), $dist, $releaseRoot)) {
        $full = Assert-UnderRoot $path
        if (Test-Path $full) {
            Write-Host "-- remove $full" -ForegroundColor Yellow
            Remove-Item -LiteralPath $full -Recurse -Force
        }
    }
}

if (-not $SkipBuild) {
    Stop-SuperMenuProcesses
    & $buildScript -Zip -Version $Version
    if ($LASTEXITCODE -ne 0) {
        throw 'scripts/build.ps1 failed.'
    }
}
else {
    Write-Host '-- SkipBuild: using existing dist/' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Verify dist
# ---------------------------------------------------------------------------
Write-Host '-- verify dist' -ForegroundColor Cyan
$required = @(
    'SuperMenuBar.exe',
    'README.md',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'VERSION.txt',
    'Settings\SuperMenu.Settings.exe',
    'Media\SuperMenu.Media.exe',
    'Fonts\SegoeIcons.ttf',
    'Fonts\LICENSE-HarmonyOS-Sans.txt',
    'docs\TRAY.md'
)
foreach ($relative in $required) {
    $full = Join-Path $dist $relative
    if (-not (Test-Path $full)) {
        throw "Missing release file: dist\$relative"
    }
}

$fontFiles = @(Get-ChildItem (Join-Path $dist 'Fonts') -Filter 'HarmonyOS_Sans_SC_*.ttf' -File -ErrorAction SilentlyContinue)
if ($fontFiles.Count -ne 4) {
    throw "Expected 4 HarmonyOS Sans SC font files, found $($fontFiles.Count)."
}
if (Get-ChildItem $dist -Recurse -File -Filter 'MiSans*' -ErrorAction SilentlyContinue) {
    throw 'Old MiSans font files are still present in dist.'
}
$settingsFonts = @(Get-ChildItem (Join-Path $dist 'Settings\Assets\Fonts') -Filter 'HarmonyOS_Sans_SC_*.ttf' -File -ErrorAction SilentlyContinue)
if ($settingsFonts.Count -ne 4) {
    throw "Expected 4 HarmonyOS Sans SC font files in Settings, found $($settingsFonts.Count)."
}

# Run the settings app's contract/integrity selftest.
$selftestJson = Join-Path $env:TEMP "supermenu-selftest-$PID.json"
if (Test-Path $selftestJson) { Remove-Item -LiteralPath $selftestJson -Force }
$settingsExe = Join-Path $dist 'Settings\SuperMenu.Settings.exe'
$selftest = Start-Process -FilePath $settingsExe -ArgumentList @('--selftest', $selftestJson) -Wait -PassThru
if (-not (Test-Path $selftestJson)) {
    throw "Settings selftest did not write $selftestJson (exit code $($selftest.ExitCode))."
}
$result = Get-Content -LiteralPath $selftestJson -Raw | ConvertFrom-Json
Remove-Item -LiteralPath $selftestJson -Force -ErrorAction SilentlyContinue
if ($selftest.ExitCode -ne 0 -or -not $result.ok) {
    throw "Settings selftest failed (exit $($selftest.ExitCode), ok=$($result.ok))."
}
Write-Host "  selftest: $($result.checks.Count) checks passed" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Assemble release folder and zip
# ---------------------------------------------------------------------------
if (-not $SkipZip) {
    Write-Host '-- assemble release package' -ForegroundColor Cyan
    $releaseFull = Assert-UnderRoot $releaseRoot
    New-Item -ItemType Directory -Force $releaseFull | Out-Null
    if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $stage | Out-Null

    # Copy everything except the low-level build zip; release gets a clean archive.
    $distFull = (Resolve-Path $dist).Path
    Get-ChildItem -LiteralPath $distFull -Force | Where-Object {
        $_.Name -notlike '*.zip'
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stage -Recurse -Force
    }
    Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value "SuperMenu $Version" -Encoding utf8
    # Source-build symbols are not part of the portable release.
    Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.pdb' -ErrorAction SilentlyContinue |
        Remove-Item -Force

    if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $stage,
        $zipPath,
        [IO.Compression.CompressionLevel]::Optimal,
        $false
    )
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath $hashPath -Value "$hash  $([IO.Path]::GetFileName($zipPath))" -Encoding ascii

    $zipMb = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Host "  release: $zipPath ($zipMb MB)" -ForegroundColor Green
    Write-Host "  sha256:  $hash" -ForegroundColor Green
}
else {
    Write-Host '-- SkipZip: no release zip created' -ForegroundColor Yellow
}

$exeMb = [math]::Round((Get-Item (Join-Path $dist 'SuperMenuBar.exe')).Length / 1MB, 2)
Write-Host "== release ready: dist ($exeMb MB bar) ==" -ForegroundColor Cyan
Write-Host "Run: .\dist\SuperMenuBar.exe"
