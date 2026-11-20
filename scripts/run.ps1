#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'dist\SuperMenuBar.exe'
if (-not (Test-Path $exe)) {
    throw "dist\SuperMenuBar.exe not found. Run scripts\build.ps1 first."
}
$running = Get-Process -Name SuperMenuBar -ErrorAction SilentlyContinue
if ($running) {
    # A second instance forwards --settings to the running bar.
    Start-Process -FilePath $exe -ArgumentList '--settings' -WorkingDirectory (Split-Path $exe)
    Write-Host 'SuperMenu is already running; opened settings.'
} else {
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
    Write-Host 'SuperMenu started.'
}
