<#
.SYNOPSIS
Build Plugins\Monolith\Binaries\monolith_watchdog.exe from Scripts\monolith_watchdog.py.

.DESCRIPTION
Creates an isolated Python virtual environment under Plugins\Monolith\Saved\Build\MonolithWatchdog,
installs PyInstaller there, builds a one-file console executable, and copies it to
Plugins\Monolith\Binaries\monolith_watchdog.exe.
#>
[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$scriptRoot = $PSScriptRoot
$pluginRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot '..')).Path
$source = Join-Path $scriptRoot 'monolith_watchdog.py'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Source wrapper not found: $source"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $pluginRoot 'Binaries\monolith_watchdog.exe'
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$buildRoot = Join-Path $pluginRoot 'Saved\Build\MonolithWatchdog'
$venvRoot = Join-Path $buildRoot 'venv'
$distRoot = Join-Path $buildRoot 'dist'
$workRoot = Join-Path $buildRoot 'work'
$specRoot = Join-Path $buildRoot 'spec'

New-Item -ItemType Directory -Path $buildRoot, $distRoot, $workRoot, $specRoot -Force | Out-Null

$venvPython = Join-Path $venvRoot 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $venvPython -PathType Leaf)) {
    & $Python -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create Python venv at $venvRoot"
    }
}

& $venvPython -m pip install --upgrade pip pyinstaller
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install PyInstaller in $venvRoot"
}

& $venvPython -m PyInstaller `
    --onefile `
    --console `
    --clean `
    --name monolith_watchdog `
    --distpath $distRoot `
    --workpath $workRoot `
    --specpath $specRoot `
    $source
if ($LASTEXITCODE -ne 0) {
    throw "PyInstaller build failed"
}

$builtExe = Join-Path $distRoot 'monolith_watchdog.exe'
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
    throw "Expected build output missing: $builtExe"
}

Copy-Item -LiteralPath $builtExe -Destination $OutputPath -Force
Write-Output ("RESULT=MONOLITH_WATCHDOG_BUILT output={0}" -f $OutputPath)
