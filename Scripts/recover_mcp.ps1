<#
.SYNOPSIS
Deterministic Monolith MCP recovery: probe /health, launch the host project's
headless editor wrapper when the server is down, wait for the endpoint, report.

.DESCRIPTION
Runs the documented Go-checkout recovery sequence (CLAUDE.md section 14 and
Skills/monolith-mcp) as one deterministic entry point:

  1. GET <mcp-url-with-/health> (3s timeout). 200 means the server is up.
  2. If down and -ProbeOnly: report and stop.
  3. Resolve the host checkout root (walk up from this script until a
     *.uproject is found, or use -ProjectRoot) and require
     BatchFiles/RunHeadlessEditor.bat there. No substitute editor launch is
     attempted when the wrapper is missing.
  4. Skip the launch when an UnrealEditor process already exists (a boot may
     be in progress) unless -ForceLaunch is passed.
  5. Poll /health until it answers 200 or -TimeoutSec elapses; on timeout,
     point at the newest Saved/HeadlessMcp/Logs/HeadlessEditor-*.log.

The MCP client configuration itself stays on the existing Monolith proxy; the
proxy detects the server state transition and refreshes its tool list.

.PARAMETER McpUrl
MCP endpoint (default: MONOLITH_URL env var or http://localhost:9316/mcp).

.PARAMETER TimeoutSec
Maximum seconds to wait for /health after the launch step (default 600).

.PARAMETER PollIntervalSec
Seconds between /health probes while waiting (default 5).

.PARAMETER ProbeOnly
Only report up/down; never launch anything.

.PARAMETER ForceLaunch
Launch the wrapper even when UnrealEditor processes already exist.

.PARAMETER ProjectRoot
Explicit host checkout root containing the .uproject (skips upward search).

.OUTPUTS
Line-oriented status ending in one RESULT= token.

Exit codes:
  0  MCP endpoint is up (already up, or came up after the launch step)
  2  endpoint down and -ProbeOnly was requested
  3  blocked: no host checkout / RunHeadlessEditor.bat not found
  4  blocked: wrapper exited non-zero
  5  timeout: /health did not answer 200 within -TimeoutSec
  6  editor process exited before /health answered (inspect the editor log)
#>
[CmdletBinding()]
param(
    [string]$McpUrl = $(if ($env:MONOLITH_URL) { $env:MONOLITH_URL } else { 'http://localhost:9316/mcp' }),
    [int]$TimeoutSec = 600,
    [int]$PollIntervalSec = 5,
    [switch]$ProbeOnly,
    [switch]$ForceLaunch,
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Continue'
$healthUrl = $McpUrl -replace '/mcp/?$', '/health'

function Get-MonolithHealth {
    try {
        $resp = Invoke-WebRequest -Uri $healthUrl -Method Get -TimeoutSec 3 -UseBasicParsing
        if ($resp.StatusCode -eq 200) {
            try { return $resp.Content | ConvertFrom-Json } catch { return [PSCustomObject]@{ status = 'ok' } }
        }
    }
    catch { }
    return $null
}

function Write-UpResult {
    param($Health, [int]$ElapsedSec)
    Write-Output ("INFO health url={0} version={1} tools_registered={2} pid={3} uptime_seconds={4}" -f `
            $healthUrl, $Health.version, $Health.tools_registered, $Health.pid, [int]$Health.uptime_seconds)
    Write-Output 'INFO reconnect the existing Monolith proxy/client, then re-run monolith_status() before namespace actions'
    Write-Output ("RESULT=MCP_UP elapsed_seconds={0}" -f $ElapsedSec)
}

function Resolve-HostRoot {
    if ($ProjectRoot) {
        if (-not (Test-Path $ProjectRoot)) { return $null }
        return (Resolve-Path $ProjectRoot).Path
    }
    $dir = Split-Path -Parent $PSScriptRoot   # Scripts -> plugin root, then upward
    for ($depth = 0; $depth -lt 8 -and $dir; $depth++) {
        $uproject = Get-ChildItem -Path $dir -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($uproject) { return $dir }
        $dir = Split-Path -Parent $dir
    }
    return $null
}

# Only a real editor instance can ever bind the MCP port; -game/-server instances never will.
# Get-Process answers process existence (CIM alone can transiently return nothing for a
# live process, which previously produced a false EDITOR_EXITED); CIM only classifies the
# command line, and a process whose command line is not readable stays a candidate.
function Get-EditorServerCandidates {
    $procs = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { return @() }
    $cims = @{}
    Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { $cims[[int]$_.ProcessId] = $_.CommandLine }
    return @($procs | Where-Object {
            $cmd = $cims[$_.Id]
            (-not $cmd) -or (
                (-not ($cmd -match '(?i)(^|\s)-(game|server)(\s|$)')) -and
                (-not ($cmd -match '(?i)bMcpServerEnabled\s*[:=]\s*False'))
            )
        })
}

function Get-NewestEditorLog {
    param([string]$Root)
    return Get-ChildItem -Path (Join-Path $Root 'Saved\HeadlessMcp\Logs\HeadlessEditor-*.log') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Set-HeadlessAssetEditorRestoreState {
    param([string]$Root)

    $ini = Join-Path $Root 'Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini'
    $iniDir = Split-Path -Parent $ini
    if (-not (Test-Path $iniDir)) {
        New-Item -ItemType Directory -Path $iniDir -Force | Out-Null
    }

    $lines = @()
    if (Test-Path $ini) {
        $lines = @(Get-Content -LiteralPath $ini)
    }

    $out = New-Object System.Collections.Generic.List[string]
    $inAssetSection = $false
    $inLoadingSection = $false
    $assetSectionSeen = $false
    $loadingSectionSeen = $false
    $cleanShutdownSeen = $false
    $debuggerSeen = $false
    $restoreSeen = $false

    function Complete-AssetSection {
        if ($inAssetSection) {
            if (-not $cleanShutdownSeen) { $out.Add('CleanShutdown=True') }
            if (-not $debuggerSeen) { $out.Add('DebuggerAttached=False') }
        }
    }

    function Complete-LoadingSection {
        if ($inLoadingSection -and -not $restoreSeen) {
            $out.Add('RestoreOpenAssetTabsOnRestart=NeverRestore')
        }
    }

    foreach ($line in $lines) {
        if ($line -match '^\[.+\]\s*$') {
            Complete-AssetSection
            Complete-LoadingSection
            $inAssetSection = ($line -eq '[AssetEditorSubsystem]')
            $inLoadingSection = ($line -eq '[/Script/UnrealEd.EditorLoadingSavingSettings]')
            if ($inAssetSection) {
                $assetSectionSeen = $true
                $cleanShutdownSeen = $false
                $debuggerSeen = $false
            }
            if ($inLoadingSection) {
                $loadingSectionSeen = $true
                $restoreSeen = $false
            }
            $out.Add($line)
            continue
        }

        if ($inAssetSection) {
            if ($line -match '^CleanShutdown=') {
                if (-not $cleanShutdownSeen) { $out.Add('CleanShutdown=True') }
                $cleanShutdownSeen = $true
                continue
            }
            if ($line -match '^DebuggerAttached=') {
                if (-not $debuggerSeen) { $out.Add('DebuggerAttached=False') }
                $debuggerSeen = $true
                continue
            }
            if ($line -match '^OpenAssetsAtExit=') {
                continue
            }
        }

        if ($inLoadingSection -and $line -match '^RestoreOpenAssetTabsOnRestart=') {
            if (-not $restoreSeen) { $out.Add('RestoreOpenAssetTabsOnRestart=NeverRestore') }
            $restoreSeen = $true
            continue
        }

        $out.Add($line)
    }

    Complete-AssetSection
    Complete-LoadingSection

    if (-not $assetSectionSeen) {
        if ($out.Count -gt 0 -and $out[$out.Count - 1] -ne '') { $out.Add('') }
        $out.Add('[AssetEditorSubsystem]')
        $out.Add('CleanShutdown=True')
        $out.Add('DebuggerAttached=False')
    }
    if (-not $loadingSectionSeen) {
        if ($out.Count -gt 0 -and $out[$out.Count - 1] -ne '') { $out.Add('') }
        $out.Add('[/Script/UnrealEd.EditorLoadingSavingSettings]')
        $out.Add('RestoreOpenAssetTabsOnRestart=NeverRestore')
    }

    Set-Content -LiteralPath $ini -Value $out -Encoding UTF8
}

function Initialize-HeadlessEditorConfigState {
    param([string]$Root)

    $configDir = Join-Path $Root 'Saved\HeadlessMcp\Config\WindowsEditor'
    if (-not (Test-Path $configDir)) {
        New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    }

    $layoutIni = Join-Path $configDir 'EditorLayout.ini'
    if (Test-Path $layoutIni) {
        Remove-Item -LiteralPath $layoutIni -Force
    }

    $perProjectIni = Join-Path $configDir 'EditorPerProjectUserSettings.ini'
    $perProjectLines = @(
        '[AssetEditorSubsystem]',
        'CleanShutdown=True',
        'DebuggerAttached=False',
        '',
        '[/Script/UnrealEd.EditorLoadingSavingSettings]',
        'RestoreOpenAssetTabsOnRestart=NeverRestore'
    )
    Set-Content -LiteralPath $perProjectIni -Value $perProjectLines -Encoding UTF8

    return [PSCustomObject]@{
        LayoutIni = $layoutIni
        PerProjectIni = $perProjectIni
    }
}

$health = Get-MonolithHealth
if ($health) {
    Write-UpResult -Health $health -ElapsedSec 0
    exit 0
}

if ($ProbeOnly) {
    Write-Output ("RESULT=MCP_DOWN probe_only=true url={0}" -f $healthUrl)
    exit 2
}

$hostRoot = Resolve-HostRoot
if (-not $hostRoot) {
    Write-Output 'RESULT=BLOCKED reason=host_root_not_found detail=no *.uproject found upward from the script and no -ProjectRoot given'
    exit 3
}
$wrapper = Join-Path $hostRoot 'BatchFiles\RunHeadlessEditor.bat'
if (-not (Test-Path $wrapper)) {
    Write-Output ("RESULT=BLOCKED reason=headless_wrapper_missing path={0}" -f $wrapper)
    exit 3
}

$editorProcs = Get-EditorServerCandidates
if ($editorProcs.Count -gt 0 -and -not $ForceLaunch) {
    Write-Output ("INFO editor_processes_detected pids={0} skipping_launch=true (boot may be in progress; use -ForceLaunch to launch anyway)" -f (($editorProcs.Id) -join ','))
}
else {
    Write-Output ("INFO launching {0}" -f $wrapper)
    Set-HeadlessAssetEditorRestoreState -Root $hostRoot
    $headlessConfig = Initialize-HeadlessEditorConfigState -Root $hostRoot
    $headlessArgs = @(
        ("-EditorLayoutINI={0}" -f $headlessConfig.LayoutIni),
        ("-EditorPerProjectUserSettingsINI={0}" -f $headlessConfig.PerProjectIni),
        '-ini:EditorPerProjectUserSettings:AssetEditorSubsystem:CleanShutdown=True',
        '-ini:EditorPerProjectUserSettings:[/Script/UnrealEd.EditorLoadingSavingSettings]:RestoreOpenAssetTabsOnRestart=NeverRestore'
    )
    Write-Output ("INFO launch_overrides {0}" -f ($headlessArgs -join ' '))
    # The wrapper must get its own (hidden) console: the editor it backgrounds inherits
    # that console's stdio, so piping the wrapper here would block until the editor
    # exits, and sharing this script's console group would forward a later Ctrl/kill
    # of this script's tree to the editor as ConsoleCtrl. WaitForExit() (not
    # Start-Process -Wait) is used because Windows PowerShell 5.1's -Wait waits on the
    # whole descendant tree — including the backgrounded editor — while .NET
    # WaitForExit only waits on the wrapper process itself.
    $wrapperProc = Start-Process -FilePath $wrapper -ArgumentList $headlessArgs -WorkingDirectory $hostRoot -WindowStyle Hidden -PassThru
    if (-not $wrapperProc.WaitForExit(60000)) {
        Write-Output 'RESULT=WRAPPER_FAILED exit_code=timeout detail=RunHeadlessEditor.bat did not exit within 60s'
        exit 4
    }
    if ($wrapperProc.ExitCode -ne 0) {
        Write-Output ("RESULT=WRAPPER_FAILED exit_code={0}" -f $wrapperProc.ExitCode)
        exit 4
    }
    $launchLog = Get-NewestEditorLog -Root $hostRoot
    if ($launchLog) {
        Write-Output ("INFO editor_log path={0}" -f $launchLog.FullName)
    }
}

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$nextNotice = 30
$emptyPolls = 0
while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Seconds $PollIntervalSec
    $health = Get-MonolithHealth
    if ($health) {
        Write-UpResult -Health $health -ElapsedSec ([int]$stopwatch.Elapsed.TotalSeconds)
        exit 0
    }
    if (@(Get-EditorServerCandidates).Count -eq 0) {
        # Debounce: require two consecutive empty samples before declaring death.
        $emptyPolls++
        if ($emptyPolls -ge 2) {
            $crashLog = Get-NewestEditorLog -Root $hostRoot
            if ($crashLog) {
                Write-Output ("INFO inspect_editor_log path={0}" -f $crashLog.FullName)
            }
            Write-Output ("RESULT=EDITOR_EXITED elapsed_seconds={0} detail=editor process exited before /health answered; inspect the editor log for the crash or shutdown cause" -f [int]$stopwatch.Elapsed.TotalSeconds)
            exit 6
        }
    }
    else {
        $emptyPolls = 0
    }
    if ($stopwatch.Elapsed.TotalSeconds -ge $nextNotice) {
        Write-Output ("WAITING elapsed_seconds={0} (editor boot can take minutes)" -f [int]$stopwatch.Elapsed.TotalSeconds)
        $nextNotice += 30
    }
}

$latestLog = Get-NewestEditorLog -Root $hostRoot
if ($latestLog) {
    Write-Output ("INFO inspect_editor_log path={0}" -f $latestLog.FullName)
}
Write-Output ("RESULT=MCP_TIMEOUT waited_seconds={0} url={1}" -f $TimeoutSec, $healthUrl)
exit 5
