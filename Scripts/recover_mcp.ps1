<#
.SYNOPSIS
Deterministic Monolith MCP recovery: probe /health, launch the host project's
headless editor wrapper when the server is down, wait for the endpoint, report.

.DESCRIPTION
Runs the documented checkout recovery sequence (AGENTS.md section 14 and
Skills/monolith-mcp) as one deterministic entry point:

  1. Read Saved/Monolith/Activation.ini. When server activation is off,
     report RESULT=MCP_DISABLED and perform no probe/build/launch mutation.
  2. GET <mcp-url-with-/health> (HealthTimeoutSec, default 5s). The response is healthy only when
     its JSON contract is complete and its PID is an Unreal Editor process for
     this checkout's .uproject.
  3. If down, require the MCP port to have no listener before any launch. A
     non-HTTP/non-200, malformed, multi-owner, or unreadable listener is an
     untrusted occupied endpoint and blocks recovery. A transport timeout from
     one exclusive listener PID is classified as trusted-busy only when that
     live PID is a durable Unreal Editor for this exact project. An exact
     planned-exit automation editor is classified separately and is allowed to
     finish without build/launch/stop mutation before recovery continues. In
     -ProbeOnly mode, report the health failure reason plus local candidates.
  4. Resolve the host checkout root (walk up from this script until a
     *.uproject is found, or use -ProjectRoot) and require
     Build/BatchFiles/RunHeadlessEditor.bat there. No substitute editor launch
     is attempted when the wrapper is missing.
  5. Skip the launch when an UnrealEditor process already exists (a boot may
     be in progress) unless -ForceLaunch is passed.
  6. Poll /health until it answers with the validated contract or -TimeoutSec
     elapses; on timeout,
     point at the newest Saved/HeadlessMcp/Logs/HeadlessEditor-*.log.

The MCP client configuration itself stays on the existing Monolith proxy; the
proxy detects the server state transition and refreshes its tool list.

.PARAMETER McpUrl
MCP endpoint (default: MONOLITH_URL env var or http://localhost:9316/mcp).

.PARAMETER TimeoutSec
Maximum seconds to wait for /health after the launch step (default 600).

.PARAMETER HealthTimeoutSec
Absolute timeout for each individual /health HTTP request (default 5).

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
  0  MCP endpoint is up, or server activation is intentionally disabled
  2  endpoint down, exact durable editor is transiently busy, or exact-project
     planned-exit automation is active, and -ProbeOnly was requested
  3  blocked: no host checkout / RunHeadlessEditor.bat not found, or the MCP
     port is occupied/unreadable without a fully trusted health identity
  4  blocked: wrapper exited non-zero
  5  timeout: /health did not answer 200 within -TimeoutSec
  6  editor process exited before /health answered (inspect the editor log)
#>
[CmdletBinding()]
param(
    [string]$McpUrl = $(if ($env:MONOLITH_URL) { $env:MONOLITH_URL } else { 'http://localhost:9316/mcp' }),
    [int]$TimeoutSec = 600,
    [ValidateRange(1, 60)]
    [int]$HealthTimeoutSec = 5,
    [int]$PollIntervalSec = 5,
    [switch]$ProbeOnly,
    [switch]$ForceLaunch,
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Continue'
$healthUrl = $McpUrl -replace '/mcp/?$', '/health'

$hostRoleHelperPath = Join-Path $PSScriptRoot 'mcp_host_role.ps1'
if (-not (Test-Path -LiteralPath $hostRoleHelperPath -PathType Leaf)) {
    Write-Output ("RESULT=BLOCKED reason=mcp_host_role_helper_missing path={0}" -f $hostRoleHelperPath)
    exit 3
}
. $hostRoleHelperPath

$activationHelperPath = Join-Path $PSScriptRoot 'monolith_activation_state.ps1'
if (-not (Test-Path -LiteralPath $activationHelperPath -PathType Leaf)) {
    Write-Output ("RESULT=BLOCKED reason=activation_state_helper_missing path={0}" -f $activationHelperPath)
    exit 3
}
. $activationHelperPath

function Get-MonolithHealth {
    $probe = Get-MonolithHealthProbe
    return $probe.Health
}

function Get-MonolithHealthProbe {
    try {
        $resp = Invoke-WebRequest -Uri $healthUrl -Method Get -TimeoutSec $HealthTimeoutSec -UseBasicParsing -ErrorAction Stop
        if ($resp.StatusCode -eq 200) {
            $health = $null
            try {
                $health = $resp.Content | ConvertFrom-Json -ErrorAction Stop
            }
            catch {
                return [PSCustomObject]@{
                    Health = $null
                    ErrorClass = 'invalid_json'
                    ErrorMessage = 'HTTP 200 response was not valid JSON'
                    StatusCode = [int]$resp.StatusCode
                }
            }

            $contract = Test-MonolithHealthContract -Health $health -ExpectedPort (Get-McpHealthPort)
            if (-not $contract.Valid) {
                return [PSCustomObject]@{
                    Health = $null
                    ErrorClass = 'health_contract'
                    ErrorMessage = $contract.ErrorCode
                    StatusCode = [int]$resp.StatusCode
                }
            }

            $identity = Test-MonolithHealthProcessIdentity -Health $health -Root $script:hostRoot
            if (-not $identity.Valid) {
                return [PSCustomObject]@{
                    Health = $null
                    ErrorClass = 'health_identity'
                    ErrorMessage = $identity.ErrorCode
                    StatusCode = [int]$resp.StatusCode
                    CandidatePid = [int]$health.pid
                }
            }

            $listenerIdentity = Test-MonolithHealthListenerIdentity -Health $health -Port (Get-McpHealthPort)
            if (-not $listenerIdentity.Valid) {
                return [PSCustomObject]@{
                    Health = $null
                    ErrorClass = 'health_identity'
                    ErrorMessage = $listenerIdentity.ErrorCode
                    StatusCode = [int]$resp.StatusCode
                }
            }

            return [PSCustomObject]@{
                Health = $health
                ErrorClass = $null
                ErrorMessage = $null
                StatusCode = [int]$resp.StatusCode
            }
        }
        return [PSCustomObject]@{
            Health = $null
            ErrorClass = 'http_status'
            ErrorMessage = ("HTTP status {0}" -f $resp.StatusCode)
            StatusCode = [int]$resp.StatusCode
        }
    }
    catch {
        $class = Get-HealthFailureClass -ErrorRecord $_
        $statusCode = $null
        if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
            try { $statusCode = [int]$_.Exception.Response.StatusCode } catch { $statusCode = $null }
        }
        return [PSCustomObject]@{
            Health = $null
            ErrorClass = $class
            ErrorMessage = $_.Exception.Message
            StatusCode = $statusCode
        }
    }
}

function Get-HealthFailureClass {
    param($ErrorRecord)

    $exception = $ErrorRecord.Exception
    $message = if ($exception) { [string]$exception.Message } else { '' }
    if ($exception -and $exception.Response -and $exception.Response.StatusCode) {
        return 'http_status'
    }

    $status = $null
    if ($exception -and $exception.PSObject.Properties['Status']) {
        $status = [string]$exception.Status
    }

    if ($status -match '(?i)NameResolutionFailure' -or $message -match '(?i)name resolution|no such host') {
        return 'name_resolution'
    }
    if ($status -match '(?i)Timeout' -or $message -match '(?i)timed out|timeout') {
        return 'timeout'
    }
    if ($status -match '(?i)ConnectFailure' -or $message -match '(?i)actively refused|connection refused|no connection could be made') {
        return 'connection_refused'
    }
    if ($message -match '(?i)closed|reset|forcibly') {
        return 'connection_closed'
    }
    return 'request_failed'
}

function Format-RecoverResultValue {
    param([AllowNull()]$Value, [int]$MaxLength = 160)

    if ($null -eq $Value) { return '-' }
    $text = ([string]$Value) -replace '[\r\n]+', ' '
    $text = $text.Trim()
    if ($text.Length -eq 0) { return '-' }
    if ($text.Length -gt $MaxLength) {
        $text = $text.Substring(0, [Math]::Max(0, $MaxLength - 3)) + '...'
    }
    return ($text -replace '\s+', '_')
}

function Join-RecoverIds {
    param([AllowNull()]$Values)

    $items = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [string]$_ })
    if ($items.Count -eq 0) { return '-' }
    return ($items -join ',')
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

function Get-ProjectFile {
    param([string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) { return $null }
    return Get-ChildItem -LiteralPath $Root -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Test-MonolithJsonNumber {
    param($Value, [switch]$AllowZero, [switch]$AllowFraction)

    if ($null -eq $Value -or $Value -is [bool] -or -not ($Value -is [ValueType])) {
        return $false
    }
    try {
        $number = [double]$Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or
            (-not $AllowFraction -and [Math]::Floor($number) -ne $number)) {
            return $false
        }
        return $(if ($AllowZero) { $number -ge 0 } else { $number -gt 0 })
    }
    catch {
        return $false
    }
}

function Test-MonolithHealthContract {
    param($Health, [int]$ExpectedPort)

    if ($null -eq $Health -or $Health -is [System.Array]) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_not_object' }
    }
    if ([string]$Health.status -cne 'ok') {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'status_not_ok' }
    }
    if (-not (Test-MonolithJsonNumber -Value $Health.pid)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'pid_not_positive_integer' }
    }
    if (-not (Test-MonolithJsonNumber -Value $Health.port) -or [int]$Health.port -ne $ExpectedPort) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'port_mismatch' }
    }
    if ([string]::IsNullOrWhiteSpace([string]$Health.version)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'version_missing' }
    }
    if (-not (Test-MonolithJsonNumber -Value $Health.uptime_seconds -AllowZero -AllowFraction)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'uptime_invalid' }
    }
    if (-not (Test-MonolithJsonNumber -Value $Health.tools_registered)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'tools_registered_invalid' }
    }
    if ($null -eq $Health.mcp_transport -or [string]$Health.mcp_transport.primary_route -cne '/mcp') {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'primary_route_invalid' }
    }
    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
}

function Test-MonolithEditorCommandLineForProject {
    param([string]$CommandLine, [string]$ProjectFile)
    return Test-MonolithDurableMcpHostCommandLine -CommandLine $CommandLine -ProjectFile $ProjectFile
}

function Test-MonolithHealthProcessIdentity {
    param($Health, [string]$Root, $ProcessRecord)

    $projectFile = Get-ProjectFile -Root $Root
    if (-not $projectFile) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'project_file_missing' }
    }

    $pidValue = [int]$Health.pid
    $process = $ProcessRecord
    if (-not $PSBoundParameters.ContainsKey('ProcessRecord')) {
        $process = Get-CimInstance Win32_Process -Filter ("ProcessId = {0}" -f $pidValue) -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $process) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_not_found' }
    }

    $processName = [System.IO.Path]::GetFileName([string]$process.Name)
    if ($processName -notin @('UnrealEditor.exe', 'UnrealEditor-Cmd.exe')) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_not_unreal_editor' }
    }
    $commandLine = [string]$process.CommandLine
    if ((Test-MonolithCommandLineTargetsProject -CommandLine $commandLine -ProjectFile $projectFile.FullName) -and
        (Test-MonolithEphemeralAutomationCommandLine -CommandLine $commandLine)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_ephemeral_automation' }
    }
    if (-not (Test-MonolithEditorCommandLineForProject -CommandLine $commandLine -ProjectFile $projectFile.FullName)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_wrong_project_or_mode' }
    }
    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
}

function Test-MonolithHealthListenerIdentity {
    param($Health, [int]$Port, $ListenerSummary)

    $summary = $ListenerSummary
    if (-not $PSBoundParameters.ContainsKey('ListenerSummary')) {
        $summary = Get-PortListenerSummary -Port $Port
    }
    if ($null -eq $summary -or $summary.Count -lt 0) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'listener_ownership_unavailable' }
    }
    $listenerPids = @($summary.Pids | ForEach-Object { [int]$_ } | Sort-Object -Unique)
    if ($listenerPids.Count -ne 1) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_not_exclusive_listener_owner' }
    }
    $healthPid = [int]$Health.pid
    if ($healthPid -ne $listenerPids[0]) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_not_listener_owner' }
    }
    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
}

function Test-MonolithRecoveryPortClear {
    param($ListenerSummary)

    if ($null -eq $ListenerSummary -or $ListenerSummary.Count -lt 0) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'listener_ownership_unavailable' }
    }

    $listenerPids = @($ListenerSummary.Pids | ForEach-Object { [int]$_ } | Sort-Object -Unique)
    if ([int]$ListenerSummary.Count -eq 0 -and $listenerPids.Count -eq 0) {
        return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
    }
    if ([int]$ListenerSummary.Count -le 0 -or $listenerPids.Count -eq 0) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'listener_summary_inconsistent' }
    }
    return [PSCustomObject]@{ Valid = $false; ErrorCode = 'listener_present_without_trusted_health' }
}

function Get-MonolithRecoveryPortGate {
    param([int]$Port)

    $summary = Get-PortListenerSummary -Port $Port
    $validation = Test-MonolithRecoveryPortClear -ListenerSummary $summary
    return [PSCustomObject]@{
        Valid = $validation.Valid
        ErrorCode = $validation.ErrorCode
        Count = $summary.Count
        Pids = @($summary.Pids)
        Owners = @($summary.Owners)
        Error = $summary.Error
    }
}

function Test-MonolithTrustedBusyListener {
    param(
        [string]$Root,
        $ListenerSummary,
        $ProcessRecord
    )

    if ($null -eq $ListenerSummary -or $ListenerSummary.Count -lt 0) {
        return [PSCustomObject]@{ Valid = $false; Pid = $null; ErrorCode = 'listener_ownership_unavailable' }
    }

    $listenerPids = @($ListenerSummary.Pids | ForEach-Object { [int]$_ } | Sort-Object -Unique)
    if ([int]$ListenerSummary.Count -le 0 -or $listenerPids.Count -ne 1) {
        return [PSCustomObject]@{ Valid = $false; Pid = $null; ErrorCode = 'listener_pid_not_exclusive' }
    }

    $listenerPid = [int]$listenerPids[0]
    $process = $ProcessRecord
    if (-not $PSBoundParameters.ContainsKey('ProcessRecord')) {
        try {
            $liveProcess = Get-Process -Id $listenerPid -ErrorAction Stop
            if ($liveProcess.ProcessName -notin @('UnrealEditor', 'UnrealEditor-Cmd')) {
                return [PSCustomObject]@{ Valid = $false; Pid = $listenerPid; ErrorCode = 'listener_pid_not_unreal_editor' }
            }
        }
        catch {
            return [PSCustomObject]@{ Valid = $false; Pid = $listenerPid; ErrorCode = 'listener_pid_not_found' }
        }
        $process = Get-CimInstance Win32_Process -Filter ("ProcessId = {0}" -f $listenerPid) -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $process) {
        return [PSCustomObject]@{ Valid = $false; Pid = $listenerPid; ErrorCode = 'listener_process_identity_unavailable' }
    }

    $recordPid = if ($process.PSObject.Properties['ProcessId']) { [int]$process.ProcessId } elseif ($process.PSObject.Properties['Id']) { [int]$process.Id } else { $listenerPid }
    if ($recordPid -ne $listenerPid) {
        return [PSCustomObject]@{ Valid = $false; Pid = $listenerPid; ErrorCode = 'listener_process_pid_mismatch' }
    }

    $identity = Test-MonolithHealthProcessIdentity `
        -Health ([PSCustomObject]@{ pid = $listenerPid }) `
        -Root $Root `
        -ProcessRecord $process
    if (-not $identity.Valid) {
        return [PSCustomObject]@{ Valid = $false; Pid = $listenerPid; ErrorCode = $identity.ErrorCode }
    }
    return [PSCustomObject]@{ Valid = $true; Pid = $listenerPid; ErrorCode = $null }
}

function Get-MonolithUnavailableEndpointState {
    param(
        [string]$HealthErrorClass,
        [string]$HealthErrorCode,
        $HealthStatusCode,
        $HealthPid,
        [string]$Root,
        $PortGate,
        $ProcessRecord
    )

    $ephemeralHttpOwner = $HealthStatusCode -eq 200 -and
        $HealthErrorClass -eq 'health_identity' -and
        $HealthErrorCode -eq 'health_pid_ephemeral_automation'
    if ($ephemeralHttpOwner) {
        $listenerPids = @($PortGate.Pids | ForEach-Object { [int]$_ } | Sort-Object -Unique)
        if ($listenerPids.Count -eq 1 -and $null -ne $HealthPid -and $listenerPids[0] -eq [int]$HealthPid) {
            return [PSCustomObject]@{ State = 'ephemeral_automation'; Pid = [int]$HealthPid; ErrorCode = $null }
        }
        return [PSCustomObject]@{ State = 'blocked'; Pid = $null; ErrorCode = 'ephemeral_health_listener_mismatch' }
    }

    $invalidHttpIdentity = $HealthStatusCode -eq 200 -and
        $HealthErrorClass -in @('invalid_json', 'health_contract', 'health_identity')
    if ($invalidHttpIdentity) {
        return [PSCustomObject]@{ State = 'blocked'; Pid = $null; ErrorCode = 'invalid_http_identity' }
    }
    if ($PortGate.Valid) {
        return [PSCustomObject]@{ State = 'down'; Pid = $null; ErrorCode = $null }
    }

    if ($HealthErrorClass -in @('timeout', 'request_failed', 'connection_closed')) {
        $trustedArgs = @{ Root = $Root; ListenerSummary = $PortGate }
        if ($PSBoundParameters.ContainsKey('ProcessRecord')) { $trustedArgs.ProcessRecord = $ProcessRecord }
        $trusted = Test-MonolithTrustedBusyListener @trustedArgs
        if ($trusted.Valid) {
            return [PSCustomObject]@{ State = 'trusted_busy'; Pid = $trusted.Pid; ErrorCode = $null }
        }
    }
    return [PSCustomObject]@{ State = 'blocked'; Pid = $null; ErrorCode = $PortGate.ErrorCode }
}

# Only a real editor instance can ever bind the MCP port; -game/-server instances never will.
# Get-Process answers process existence (CIM alone can transiently return nothing for a
# live process, which previously produced a false EDITOR_EXITED); CIM only classifies the
# command line, and a process whose command line is not readable stays a candidate.
function Get-EditorServerCandidates {
    param([string]$Root = $script:hostRoot)

    $projectFile = Get-ProjectFile -Root $Root
    if (-not $projectFile) { return @() }
    # An unreadable command line remains a conservative boot candidate;
    # readable processes must identify this exact project and a durable role.
    return @(Get-MonolithEditorProcessCandidates `
            -ProjectFile $projectFile.FullName `
            -Role durable `
            -AllowUnreadableDurable)
}

function Get-EphemeralAutomationCandidates {
    param([string]$Root = $script:hostRoot)

    $projectFile = Get-ProjectFile -Root $Root
    if (-not $projectFile) { return @() }
    return @(Get-MonolithEditorProcessCandidates `
            -ProjectFile $projectFile.FullName `
            -Role ephemeral_automation)
}

function Get-NewestEditorLog {
    param([string]$Root)
    return Get-ChildItem -Path (Join-Path $Root 'Saved\HeadlessMcp\Logs\HeadlessEditor-*.log') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Get-McpHealthPort {
    try {
        $uri = [System.Uri]$healthUrl
        if ($uri.Port -gt 0) { return [int]$uri.Port }
    }
    catch { }
    return 9316
}

function Get-PortListenerSummary {
    param([int]$Port)

    try {
        $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction Stop)
        $pids = @($listeners | ForEach-Object { $_.OwningProcess } | Sort-Object -Unique)
        $owners = @()
        foreach ($listenerPid in $pids) {
            try {
                $proc = Get-Process -Id $listenerPid -ErrorAction Stop
                $owners += ("{0}:{1}" -f $listenerPid, $proc.ProcessName)
            }
            catch {
                $owners += ("{0}:unknown" -f $listenerPid)
            }
        }
        return [PSCustomObject]@{
            Count = $listeners.Count
            Pids = $pids
            Owners = $owners
            Error = $null
        }
    }
    catch {
        $netstatError = $_.Exception.Message
        try {
            $netstat = Get-Command 'netstat.exe' -ErrorAction Stop
            $netstatOutput = @(& $netstat.Source -ano -p tcp 2>$null)
            if ($LASTEXITCODE -ne 0) {
                throw ("netstat exited {0}" -f $LASTEXITCODE)
            }
            $rows = @($netstatOutput | Where-Object { $_ -match (":{0}\s" -f [regex]::Escape([string]$Port)) -and $_ -match '\sLISTENING\s+(\d+)\s*$' })
            $pids = @($rows | ForEach-Object {
                    if ($_ -match '\sLISTENING\s+(\d+)\s*$') { [int]$Matches[1] }
                } | Sort-Object -Unique)
            $owners = @()
            foreach ($listenerPid in $pids) {
                try {
                    $proc = Get-Process -Id $listenerPid -ErrorAction Stop
                    $owners += ("{0}:{1}" -f $listenerPid, $proc.ProcessName)
                }
                catch {
                    $owners += ("{0}:unknown" -f $listenerPid)
                }
            }
            return [PSCustomObject]@{
                Count = $rows.Count
                Pids = $pids
                Owners = $owners
                Error = $null
            }
        }
        catch {
            $netstatError = ("{0}; netstat fallback: {1}" -f $netstatError, $_.Exception.Message)
        }
        return [PSCustomObject]@{
            Count = -1
            Pids = @()
            Owners = @()
            Error = $netstatError
        }
    }
}

function Get-HeadlessEditorCandidates {
    param([string]$Root = $script:hostRoot)

    $procs = @(Get-EditorServerCandidates -Root $Root)
    if ($procs.Count -eq 0) { return @() }

    $cims = @{}
    Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { $cims[[int]$_.ProcessId] = $_.CommandLine }
    return @($procs | Where-Object {
            $cmd = $cims[$_.Id]
            $cmd -and ($cmd -match '(?i)(-NullRHI|Saved\\HeadlessMcp|HeadlessEditor-)')
        })
}

function Get-RecoverProbeNextAction {
    param($ListenerSummary, $EditorCandidates, $HeadlessCandidates)

    if ($HeadlessCandidates.Count -gt 0) {
        return 'run_watch_mcp_or_recover_force_after_timeout'
    }
    if ($EditorCandidates.Count -gt 0) {
        return 'wait_or_run_recover_mcp_without_force'
    }
    if ($ListenerSummary.Count -gt 0) {
        return 'inspect_non_editor_listener_on_mcp_port'
    }
    return 'run_watch_mcp_or_recover_mcp'
}

function Write-DownProbeResult {
    param($Probe, $ListenerSummary)

    $port = Get-McpHealthPort
    $listeners = $ListenerSummary
    if (-not $PSBoundParameters.ContainsKey('ListenerSummary')) {
        $listeners = Get-PortListenerSummary -Port $port
    }
    $editorCandidates = @(Get-EditorServerCandidates)
    $headlessCandidates = @(Get-HeadlessEditorCandidates)
    $nextAction = Get-RecoverProbeNextAction -ListenerSummary $listeners -EditorCandidates $editorCandidates -HeadlessCandidates $headlessCandidates

    $fields = New-Object System.Collections.Generic.List[string]
    $fields.Add('RESULT=MCP_DOWN')
    $fields.Add('probe_only=true')
    $fields.Add(("url={0}" -f (Format-RecoverResultValue $healthUrl)))
    $fields.Add(("reason={0}" -f (Format-RecoverResultValue $Probe.ErrorClass)))
    if ($null -ne $Probe.StatusCode) {
        $fields.Add(("status_code={0}" -f $Probe.StatusCode))
    }
    $fields.Add(("detail={0}" -f (Format-RecoverResultValue $Probe.ErrorMessage)))
    $fields.Add(("listener_port={0}" -f $port))
    $fields.Add(("listener_count={0}" -f $listeners.Count))
    $fields.Add(("listener_pids={0}" -f (Join-RecoverIds $listeners.Pids)))
    $fields.Add(("listener_owners={0}" -f (Format-RecoverResultValue (($listeners.Owners) -join ','))))
    if ($listeners.Error) {
        $fields.Add(("listener_error={0}" -f (Format-RecoverResultValue $listeners.Error)))
    }
    $fields.Add(("editor_candidate_count={0}" -f $editorCandidates.Count))
    $fields.Add(("editor_candidate_pids={0}" -f (Join-RecoverIds ($editorCandidates | ForEach-Object { $_.Id }))))
    $fields.Add(("headless_candidate_count={0}" -f $headlessCandidates.Count))
    $fields.Add(("headless_candidate_pids={0}" -f (Join-RecoverIds ($headlessCandidates | ForEach-Object { $_.Id }))))
    $fields.Add(("next_action={0}" -f $nextAction))

    Write-Output ($fields -join ' ')
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
        '[/Script/UnrealEd.EditorStyleSettings]',
        'AssetEditorOpenLocation=NewWindow',
        '',
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

function Start-MonolithHeadlessEditorWrapper {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Wrapper,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string[]]$EditorArguments
    )

    # RunHeadlessEditor.bat explicitly owns editor-argument transport through
    # UE_EDITOR_EXTRA_ARGS. Passing the same values through Start-Process
    # -ArgumentList adds a second cmd.exe parsing boundary and can split quoted
    # INI paths or reinterpret ':'/'[' tokens before the wrapper sees them.
    $previousExtraArgs = [Environment]::GetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', 'Process')
    $combinedArgs = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($previousExtraArgs)) {
        $combinedArgs.Add($previousExtraArgs.Trim())
    }
    foreach ($argument in $EditorArguments) {
        if (-not [string]::IsNullOrWhiteSpace($argument)) {
            $combinedArgs.Add($argument.Trim())
        }
    }

    try {
        [Environment]::SetEnvironmentVariable(
            'UE_EDITOR_EXTRA_ARGS',
            ($combinedArgs -join ' '),
            'Process')
        return Start-Process `
            -FilePath $Wrapper `
            -WorkingDirectory $WorkingDirectory `
            -WindowStyle Hidden `
            -PassThru
    }
    finally {
        [Environment]::SetEnvironmentVariable(
            'UE_EDITOR_EXTRA_ARGS',
            $previousExtraArgs,
            'Process')
    }
}

$hostRoot = Resolve-HostRoot
if (-not $hostRoot) {
    Write-Output 'RESULT=BLOCKED reason=host_root_not_found detail=no *.uproject found upward from the script and no -ProjectRoot given'
    exit 3
}
$script:hostRoot = $hostRoot

$activationState = Get-MonolithActivationState -Root $hostRoot
if (-not $activationState.ServerEnabled) {
    $invalidKeys = @($activationState.InvalidKeys) -join ','
    if ([string]::IsNullOrWhiteSpace($invalidKeys)) {
        $invalidKeys = '-'
    }
    Write-Output (
        "RESULT=MCP_DISABLED desired_enabled=false mutation=none state_path={0} invalid_keys={1} next_action=run_Monolith.StartServer_in_editor_console" -f
            (Format-RecoverResultValue $activationState.StatePath), $invalidKeys)
    exit 0
}

$healthProbe = Get-MonolithHealthProbe
if ($healthProbe.Health) {
    Write-UpResult -Health $healthProbe.Health -ElapsedSec 0
    exit 0
}

$port = Get-McpHealthPort
$portGate = Get-MonolithRecoveryPortGate -Port $port
$endpointState = Get-MonolithUnavailableEndpointState `
    -HealthErrorClass $healthProbe.ErrorClass `
    -HealthErrorCode $healthProbe.ErrorMessage `
    -HealthStatusCode $healthProbe.StatusCode `
    -HealthPid $healthProbe.CandidatePid `
    -Root $hostRoot `
    -PortGate $portGate
if ($endpointState.State -eq 'trusted_busy') {
    if ($ProbeOnly) {
        Write-Output ("RESULT=MCP_BUSY reason=trusted_editor_health_unavailable health_error={0} detail={1} url={2} listener_port={3} listener_count={4} listener_pids={5} trusted_pid={6} next_action=retry_health_no_mutation" -f `
                (Format-RecoverResultValue $healthProbe.ErrorClass), (Format-RecoverResultValue $healthProbe.ErrorMessage), `
                (Format-RecoverResultValue $healthUrl), $port, $portGate.Count, (Join-RecoverIds $portGate.Pids), $endpointState.Pid)
        exit 2
    }
    Write-Output ("INFO trusted_editor_busy pid={0} health_error={1} skipping_launch=true action=wait_for_valid_health" -f `
            $endpointState.Pid, (Format-RecoverResultValue $healthProbe.ErrorClass))
}
elseif ($endpointState.State -eq 'ephemeral_automation') {
    Write-Output ("INFO ephemeral_automation_owner pid={0} skipping_launch=true action=wait_for_planned_exit" -f $endpointState.Pid)
}
elseif ($endpointState.State -eq 'blocked') {
    Write-Output ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint health_error={0} detail={1} url={2} listener_port={3} listener_count={4} listener_pids={5} listener_gate={6}" -f `
            (Format-RecoverResultValue $healthProbe.ErrorClass), (Format-RecoverResultValue $healthProbe.ErrorMessage), `
            (Format-RecoverResultValue $healthUrl), $port, $portGate.Count, (Join-RecoverIds $portGate.Pids), `
            (Format-RecoverResultValue $endpointState.ErrorCode))
    exit 3
}

$ephemeralAutomationProcs = @(Get-EphemeralAutomationCandidates -Root $hostRoot)
if ($endpointState.State -eq 'ephemeral_automation' -or $ephemeralAutomationProcs.Count -gt 0) {
    $ephemeralPids = @($ephemeralAutomationProcs.Id)
    if ($endpointState.Pid -and $endpointState.Pid -notin $ephemeralPids) {
        $ephemeralPids += $endpointState.Pid
    }
    if ($ProbeOnly) {
        Write-Output ("RESULT=MCP_EPHEMERAL_AUTOMATION reason=planned_exit_editor_active pids={0} mutation=none next_action=wait_for_exit_then_recover" -f `
                (Join-RecoverIds $ephemeralPids))
        exit 2
    }

    Write-Output ("INFO waiting_for_ephemeral_automation_exit pids={0} mutation=none" -f (Join-RecoverIds $ephemeralPids))
    $automationWait = [System.Diagnostics.Stopwatch]::StartNew()
    $automationExited = $false
    while ($automationWait.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Start-Sleep -Seconds $PollIntervalSec

        $waitProbe = Get-MonolithHealthProbe
        if ($waitProbe.Health) {
            Write-UpResult -Health $waitProbe.Health -ElapsedSec ([int]$automationWait.Elapsed.TotalSeconds)
            exit 0
        }

        $waitPortGate = Get-MonolithRecoveryPortGate -Port $port
        $remainingAutomation = @(Get-EphemeralAutomationCandidates -Root $hostRoot)
        if ($remainingAutomation.Count -eq 0 -and $waitPortGate.Valid) {
            Write-Output ("INFO ephemeral_automation_exited elapsed_seconds={0} action=launch_durable_headless" -f `
                    [int]$automationWait.Elapsed.TotalSeconds)
            $automationExited = $true
            break
        }

        $waitState = Get-MonolithUnavailableEndpointState `
            -HealthErrorClass $waitProbe.ErrorClass `
            -HealthErrorCode $waitProbe.ErrorMessage `
            -HealthStatusCode $waitProbe.StatusCode `
            -HealthPid $waitProbe.CandidatePid `
            -Root $hostRoot `
            -PortGate $waitPortGate
        if ($waitState.State -eq 'blocked') {
            Write-Output ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint phase=automation_handoff health_error={0} detail={1} listener_port={2} listener_pids={3} listener_gate={4}" -f `
                    (Format-RecoverResultValue $waitProbe.ErrorClass), (Format-RecoverResultValue $waitProbe.ErrorMessage), `
                    $port, (Join-RecoverIds $waitPortGate.Pids), (Format-RecoverResultValue $waitState.ErrorCode))
            exit 3
        }
    }
    if (-not $automationExited) {
        $remainingAutomation = @(Get-EphemeralAutomationCandidates -Root $hostRoot)
        Write-Output ("RESULT=MCP_TIMEOUT reason=ephemeral_automation_did_not_exit waited_seconds={0} pids={1}" -f `
                $TimeoutSec, (Join-RecoverIds $remainingAutomation.Id))
        exit 5
    }
}

if ($ProbeOnly) {
    Write-DownProbeResult -Probe $healthProbe -ListenerSummary $portGate
    exit 2
}

$wrapper = Join-Path $hostRoot 'Build\BatchFiles\RunHeadlessEditor.bat'
if (-not (Test-Path $wrapper)) {
    Write-Output ("RESULT=BLOCKED reason=headless_wrapper_missing path={0}" -f $wrapper)
    exit 3
}

$editorProcs = Get-EditorServerCandidates
if ($endpointState.State -eq 'trusted_busy') {
    Write-Output ("INFO trusted_editor_process pid={0} skipping_launch=true force_launch_ignored=true" -f $endpointState.Pid)
}
elseif ($editorProcs.Count -gt 0 -and -not $ForceLaunch) {
    Write-Output ("INFO editor_processes_detected pids={0} skipping_launch=true (boot may be in progress; use -ForceLaunch to launch anyway)" -f (($editorProcs.Id) -join ','))
}
else {
    # Close the probe-to-launch race as far as a user-space supervisor can: a
    # listener that appeared since the initial health sample blocks before any
    # config write or process launch. The new endpoint must first prove the
    # complete health/process/listener contract on a later invocation.
    $preLaunchPortGate = Get-MonolithRecoveryPortGate -Port $port
    if (-not $preLaunchPortGate.Valid) {
        Write-Output ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint phase=before_launch listener_port={0} listener_count={1} listener_pids={2} listener_gate={3}" -f `
                $port, $preLaunchPortGate.Count, (Join-RecoverIds $preLaunchPortGate.Pids), `
                (Format-RecoverResultValue $preLaunchPortGate.ErrorCode))
        exit 3
    }
    Write-Output ("INFO launching {0}" -f $wrapper)
    Set-HeadlessAssetEditorRestoreState -Root $hostRoot
    $headlessConfig = Initialize-HeadlessEditorConfigState -Root $hostRoot
    $headlessArgs = @(
        ('-EditorLayoutINI="{0}"' -f $headlessConfig.LayoutIni),
        ('-EditorPerProjectUserSettingsINI="{0}"' -f $headlessConfig.PerProjectIni),
        '-ini:EditorPerProjectUserSettings:/Script/UnrealEd.EditorStyleSettings:AssetEditorOpenLocation=NewWindow',
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
    $wrapperProc = Start-MonolithHeadlessEditorWrapper `
        -Wrapper $wrapper `
        -WorkingDirectory $hostRoot `
        -EditorArguments $headlessArgs
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
        $healthyLog = Get-NewestEditorLog -Root $hostRoot
        if ($healthyLog) {
            Write-Output ("INFO editor_log_current path={0}" -f $healthyLog.FullName)
        }
        Write-UpResult -Health $health -ElapsedSec ([int]$stopwatch.Elapsed.TotalSeconds)
        exit 0
    }
    if (@(Get-EditorServerCandidates -Root $hostRoot).Count -eq 0) {
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
