<#
.SYNOPSIS
Keep the Monolith editor-backed MCP endpoint online for agent work.

.DESCRIPTION
Long-running watchdog for project checkouts that use Monolith through the
editor-hosted HTTP MCP endpoint. The script probes /health repeatedly and accepts
it only when the complete JSON contract and reported Unreal Editor PID identify
this checkout's .uproject and exclusively own the relevant listener PID set.
When health is not trusted, any occupied or unreadable MCP listener blocks
before build, launch, or process-stop mutations. When the endpoint is down, the
port is provably clear, and no current-project editor-server candidate remains,
the watchdog runs the host project's primary editor UBT build, then delegates
the launch/wait/reconnect sequence to Scripts/recover_mcp.ps1.

When the endpoint is healthy, the same long-running process can perform one
daily Monolith index maintenance pass. By default this runs at 05:00 Korea
Standard Time, starts incremental asset/source indexing through the bridge
namespace, waits for those indexes to go idle, then refreshes the derived
Saved/graph.db export through monolith_query.exe with its cooldown gate intact.

This script does not replace recover_mcp.ps1. It is a supervisor for the common
Codex/direct-client pain point where the endpoint dies between agent calls.

.PARAMETER McpUrl
MCP endpoint (default: MONOLITH_URL env var or http://localhost:9316/mcp).

.PARAMETER PollIntervalSec
Seconds between healthy-state probes (default 15).

.PARAMETER RecoverTimeoutSec
Maximum seconds recover_mcp.ps1 may wait for /health after launch (default 600).

.PARAMETER RecoverPollIntervalSec
Seconds between recover_mcp.ps1 health probes (default 5).

.PARAMETER NoBuildBeforeRestart
Skip the UBT build step before restart. Use only when intentionally testing
launch behavior; normal agent operation should build before restart.

.PARAMETER ProbeOnly
Report one watchdog health/process sample and exit. Never builds, recovers, or
launches the editor.

.PARAMETER ProbeBuildLocksOnly
Probe the UBT link outputs (project and plugin UnrealEditor-*.dll files) for
processes that hold them open and exit. Exit 0 when all outputs are free,
exit 2 when at least one is locked, and exit 11 when an unexpected write-probe
error prevents a complete verdict. Never builds, recovers, or launches the
editor. Use to verify the DLL-lock preflight against a live editor or game client.

.PARAMETER MaxRestartAttempts
Maximum build+restart cycles before backing off. 0 means unlimited. Past the
limit the watchdog stops attempting restarts and escalates its probe sleep
(exponential backoff up to -RestartLimitBackoffMaxSec) instead of spinning; a
later healthy probe resets the budget.

.PARAMETER RestartLimitBackoffMaxSec
Upper bound in seconds for the escalated probe sleep after MaxRestartAttempts
is exceeded. Default 600.

.PARAMETER BuildFailureBackoffMaxSec
Upper bound in seconds for the escalated probe sleep after consecutive
pre-restart build failures or locked-DLL build blocks. Starting from the second
consecutive failure the sleep doubles from PollIntervalSec up to this cap; a
successful build or a healthy probe resets the streak. Default 600.

.PARAMETER RecoverInvokeGraceSec
Extra seconds granted to one recover_mcp.ps1 child invocation beyond
-RecoverTimeoutSec before the watchdog kills it and reports
RESULT=RECOVER_TIMEOUT. Default 300.

.PARAMETER Once
Run one probe/recover cycle and exit.

.PARAMETER DisableDailyReindex
Disable the daily asset/source/graph maintenance pass.

.PARAMETER DailyReindexTime
Daily maintenance start time in HH:mm, interpreted in DailyReindexTimeZone
(default 05:00).

.PARAMETER DailyReindexTimeZone
Windows time-zone id for DailyReindexTime (default Korea Standard Time).

.PARAMETER DailyReindexMode
Indexing mode: incremental (default) or full.

.PARAMETER DailyReindexTargets
Maintenance targets: assets, source, graph. Default is all three.

.PARAMETER DailyReindexWaitTimeoutSec
Maximum seconds to wait for asset/source indexing to become idle before graph
maintenance (default 1800).

.PARAMETER DailyReindexWaitPollSec
Seconds between bridge.get_index_status polls while waiting (default 10).

.PARAMETER DailyReindexActionTimeoutSec
HTTP timeout for a single MCP action call (default 120).

.PARAMETER DailyGraphCooldownSeconds
Cooldown passed to source build_crg_graph for graph.db maintenance (default
1800). Use 0 only for intentional diagnostics.

.PARAMETER RunDailyReindexNow
Run one maintenance pass as soon as the MCP endpoint is healthy, ignoring the
daily schedule. Combine with -Once for a smoke test.

.PARAMETER SkipRestartReindex
Skip the restart recovery indexing pass. Normal restart recovery runs source and
graph maintenance before launch and asset maintenance after MCP health returns.
Pre-launch maintenance failure does not prevent endpoint recovery; requested
targets are retried after health returns.

.PARAMETER RestartReindexMode
Indexing mode for restart-triggered maintenance: incremental (default) or full.

.PARAMETER RestartReindexTargets
Restart-triggered maintenance targets. Default is assets, source, graph.

.PARAMETER ProjectRoot
Explicit host checkout root containing the .uproject (skips upward search).

.OUTPUTS
Line-oriented status in `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][EventType]`
format. Watchdog event names are PascalCase; fields and enum-like values are
lowerCamel. RESULT tokens select the event name and are not repeated as fields.
Payloads longer than 240 characters, or payloads that already contain line
breaks, are printed as a header line followed by the payload on the next line.
The same structured events are appended to
`<plugin>/Logs/<yyyyMMdd>/watchdog.jsonl`.
Notable events:
  WatchdogStart         watchdog instance started (instance boundary marker)
  McpUp                 endpoint is reachable
  McpDown               endpoint is down in -ProbeOnly mode
  BuildFailed           UBT failed, editor was not restarted
  BlockedDllLocked      UBT link outputs are held by another process; the
                        build was skipped instead of failing with LNK1104
  BuildLocksPresent     -ProbeBuildLocksOnly found locked link outputs
  BuildLocksClear       -ProbeBuildLocksOnly found all link outputs free
  BuildLockProbeFailed  an unexpected write-probe error made the scan
                        incomplete; UBT was not started
  RestartLimit          MaxRestartAttempts exceeded; probe sleep backs off
  RestartAttemptsReset  healthy probe cleared the restart budget
  RecoverTimeout        recover child exceeded its invoke timeout and was killed
  Fatal                 unhandled terminating error; watchdog exits 9
  DailyReindexOk        scheduled/manual index maintenance succeeded
  DailyReindexFailed    scheduled/manual index maintenance failed
  RestartReindexOk      restart-triggered index maintenance succeeded
  RestartReindexFailed  restart-triggered index maintenance failed
  Blocked               required project/wrapper/build files are missing, or an
                        occupied endpoint fails trusted project/process identity

Exit codes:
  0  endpoint is up, or recover cycle succeeded in -Once mode, or
     -ProbeBuildLocksOnly found all link outputs free
  2  endpoint down and -ProbeOnly was requested, or -ProbeBuildLocksOnly
     found locked link outputs
  3  blocked: host root, .uproject, resolver, UBT, recover script, or trusted
     endpoint/listener identity missing
  4  build failed before restart
  7  restart limit reached
  8  index maintenance failed in -Once mode
  9  unhandled terminating error (RESULT=FATAL logged as the last line)
  10 build skipped: UBT link outputs are locked by another process
  11 build skipped: an unexpected DLL write-probe error prevented a complete scan
  otherwise recover_mcp.ps1 exit code when -Once is used and recovery fails
  (a recover child killed on timeout surfaces as recover exit code 124)
#>
[CmdletBinding()]
param(
    [string]$McpUrl = $(if ($env:MONOLITH_URL) { $env:MONOLITH_URL } else { 'http://localhost:9316/mcp' }),
    [int]$PollIntervalSec = 15,
    [int]$RecoverTimeoutSec = 600,
    [int]$RecoverPollIntervalSec = 5,
    [switch]$NoBuildBeforeRestart,
    [switch]$ProbeOnly,
    [switch]$ProbeBuildLocksOnly,
    [int]$MaxRestartAttempts = 0,
    [int]$RestartLimitBackoffMaxSec = 600,
    [int]$BuildFailureBackoffMaxSec = 600,
    [int]$RecoverInvokeGraceSec = 300,
    [switch]$Once,
    [switch]$DisableDailyReindex,
    [string]$DailyReindexTime = '05:00',
    [string]$DailyReindexTimeZone = 'Korea Standard Time',
    [ValidateSet('incremental', 'full')]
    [string]$DailyReindexMode = 'incremental',
    [ValidateSet('assets', 'source', 'graph')]
    [string[]]$DailyReindexTargets = @('assets', 'source', 'graph'),
    [int]$DailyReindexWaitTimeoutSec = 1800,
    [int]$DailyReindexWaitPollSec = 10,
    [int]$DailyReindexActionTimeoutSec = 120,
    [int]$DailyGraphCooldownSeconds = 1800,
    [switch]$RunDailyReindexNow,
    [switch]$SkipRestartReindex,
    [ValidateSet('incremental', 'full')]
    [string]$RestartReindexMode = 'incremental',
    [ValidateSet('assets', 'source', 'graph')]
    [string[]]$RestartReindexTargets = @('assets', 'source', 'graph'),
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Continue'
$healthUrl = $McpUrl -replace '/mcp/?$', '/health'
$script:lastKnownMcpPid = $null
$script:watchdogInlinePayloadLimit = 240
$script:watchdogLogRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'Logs'

function ConvertTo-WatchdogCase {
    param(
        [string]$Text,
        [switch]$Pascal
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return ''
    }

    $parts = @([regex]::Split($Text.Trim(), '[^A-Za-z0-9]+') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($parts.Count -eq 0) {
        return ''
    }

    $converted = @()
    foreach ($part in $parts) {
        if ($part -match '^[0-9]+$') {
            $converted += $part
            continue
        }

        $word = $part
        if ($word -cmatch '^[A-Z0-9]+$' -or $word -cmatch '^[a-z0-9]+$') {
            $word = $word.ToLowerInvariant()
        }

        $head = $word.Substring(0, 1).ToUpperInvariant()
        $tail = if ($word.Length -gt 1) { $word.Substring(1) } else { '' }
        $converted += ($head + $tail)
    }

    $result = ($converted -join '')
    if (-not $Pascal -and $result.Length -gt 0) {
        $head = $result.Substring(0, 1).ToLowerInvariant()
        $tail = if ($result.Length -gt 1) { $result.Substring(1) } else { '' }
        return ($head + $tail)
    }
    return $result
}

function ConvertTo-WatchdogFieldValue {
    param(
        [string]$Name,
        [string]$Value
    )

    if ($null -eq $Value) {
        return ''
    }

    $text = ([string]$Value).Trim()
    if ($text -match '^(?i:true|false)$') {
        return $text.ToLowerInvariant()
    }

    if ($text -match '^-?\d+(\.\d+)?$') {
        return $text
    }

    $enumLikeFields = @('result', 'reason', 'phase', 'action', 'mode', 'scope', 'targets')
    if ($enumLikeFields -notcontains $Name) {
        return $text
    }

    if ($text -match '[\s:=\\/]' -or [string]::IsNullOrWhiteSpace($text)) {
        return $text
    }

    if ($text -match '^[A-Za-z][A-Za-z0-9_-]*(,[A-Za-z][A-Za-z0-9_-]*)+$') {
        return ((@($text -split ',') | ForEach-Object { ConvertTo-WatchdogCase $_ }) -join ',')
    }

    if ($text -match '^[A-Za-z][A-Za-z0-9_-]*$') {
        return (ConvertTo-WatchdogCase $text)
    }

    return $text
}

function Format-WatchdogLogValue {
    param([string]$Value)

    $text = if ($null -eq $Value) { '' } else { [string]$Value }
    if ($text -match '[\s"=]' -or $text.Length -eq 0) {
        return ('"{0}"' -f ($text -replace '"', '\"'))
    }
    return $text
}

function Format-WatchdogTimestamp {
    param([datetime]$Date = (Get-Date))

    return $Date.ToString('yyyy-MM-dd HH:mm:ss')
}

function Format-WatchdogDuration {
    param([string]$Seconds)

    if ([string]::IsNullOrWhiteSpace($Seconds)) {
        return ''
    }

    try {
        $totalSeconds = [int64][Math]::Max(0, [double]::Parse($Seconds, [Globalization.CultureInfo]::InvariantCulture))
    }
    catch {
        return $Seconds
    }

    $span = [TimeSpan]::FromSeconds($totalSeconds)
    $days = [int][Math]::Floor($span.TotalDays)
    return ('{0}D/{1:00}:{2:00}:{3:00}' -f $days, $span.Hours, $span.Minutes, $span.Seconds)
}

function ConvertFrom-WatchdogMessage {
    param([string]$Message)

    $fields = [ordered]@{}
    $eventToken = $null
    $text = if ($null -eq $Message) { '' } else { $Message.Trim() }
    $pattern = '(?ms)(?:^|\s)(?<key>[A-Za-z][A-Za-z0-9_.-]*)=(?<value>.*?)(?=\s+[A-Za-z][A-Za-z0-9_.-]*=|$)'
    $matches = [regex]::Matches($text, $pattern)

    if ($matches.Count -gt 0) {
        $prefix = $text.Substring(0, $matches[0].Index).Trim()
        if (-not [string]::IsNullOrWhiteSpace($prefix)) {
            $eventToken = $prefix
        }

        foreach ($match in $matches) {
            $rawName = $match.Groups['key'].Value
            $name = if ($rawName -ceq 'RESULT') { 'result' } else { ConvertTo-WatchdogCase $rawName }
            $fields[$name] = ConvertTo-WatchdogFieldValue -Name $name -Value $match.Groups['value'].Value
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($text)) {
        $eventToken = $text
    }

    if ([string]::IsNullOrWhiteSpace($eventToken) -and $fields.Contains('result')) {
        $eventToken = $fields['result']
    }

    if ([string]::IsNullOrWhiteSpace($eventToken)) {
        $eventToken = 'watchdog'
    }

    return [PSCustomObject]@{
        Event  = ConvertTo-WatchdogCase $eventToken -Pascal
        Fields = $fields
    }
}

function Write-WatchdogJsonLog {
    param(
        [datetime]$Timestamp,
        [string]$McpPid,
        [string]$Event,
        [System.Collections.IDictionary]$Fields,
        [string]$Message
    )

    try {
        $dateFolder = Join-Path $script:watchdogLogRoot $Timestamp.ToString('yyyyMMdd')
        [void][System.IO.Directory]::CreateDirectory($dateFolder)
        $path = Join-Path $dateFolder 'watchdog.jsonl'

        $jsonFields = [ordered]@{}
        foreach ($field in $Fields.GetEnumerator()) {
            $jsonFields[$field.Key] = $field.Value
        }

        $record = [ordered]@{
            timestamp        = $Timestamp.ToString('o')
            displayTimestamp = (Format-WatchdogTimestamp -Date $Timestamp)
            pid              = $(if ($McpPid -eq '-') { $null } else { $McpPid })
            event            = $Event
            fields           = $jsonFields
            message          = $Message
        }

        $json = $record | ConvertTo-Json -Depth 8 -Compress
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::AppendAllText($path, ($json + [Environment]::NewLine), $utf8NoBom)
    }
    catch {
        [Console]::Error.WriteLine(("watchdog_jsonl_write_failed path={0} error={1}" -f `
                    (Join-Path $script:watchdogLogRoot ((Get-Date).ToString('yyyyMMdd') + '\watchdog.jsonl')),
                    (Format-WatchdogValue $_.Exception.Message)))
    }
}

function Write-Watchdog {
    param([string]$Message)
    $now = Get-Date
    $stamp = Format-WatchdogTimestamp -Date $now
    $entry = ConvertFrom-WatchdogMessage -Message $Message
    $fields = [ordered]@{}
    foreach ($field in $entry.Fields.GetEnumerator()) {
        $fields[$field.Key] = $field.Value
    }

    $mcpPid = if ($script:lastKnownMcpPid) { $script:lastKnownMcpPid } else { '-' }
    if ($fields.Contains('pid')) {
        $fieldPid = [string]$fields['pid']
        if (-not [string]::IsNullOrWhiteSpace($fieldPid)) {
            $mcpPid = $fieldPid
            $script:lastKnownMcpPid = $fieldPid
        }
        [void]$fields.Remove('pid')
    }

    if ($fields.Contains('result')) {
        [void]$fields.Remove('result')
    }

    if ($fields.Contains('uptimeSeconds')) {
        $fields['uptime'] = Format-WatchdogDuration -Seconds ([string]$fields['uptimeSeconds'])
        [void]$fields.Remove('uptimeSeconds')
    }

    if ($entry.Event -eq 'McpUp') {
        if ($fields.Contains('version')) { [void]$fields.Remove('version') }
        if ($fields.Contains('toolsRegistered')) { [void]$fields.Remove('toolsRegistered') }
    }

    $trailingDetail = $null
    if ($entry.Event -eq 'BuildFailed' -and $fields.Contains('log')) {
        $trailingDetail = [string]$fields['log']
        [void]$fields.Remove('log')
    }

    $jsonFields = [ordered]@{}
    foreach ($field in $fields.GetEnumerator()) {
        $jsonFields[$field.Key] = $field.Value
    }
    if (-not [string]::IsNullOrWhiteSpace($trailingDetail)) {
        $jsonFields['log'] = $trailingDetail
    }

    $parts = @()
    foreach ($field in $fields.GetEnumerator()) {
        $parts += ("{0}={1}" -f $field.Key, (Format-WatchdogLogValue $field.Value))
    }

    $header = ("[pid:{0}][{1}][{2}]" -f $mcpPid, $stamp, $entry.Event)
    $payload = if ($parts.Count -gt 0) { $parts -join ' ' } else { '' }
    if (-not [string]::IsNullOrWhiteSpace($trailingDetail)) {
        $payload = if ([string]::IsNullOrWhiteSpace($payload)) {
            $trailingDetail
        }
        else {
            ("{0}, {1}" -f $payload, $trailingDetail)
        }
    }

    Write-WatchdogJsonLog -Timestamp $now -McpPid $mcpPid -Event $entry.Event -Fields $jsonFields -Message $payload

    $splitPayload = (-not [string]::IsNullOrWhiteSpace($payload)) -and
        (($payload.Length -gt $script:watchdogInlinePayloadLimit) -or ($payload -match "[`r`n]"))

    if ($splitPayload) {
        [Console]::Out.WriteLine($header)
        [Console]::Out.WriteLine($payload)
        return
    }

    $line = $header
    if (-not [string]::IsNullOrWhiteSpace($payload)) {
        $line = ("{0} {1}" -f $line, $payload)
    }
    [Console]::Out.WriteLine($line)
}

function Get-MonolithHealth {
    $script:lastHealthErrorClass = $null
    $script:lastHealthErrorCode = $null
    $script:lastHealthStatusCode = $null
    try {
        $resp = Invoke-WebRequest -Uri $healthUrl -Method Get -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        $script:lastHealthStatusCode = [int]$resp.StatusCode
        if ($resp.StatusCode -eq 200) {
            try {
                $health = $resp.Content | ConvertFrom-Json -ErrorAction Stop
            }
            catch {
                $script:lastHealthErrorClass = 'invalid_json'
                $script:lastHealthErrorCode = 'http_200_body_not_json'
                return $null
            }

            $contract = Test-MonolithHealthContract -Health $health -ExpectedPort (Get-McpHealthPort)
            if (-not $contract.Valid) {
                $script:lastHealthErrorClass = 'health_contract'
                $script:lastHealthErrorCode = $contract.ErrorCode
                return $null
            }

            $identity = Test-MonolithHealthProcessIdentity -Health $health -Root $script:hostRoot
            if (-not $identity.Valid) {
                $script:lastHealthErrorClass = 'health_identity'
                $script:lastHealthErrorCode = $identity.ErrorCode
                return $null
            }
            $listenerIdentity = Test-MonolithHealthListenerIdentity -Health $health -Port (Get-McpHealthPort)
            if (-not $listenerIdentity.Valid) {
                $script:lastHealthErrorClass = 'health_identity'
                $script:lastHealthErrorCode = $listenerIdentity.ErrorCode
                return $null
            }
            return $health
        }
        $script:lastHealthErrorClass = 'http_status'
        $script:lastHealthErrorCode = ("status_{0}" -f $resp.StatusCode)
    }
    catch {
        $script:lastHealthErrorClass = 'request_failed'
        $script:lastHealthErrorCode = $_.Exception.Message
    }
    return $null
}

function Format-WatchdogValue {
    param($Value)

    if ($null -eq $Value) {
        return ''
    }

    try {
        if ($Value -is [string]) {
            $text = $Value
        }
        else {
            $text = $Value | ConvertTo-Json -Depth 8 -Compress
        }
    }
    catch {
        $text = [string]$Value
    }

    $text = $text -replace '\s+', ' '
    if ($text.Length -gt 500) {
        return ($text.Substring(0, 500) + '...')
    }
    return $text
}

function Get-DailyReindexSchedule {
    if (-not ($DailyReindexTime -match '^(?<hour>[01]?[0-9]|2[0-3]):(?<minute>[0-5][0-9])$')) {
        Write-Watchdog ("RESULT=BLOCKED reason=invalid_daily_reindex_time value={0} expected=HH:mm" -f $DailyReindexTime)
        return $null
    }

    return New-TimeSpan -Hours ([int]$Matches['hour']) -Minutes ([int]$Matches['minute'])
}

function Get-DailyReindexTimeZoneInfo {
    try {
        return [System.TimeZoneInfo]::FindSystemTimeZoneById($DailyReindexTimeZone)
    }
    catch {
        Write-Watchdog ("RESULT=BLOCKED reason=invalid_daily_reindex_timezone value={0} error={1}" -f `
                $DailyReindexTimeZone, (Format-WatchdogValue $_.Exception.Message))
        return $null
    }
}

function Test-IndexTarget {
    param(
        [string[]]$Targets,
        [string]$Target
    )
    return (@($Targets) -contains $Target)
}

function Test-DailyReindexTarget {
    param([string]$Target)
    return (Test-IndexTarget -Targets $DailyReindexTargets -Target $Target)
}

function Get-MonolithActionPayload {
    param($Response)

    if ($Response -and $Response.result -and $Response.result.structuredContent) {
        return $Response.result.structuredContent
    }

    if ($Response -and $Response.structuredContent) {
        return $Response.structuredContent
    }

    $texts = @()
    if ($Response -and $Response.result -and $Response.result.content) {
        foreach ($content in @($Response.result.content)) {
            if ($content.text) {
                $texts += [string]$content.text
            }
        }
    }

    foreach ($text in $texts) {
        try {
            return $text | ConvertFrom-Json
        }
        catch { }
    }

    return $null
}

function Invoke-MonolithAction {
    param(
        [string]$Namespace,
        [string]$Action,
        [hashtable]$Params = @{}
    )

    $request = @{
        jsonrpc = '2.0'
        id      = [guid]::NewGuid().ToString('N')
        method  = 'tools/call'
        params  = @{
            name      = 'monolith_query'
            arguments = @{
                namespace = $Namespace
                action    = $Action
                params    = $Params
            }
        }
    }

    try {
        $body = $request | ConvertTo-Json -Depth 10 -Compress
        $resp = Invoke-RestMethod -Uri $McpUrl -Method Post -ContentType 'application/json' -Body $body -TimeoutSec $DailyReindexActionTimeoutSec
    }
    catch {
        $message = Format-WatchdogValue $_.Exception.Message
        Write-Watchdog ("daily_reindex_action_failed namespace={0} action={1} error={2}" -f $Namespace, $Action, $message)
        return [PSCustomObject]@{ Succeeded = $false; Payload = $null; Summary = $message }
    }

    $payload = Get-MonolithActionPayload -Response $resp
    $summary = Format-WatchdogValue $(if ($payload) { $payload } else { $resp.result })

    if ($resp.error) {
        Write-Watchdog ("daily_reindex_action_failed namespace={0} action={1} error={2}" -f `
                $Namespace, $Action, (Format-WatchdogValue $resp.error))
        return [PSCustomObject]@{ Succeeded = $false; Payload = $payload; Summary = $summary }
    }

    if ($resp.result -and $resp.result.isError) {
        Write-Watchdog ("daily_reindex_action_failed namespace={0} action={1} result={2}" -f $Namespace, $Action, $summary)
        return [PSCustomObject]@{ Succeeded = $false; Payload = $payload; Summary = $summary }
    }

    Write-Watchdog ("daily_reindex_action_succeeded namespace={0} action={1} result={2}" -f $Namespace, $Action, $summary)
    return [PSCustomObject]@{ Succeeded = $true; Payload = $payload; Summary = $summary }
}

function Get-BridgeIndexScope {
    param([string[]]$Targets = $DailyReindexTargets)

    $hasAssets = Test-IndexTarget -Targets $Targets -Target 'assets'
    $hasSource = Test-IndexTarget -Targets $Targets -Target 'source'
    if ($hasAssets -and $hasSource) { return 'all' }
    if ($hasAssets) { return 'assets' }
    if ($hasSource) { return 'source' }
    return $null
}

function Wait-BridgeIndexIdle {
    param([string]$Scope)

    $needProject = ($Scope -eq 'all' -or $Scope -eq 'assets')
    $needSource = ($Scope -eq 'all' -or $Scope -eq 'source')
    $deadline = (Get-Date).AddSeconds($DailyReindexWaitTimeoutSec)

    while ($true) {
        $status = Invoke-MonolithAction -Namespace 'bridge' -Action 'get_index_status' -Params @{ include_stats = $false }
        if (-not $status.Succeeded -or -not $status.Payload) {
            Write-Watchdog ("daily_reindex_wait_failed scope={0} reason=status_unavailable" -f $Scope)
            return $false
        }

        $project = $status.Payload.project_index
        $source = $status.Payload.source_index
        $projectAvailable = ($project -and [bool]$project.available)
        $sourceAvailable = ($source -and ([bool]$source.available -or [bool]$source.database_open -or [bool]$source.indexing))
        $projectIndexing = ($projectAvailable -and [bool]$project.indexing)
        $sourceIndexing = ($sourceAvailable -and [bool]$source.indexing)

        if (($needProject -and -not $projectAvailable) -or ($needSource -and -not $sourceAvailable)) {
            Write-Watchdog ("daily_reindex_wait_failed scope={0} project_available={1} source_available={2}" -f `
                    $Scope, $projectAvailable, $sourceAvailable)
            return $false
        }

        if ((-not $needProject -or -not $projectIndexing) -and (-not $needSource -or -not $sourceIndexing)) {
            Write-Watchdog ("daily_reindex_indexes_idle scope={0} project_indexing={1} source_indexing={2}" -f `
                    $Scope, $projectIndexing, $sourceIndexing)
            return $true
        }

        if ((Get-Date) -ge $deadline) {
            Write-Watchdog ("daily_reindex_wait_timeout scope={0} project_indexing={1} source_indexing={2} timeout_sec={3}" -f `
                    $Scope, $projectIndexing, $sourceIndexing, $DailyReindexWaitTimeoutSec)
            return $false
        }

        Write-Watchdog ("daily_reindex_waiting scope={0} project_indexing={1} source_indexing={2}" -f `
                $Scope, $projectIndexing, $sourceIndexing)
        Start-Sleep -Seconds $DailyReindexWaitPollSec
    }
}

function Invoke-GraphIndex {
    param(
        [string]$Root,
        [bool]$Full,
        [int]$CooldownSeconds = $DailyGraphCooldownSeconds,
        [string]$Prefix = 'daily_reindex'
    )

    $pluginRoot = Split-Path -Parent $PSScriptRoot
    $queryExe = Join-Path $pluginRoot 'Binaries\monolith_query.exe'
    if (-not (Test-Path -LiteralPath $queryExe -PathType Leaf)) {
        Write-Watchdog ("{0}_graph_failed reason=query_exe_missing path={1}" -f $Prefix, $queryExe)
        return $false
    }

    $args = @(
        'source',
        'build_crg_graph',
        '--execute',
        ("--cooldown_seconds={0}" -f $CooldownSeconds)
    )
    if ($Full) {
        $args += '--force'
    }

    Write-Watchdog ("{0}_graph_start mode={1} root={2} query={3}" -f `
            $Prefix, $(if ($Full) { 'full' } else { 'incremental' }), $Root, $queryExe)
    Push-Location -LiteralPath $pluginRoot
    try {
        $output = & $queryExe @args 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    $detail = Format-WatchdogValue (($output | Out-String).Trim())
    if ($exitCode -ne 0) {
        Write-Watchdog ("{0}_graph_failed exit_code={1} detail={2}" -f $Prefix, $exitCode, $detail)
        return $false
    }

    Write-Watchdog ("{0}_graph_done exit_code={1} detail={2}" -f $Prefix, $exitCode, $detail)
    return $true
}

function Invoke-DailyReindex {
    param(
        [string]$Root,
        [string]$Reason,
        [string]$Mode = $DailyReindexMode,
        [string[]]$Targets = $DailyReindexTargets,
        [int]$GraphCooldownSeconds = $DailyGraphCooldownSeconds,
        [string]$Prefix = 'daily_reindex',
        [string]$ResultToken = 'DAILY_REINDEX'
    )

    $targetsText = (@($Targets) -join ',')
    Write-Watchdog ("{0}_start reason={1} time={2} timezone={3} mode={4} targets={5}" -f `
            $Prefix, $Reason, $DailyReindexTime, $DailyReindexTimeZone, $Mode, $targetsText)

    $ok = $true
    $bridgeOk = $true
    $scope = Get-BridgeIndexScope -Targets $Targets
    if ($scope) {
        $start = Invoke-MonolithAction -Namespace 'bridge' -Action 'start_indexing' -Params @{
            scope = $scope
            full  = ($Mode -eq 'full')
        }
        if (-not $start.Succeeded) {
            $ok = $false
            $bridgeOk = $false
        }
        elseif (-not (Wait-BridgeIndexIdle -Scope $scope)) {
            $ok = $false
            $bridgeOk = $false
        }
    }

    if (Test-IndexTarget -Targets $Targets -Target 'graph') {
        if ($scope -and -not $bridgeOk) {
            Write-Watchdog ("{0}_graph_skipped reason=asset_or_source_index_not_verified_idle" -f $Prefix)
            $ok = $false
        }
        elseif (-not (Invoke-GraphIndex -Root $Root -Full ($Mode -eq 'full') -CooldownSeconds $GraphCooldownSeconds -Prefix $Prefix)) {
            $ok = $false
        }
    }

    if ($ok) {
        Write-Watchdog ("RESULT={0}_OK reason={1} mode={2} targets={3}" -f $ResultToken, $Reason, $Mode, $targetsText)
    }
    else {
        Write-Watchdog ("RESULT={0}_FAILED reason={1} mode={2} targets={3}" -f $ResultToken, $Reason, $Mode, $targetsText)
    }
    return $ok
}

function Invoke-DailyReindexIfDue {
    if ($DisableDailyReindex -or $ProbeOnly) {
        return [PSCustomObject]@{ Ran = $false; Succeeded = $true; ExitCode = 0 }
    }

    $reason = $null
    if ($RunDailyReindexNow -and -not $script:runDailyReindexNowConsumed) {
        $script:runDailyReindexNowConsumed = $true
        $reason = 'manual'
    }
    else {
        $now = [System.TimeZoneInfo]::ConvertTime([DateTimeOffset]::Now, $script:dailyReindexTimeZoneInfo)
        if ($now.TimeOfDay -lt $script:dailyReindexSchedule) {
            return [PSCustomObject]@{ Ran = $false; Succeeded = $true; ExitCode = 0 }
        }

        $dateKey = $now.ToString('yyyy-MM-dd')
        if ($script:lastDailyReindexAttemptDate -eq $dateKey) {
            return [PSCustomObject]@{ Ran = $false; Succeeded = $true; ExitCode = 0 }
        }

        $script:lastDailyReindexAttemptDate = $dateKey
        $reason = ("scheduled_date={0}" -f $dateKey)
    }

    $root = Resolve-HostRoot
    if (-not $root) {
        Write-Watchdog 'RESULT=BLOCKED reason=host_root_not_found detail=daily reindex is due but no *.uproject found upward from the script and no -ProjectRoot given'
        return [PSCustomObject]@{ Ran = $true; Succeeded = $false; ExitCode = 3 }
    }

    $success = Invoke-DailyReindex -Root $root -Reason $reason
    return [PSCustomObject]@{ Ran = $true; Succeeded = $success; ExitCode = $(if ($success) { 0 } else { 8 }) }
}

function Resolve-HostRoot {
    if ($ProjectRoot) {
        if (-not (Test-Path -LiteralPath $ProjectRoot)) { return $null }
        return (Resolve-Path -LiteralPath $ProjectRoot).Path
    }

    $dir = Split-Path -Parent $PSScriptRoot
    for ($depth = 0; $depth -lt 8 -and $dir; $depth++) {
        $uproject = Get-ChildItem -LiteralPath $dir -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($uproject) { return $dir }
        $dir = Split-Path -Parent $dir
    }
    return $null
}

function Get-McpHealthPort {
    try {
        $uri = [System.Uri]$healthUrl
        if ($uri.Port -gt 0) { return [int]$uri.Port }
    }
    catch { }
    return 9316
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

    if ([string]::IsNullOrWhiteSpace($CommandLine) -or [string]::IsNullOrWhiteSpace($ProjectFile)) {
        return $false
    }
    if ($CommandLine -match '(?i)(^|\s)-(game|server)(\s|$)' -or
        $CommandLine -match '(?i)(^|\s)-run(?:=|\s)' -or
        $CommandLine -match '(?i)bMcpServerEnabled\s*[:=]\s*False') {
        return $false
    }

    $normalizedCommandLine = $CommandLine.Replace('/', '\')
    $normalizedProjectFile = ([System.IO.Path]::GetFullPath($ProjectFile)).Replace('/', '\')
    return $normalizedCommandLine.IndexOf($normalizedProjectFile, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
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
    if (-not (Test-MonolithEditorCommandLineForProject -CommandLine ([string]$process.CommandLine) -ProjectFile $projectFile.FullName)) {
        return [PSCustomObject]@{ Valid = $false; ErrorCode = 'health_pid_wrong_project_or_mode' }
    }
    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
}

function Get-MonolithListenerSummary {
    param([int]$Port)

    try {
        $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction Stop)
        return [PSCustomObject]@{
            Count = $listeners.Count
            Pids = @($listeners | ForEach-Object { [int]$_.OwningProcess } | Sort-Object -Unique)
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
            $rows = @($netstatOutput | Where-Object {
                    $_ -match (":{0}\s" -f [regex]::Escape([string]$Port)) -and $_ -match '\sLISTENING\s+(\d+)\s*$'
                })
            return [PSCustomObject]@{
                Count = $rows.Count
                Pids = @($rows | ForEach-Object {
                        if ($_ -match '\sLISTENING\s+(\d+)\s*$') { [int]$Matches[1] }
                    } | Sort-Object -Unique)
                Error = $null
            }
        }
        catch {
            $netstatError = ("{0}; netstat fallback: {1}" -f $netstatError, $_.Exception.Message)
            return [PSCustomObject]@{ Count = -1; Pids = @(); Error = $netstatError }
        }
    }
}

function Test-MonolithHealthListenerIdentity {
    param($Health, [int]$Port, $ListenerSummary)

    $summary = $ListenerSummary
    if (-not $PSBoundParameters.ContainsKey('ListenerSummary')) {
        $summary = Get-MonolithListenerSummary -Port $Port
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

    $summary = Get-MonolithListenerSummary -Port $Port
    $validation = Test-MonolithRecoveryPortClear -ListenerSummary $summary
    return [PSCustomObject]@{
        Valid = $validation.Valid
        ErrorCode = $validation.ErrorCode
        Count = $summary.Count
        Pids = @($summary.Pids)
        Error = $summary.Error
    }
}

function Get-EditorServerCandidates {
    param([string]$Root = $script:hostRoot)

    $procs = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { return @() }
    $projectFile = Get-ProjectFile -Root $Root
    if (-not $projectFile) { return @() }

    $cims = @{}
    Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { $cims[[int]$_.ProcessId] = $_.CommandLine }

    return @($procs | Where-Object {
            $cmd = $cims[$_.Id]
            (-not $cmd) -or (Test-MonolithEditorCommandLineForProject -CommandLine $cmd -ProjectFile $projectFile.FullName)
        })
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
            $cmd -and (
                ($cmd -match '(?i)(^|\s)-NullRHI(\s|$)') -or
                ($cmd -match '(?i)Saved\\HeadlessMcp') -or
                ($cmd -match '(?i)HeadlessEditor-')
            )
        })
}

function Get-ProjectFile {
    param([string]$Root)
    return Get-ChildItem -LiteralPath $Root -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Resolve-EngineRoot {
    param(
        [string]$Root,
        [System.IO.FileInfo]$Uproject
    )

    $resolver = Join-Path $Root 'Build\BatchFiles\Script\ResolveUnrealEngine.ps1'
    if (-not (Test-Path -LiteralPath $resolver -PathType Leaf)) {
        Write-Watchdog ("RESULT=BLOCKED reason=engine_resolver_missing path={0}" -f $resolver)
        return $null
    }

    $engineRoot = & powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject.FullName -Output Root 2>&1
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace([string]$engineRoot)) {
        Write-Watchdog ("RESULT=BLOCKED reason=engine_root_unresolved detail={0}" -f (($engineRoot | Out-String).Trim()))
        return $null
    }

    return ([string]$engineRoot).Trim()
}

function Get-EditorTargetName {
    param(
        [string]$Root,
        [string]$ProjectName
    )
    $sourceRoot = Join-Path $Root 'Source'
    $targetFile = Get-ChildItem -LiteralPath $sourceRoot -Filter '*Editor.Target.cs' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($targetFile) {
        return [System.IO.Path]::GetFileNameWithoutExtension(
            [System.IO.Path]::GetFileNameWithoutExtension($targetFile.Name)
        )
    }
    return ("{0}Editor" -f $ProjectName)
}

function Get-BuildLockCandidateFiles {
    param([string]$Root)

    # Bounded glob set over the primary UBT link outputs. Deliberately not a
    # full recursive scan: project modules, one-level plugins, and two-level
    # plugin groups (e.g. Plugins\GameFeatures\<Feature>) cover every LNK1104
    # target observed in watchdog logs (2026-07-06..10 incident).
    $patterns = @(
        (Join-Path $Root 'Binaries\Win64\UnrealEditor-*.dll'),
        (Join-Path $Root 'Plugins\*\Binaries\Win64\UnrealEditor-*.dll'),
        (Join-Path $Root 'Plugins\*\*\Binaries\Win64\UnrealEditor-*.dll')
    )
    $files = @()
    foreach ($pattern in $patterns) {
        $files += @(Get-Item -Path $pattern -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer })
    }
    return ,@($files | Sort-Object -Property FullName -Unique)
}

function Get-BuildBlockingLocks {
    param([string]$Root)

    # The linker needs write access to replace each link output; a DLL mapped
    # into any running process denies that with a sharing violation (LNK1104).
    # Probe with the exact access the linker needs and maximal sharing so the
    # only failure cause is another holder. Read-only files (Perforce state,
    # ACL) are a different, persistent failure class: report them separately
    # instead of treating them as process locks, because 116 prior watchdog
    # builds succeeded with read-only files present.
    $locked = @()
    $readOnly = @()
    $probeErrors = @()
    $shareAll = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    foreach ($file in (Get-BuildLockCandidateFiles -Root $Root)) {
        if ($file.IsReadOnly) {
            $readOnly += $file.FullName
            continue
        }
        try {
            $stream = [System.IO.File]::Open(
                $file.FullName,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Write,
                $shareAll)
            $stream.Dispose()
        }
        catch [System.IO.IOException] {
            $locked += $file.FullName
        }
        catch [System.UnauthorizedAccessException] {
            $readOnly += $file.FullName
        }
        catch {
            # Never erase an unknown probe failure. A partial scan cannot prove
            # that UBT may safely replace every output, so callers fail closed
            # and surface a bounded diagnostic instead of starting the build.
            $probeErrors += [PSCustomObject]@{
                Path = $file.FullName
                ExceptionType = $_.Exception.GetType().FullName
                Message = $_.Exception.Message
            }
        }
    }
    return [PSCustomObject]@{
        Locked = @($locked)
        ReadOnly = @($readOnly)
        ProbeErrors = @($probeErrors)
    }
}

function Format-BuildProbeErrors {
    param($ProbeErrors)

    $items = @($ProbeErrors | Select-Object -First 8 | ForEach-Object {
            ("{0}|{1}|{2}" -f $_.Path, $_.ExceptionType, $_.Message)
        })
    return (Format-WatchdogValue ($items -join ';'))
}

function Get-BuildLockHolderPids {
    param([string]$Root)

    # Best-effort holder attribution for the BlockedDllLocked event. Unlike
    # Get-EditorServerCandidates this must NOT exclude -game/-server clients:
    # they hold project DLLs without ever being editor-server candidates,
    # which is precisely how the 2026-07-09 LNK1104 loop started.
    $needle = $Root.TrimEnd('\', '/')
    $pids = @()
    Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue |
        ForEach-Object {
            if ($_.CommandLine -and $_.CommandLine.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $pids += [int]$_.ProcessId
            }
        }
    return ,@($pids)
}

function Get-BuildFailureBackoffSeconds {
    $exponent = [Math]::Min([Math]::Max($script:consecutiveBuildFailures - 1, 0), 6)
    return [int][Math]::Min($PollIntervalSec * [Math]::Pow(2, $exponent), $BuildFailureBackoffMaxSec)
}

function Invoke-EditorBuild {
    param([string]$Root)

    $uproject = Get-ProjectFile -Root $Root
    if (-not $uproject) {
        Write-Watchdog ("RESULT=BLOCKED reason=uproject_not_found root={0}" -f $Root)
        return $false
    }

    $engineRoot = Resolve-EngineRoot -Root $Root -Uproject $uproject
    if (-not $engineRoot) { return $false }

    $ubt = Join-Path $engineRoot 'Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'
    if (-not (Test-Path -LiteralPath $ubt -PathType Leaf)) {
        Write-Watchdog ("RESULT=BLOCKED reason=ubt_missing path={0}" -f $ubt)
        return $false
    }

    $projectName = [System.IO.Path]::GetFileNameWithoutExtension($uproject.Name)
    $editorTarget = Get-EditorTargetName -Root $Root -ProjectName $projectName
    $logDir = Join-Path $Root 'Saved\Monolith\Watchdog'
    if (-not (Test-Path -LiteralPath $logDir)) {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }
    $buildLog = Join-Path $logDir ("UBT-{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))

    Write-Watchdog ("build_start target={0} project={1} log={2}" -f $editorTarget, $uproject.FullName, $buildLog)
    $buildArgs = @(
        $editorTarget,
        'Win64',
        'Development',
        ("-Project={0}" -f $uproject.FullName),
        '-WaitMutex',
        '-NoHotReloadFromIDE'
    )
    $buildOutput = & $ubt @buildArgs 2>&1
    $exitCode = $LASTEXITCODE
    $buildOutput | Set-Content -LiteralPath $buildLog -Encoding UTF8

    if ($exitCode -ne 0) {
        Write-Watchdog ("RESULT=BUILD_FAILED exit_code={0} log={1}" -f $exitCode, $buildLog)
        return $false
    }

    Write-Watchdog ("build_succeeded target={0} log={1}" -f $editorTarget, $buildLog)
    return $true
}

function Invoke-SourceCommandletReindex {
    param(
        [string]$Root,
        [string]$Mode
    )

    $uproject = Get-ProjectFile -Root $Root
    if (-not $uproject) {
        Write-Watchdog ("RESULT=BLOCKED reason=uproject_not_found root={0}" -f $Root)
        return $false
    }

    $engineRoot = Resolve-EngineRoot -Root $Root -Uproject $uproject
    if (-not $engineRoot) { return $false }

    $cmd = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    if (-not (Test-Path -LiteralPath $cmd -PathType Leaf)) {
        Write-Watchdog ("RESULT=BLOCKED reason=unrealeditor_cmd_missing path={0}" -f $cmd)
        return $false
    }

    $logDir = Join-Path $Root 'Saved\Monolith\Watchdog'
    if (-not (Test-Path -LiteralPath $logDir)) {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }
    $log = Join-Path $logDir ("MonolithReindex-{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
    $modeArg = if ($Mode -eq 'full') { '-mode=full' } else { '-mode=project' }

    $args = @(
        $uproject.FullName,
        '-run=MonolithReindex',
        $modeArg,
        '-unattended',
        '-nopause',
        '-nosplash',
        '-nullrhi',
        ("-ABSLOG={0}" -f $log)
    )

    Write-Watchdog ("pre_restart_reindex_source_start mode={0} project={1} log={2}" -f $Mode, $uproject.FullName, $log)
    $output = & $cmd @args 2>&1
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath $log -Encoding UTF8

    if ($exitCode -ne 0) {
        Write-Watchdog ("pre_restart_reindex_source_failed exit_code={0} log={1} detail={2}" -f `
                $exitCode, $log, (Format-WatchdogValue (($output | Out-String).Trim())))
        return $false
    }

    Write-Watchdog ("pre_restart_reindex_source_done mode={0} log={1}" -f $Mode, $log)
    return $true
}

function Invoke-PreRestartReindex {
    param([string]$Root)

    if ($SkipRestartReindex) {
        Write-Watchdog 'pre_restart_reindex_skipped reason=SkipRestartReindex'
        return $true
    }

    $ok = $true
    $targetsText = (@($RestartReindexTargets) -join ',')
    Write-Watchdog ("pre_restart_reindex_start mode={0} targets={1}" -f $RestartReindexMode, $targetsText)

    if (Test-IndexTarget -Targets $RestartReindexTargets -Target 'assets') {
        Write-Watchdog 'pre_restart_reindex_assets_deferred reason=ProjectIndex_requires_editor_subsystem action=restart_reindex_after_mcp_health'
    }

    if (Test-IndexTarget -Targets $RestartReindexTargets -Target 'source') {
        if (-not (Invoke-SourceCommandletReindex -Root $Root -Mode $RestartReindexMode)) {
            $ok = $false
        }
    }

    if (Test-IndexTarget -Targets $RestartReindexTargets -Target 'graph') {
        if (-not (Invoke-GraphIndex -Root $Root -Full ($RestartReindexMode -eq 'full') -CooldownSeconds $DailyGraphCooldownSeconds -Prefix 'pre_restart_reindex')) {
            $ok = $false
        }
    }

    if ($ok) {
        Write-Watchdog ("pre_restart_reindex_done mode={0} targets={1}" -f $RestartReindexMode, $targetsText)
        if (-not (Test-IndexTarget -Targets $RestartReindexTargets -Target 'assets')) {
            Write-Watchdog ("RESULT=RESTART_REINDEX_OK phase=pre_restart mode={0} targets={1}" -f $RestartReindexMode, $targetsText)
        }
    }
    else {
        Write-Watchdog ("RESULT=RESTART_REINDEX_FAILED phase=pre_restart mode={0} targets={1}" -f $RestartReindexMode, $targetsText)
    }
    return $ok
}

function Invoke-PostRecoverReindex {
    param(
        [string]$Root,
        [switch]$RetryPreRestartTargets
    )

    if ($SkipRestartReindex) {
        return $true
    }

    $targets = @()
    $phase = 'post_recover_assets'
    if ($RetryPreRestartTargets) {
        $targets = @($RestartReindexTargets)
        $phase = 'post_recover_retry'
    }
    elseif (Test-IndexTarget -Targets $RestartReindexTargets -Target 'assets') {
        $targets = @('assets')
    }
    if ($targets.Count -eq 0) { return $true }

    $ok = Invoke-DailyReindex -Root $Root -Reason $phase `
        -Mode $RestartReindexMode -Targets $targets -GraphCooldownSeconds $DailyGraphCooldownSeconds -Prefix 'restart_reindex' -ResultToken 'RESTART_REINDEX'
    $targetsText = $targets -join ','
    if ($ok) {
        Write-Watchdog ("RESULT=RESTART_REINDEX_OK phase={0} mode={1} targets={2}" -f $phase, $RestartReindexMode, $targetsText)
    }
    else {
        Write-Watchdog ("RESULT=RESTART_REINDEX_FAILED phase={0} mode={1} targets={2} endpoint_recovered=true" -f `
                $phase, $RestartReindexMode, $targetsText)
    }
    return $ok
}

function Start-WatchdogChildProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string]$ArgumentList,
        [Parameter(Mandatory = $true)]
        [string]$StandardOutputPath,
        [Parameter(Mandatory = $true)]
        [string]$StandardErrorPath
    )

    $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -NoNewWindow -PassThru `
        -RedirectStandardOutput $StandardOutputPath -RedirectStandardError $StandardErrorPath

    # Windows PowerShell 5.1 lazily initializes the native process handle for a
    # Start-Process -PassThru object. If the child exits before the handle is read,
    # ExitCode remains $null even after WaitForExit()/Refresh(). Pin the handle while
    # the process is live so a successful recover is reported as 0 instead of being
    # mistaken for a failure and killing the editor that just recovered.
    [void]$process.Handle
    return $process
}

function Invoke-Recover {
    param(
        [string]$Root,
        [switch]$ForceLaunch
    )

    $recover = Join-Path $PSScriptRoot 'recover_mcp.ps1'
    if (-not (Test-Path -LiteralPath $recover -PathType Leaf)) {
        Write-Watchdog ("RESULT=BLOCKED reason=recover_script_missing path={0}" -f $recover)
        return [PSCustomObject]@{ ExitCode = 3; Result = 'RESULT=BLOCKED' }
    }

    $args = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $recover,
        '-McpUrl',
        $McpUrl,
        '-TimeoutSec',
        $RecoverTimeoutSec,
        '-PollIntervalSec',
        $RecoverPollIntervalSec,
        '-ProjectRoot',
        $Root
    )
    if ($ForceLaunch) {
        $args += '-ForceLaunch'
    }

    # Bounded child invocation: the 2026-07-03 23:33 incident hung inside a
    # synchronous recover call until the next reboot killed the watchdog with
    # no terminal log line. Run recover in a monitored child process and kill
    # it past RecoverTimeoutSec + RecoverInvokeGraceSec.
    $invokeTimeoutSec = $RecoverTimeoutSec + $RecoverInvokeGraceSec
    Write-Watchdog ("recover_start force_launch={0} invoke_timeout_sec={1}" -f [bool]$ForceLaunch, $invokeTimeoutSec)

    $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $psExe -PathType Leaf)) {
        $psExe = 'powershell.exe'
    }
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    $argLine = ($args | ForEach-Object {
            $text = [string]$_
            if ($text -match '\s') { '"{0}"' -f $text } else { $text }
        }) -join ' '

    $timedOut = $false
    try {
        $proc = Start-WatchdogChildProcess -FilePath $psExe -ArgumentList $argLine `
            -StandardOutputPath $stdoutFile -StandardErrorPath $stderrFile
        if (-not $proc.WaitForExit($invokeTimeoutSec * 1000)) {
            $timedOut = $true
            try { $proc.Kill() } catch {}
            [void]$proc.WaitForExit(15000)
        }
        $exitCode = if ($timedOut) { 124 } else { $proc.ExitCode }
    }
    catch {
        Write-Watchdog ("RESULT=BLOCKED reason=recover_spawn_failed error={0}" -f (Format-WatchdogValue $_.Exception.Message))
        Remove-Item -LiteralPath $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
        return [PSCustomObject]@{ ExitCode = 3; Result = 'RESULT=BLOCKED' }
    }

    $output = @()
    foreach ($file in @($stdoutFile, $stderrFile)) {
        if (Test-Path -LiteralPath $file) {
            $output += @(Get-Content -LiteralPath $file -ErrorAction SilentlyContinue)
        }
    }
    Remove-Item -LiteralPath $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue

    $output | ForEach-Object { Write-Watchdog ("recover_output detail={0}" -f (Format-WatchdogValue $_)) }
    if ($timedOut) {
        Write-Watchdog ("RESULT=RECOVER_TIMEOUT invoke_timeout_sec={0}" -f $invokeTimeoutSec)
        return [PSCustomObject]@{ ExitCode = 124; Result = 'RESULT=RECOVER_TIMEOUT' }
    }

    $resultLine = ($output | Select-String -Pattern 'RESULT=' | Select-Object -Last 1).Line
    if (-not $resultLine) {
        $resultLine = 'RESULT=UNKNOWN'
    }
    Write-Watchdog ("recover_done exit_code={0} {1}" -f $exitCode, $resultLine)
    return [PSCustomObject]@{ ExitCode = $exitCode; Result = $resultLine }
}

function Stop-HeadlessEditors {
    param($Processes)

    $ids = @($Processes | ForEach-Object { $_.Id })
    if ($ids.Count -eq 0) {
        return
    }

    Write-Watchdog ("stopping_unhealthy_headless_editors pids={0}" -f ($ids -join ','))
    foreach ($id in $ids) {
        try {
            Stop-Process -Id $id -Force -ErrorAction Stop
        }
        catch {
            Write-Watchdog ("stop_headless_editor_failed pid={0} error={1}" -f $id, (Format-WatchdogValue $_.Exception.Message))
        }
    }
    Start-Sleep -Seconds 2
}

function Invoke-RestartSequence {
    param(
        [string]$Root,
        [string]$Reason
    )

    # This is the common mutation boundary for build, offline maintenance, and
    # relaunch. Any listener without a fully accepted health response owns the
    # port until an operator resolves it; never race that owner with UBT or a
    # second editor process.
    $port = Get-McpHealthPort
    $portGate = Get-MonolithRecoveryPortGate -Port $port
    if (-not $portGate.Valid) {
        Write-Watchdog ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint phase=before_restart_mutation listener_port={0} listener_count={1} listener_pids={2} listener_gate={3}" -f `
                $port, $portGate.Count, (($portGate.Pids) -join ','), $portGate.ErrorCode)
        return [PSCustomObject]@{ ExitCode = 3; Result = 'RESULT=BLOCKED' }
    }

    $script:restartAttempts++
    if ($MaxRestartAttempts -gt 0 -and $script:restartAttempts -gt $MaxRestartAttempts) {
        # Past the limit: escalate the probe sleep instead of spinning a
        # restart attempt every PollIntervalSec (2026-07-03 23:30 incident:
        # RESULT=RESTART_LIMIT logged every ~18s for minutes).
        $overBy = [Math]::Min($script:restartAttempts - $MaxRestartAttempts, 6)
        $backoffSec = [int][Math]::Min($PollIntervalSec * [Math]::Pow(2, $overBy), $RestartLimitBackoffMaxSec)
        Write-Watchdog ("RESULT=RESTART_LIMIT attempts={0} backoff_seconds={1}" -f ($script:restartAttempts - 1), $backoffSec)
        return [PSCustomObject]@{ ExitCode = 7; Result = 'RESULT=RESTART_LIMIT'; BackoffSeconds = $backoffSec }
    }

    Write-Watchdog ("restart_sequence_start reason={0} restart_attempt={1}" -f $Reason, $script:restartAttempts)
    if (-not $NoBuildBeforeRestart) {
        # LNK1104 preflight: never hand UBT a link output some process still
        # holds (2026-07-06..10 incident: 444 BuildFailed in a retry loop
        # while -game clients kept CommonGame/SpeedCoreRuntime DLLs mapped).
        $lockProbe = Get-BuildBlockingLocks -Root $Root
        if ($lockProbe.ProbeErrors.Count -gt 0) {
            $script:consecutiveBuildFailures++
            $backoffSec = Get-BuildFailureBackoffSeconds
            Write-Watchdog ("RESULT=BUILD_LOCK_PROBE_FAILED probe_error_count={0} locked_count={1} readonly_count={2} consecutive_build_failures={3} backoff_seconds={4} probe_errors={5}" -f `
                    $lockProbe.ProbeErrors.Count, $lockProbe.Locked.Count, $lockProbe.ReadOnly.Count, `
                    $script:consecutiveBuildFailures, $backoffSec, (Format-BuildProbeErrors $lockProbe.ProbeErrors))
            return [PSCustomObject]@{ ExitCode = 11; Result = 'RESULT=BUILD_LOCK_PROBE_FAILED'; BackoffSeconds = $backoffSec }
        }
        if ($lockProbe.Locked.Count -gt 0) {
            $script:consecutiveBuildFailures++
            $backoffSec = Get-BuildFailureBackoffSeconds
            $holderPids = Get-BuildLockHolderPids -Root $Root
            Write-Watchdog ("RESULT=BLOCKED_DLL_LOCKED locked_count={0} readonly_count={1} consecutive_build_failures={2} backoff_seconds={3} holder_pids_best_effort={4} locked_files={5}" -f `
                    $lockProbe.Locked.Count, $lockProbe.ReadOnly.Count, $script:consecutiveBuildFailures, $backoffSec, `
                    $(if ($holderPids.Count -gt 0) { $holderPids -join ',' } else { '-' }), `
                    (Format-WatchdogValue (($lockProbe.Locked | Select-Object -First 8) -join ';')))
            return [PSCustomObject]@{ ExitCode = 10; Result = 'RESULT=BLOCKED_DLL_LOCKED'; BackoffSeconds = $backoffSec }
        }

        # The bounded write probe can take long enough for another process to
        # bind the MCP port. Recheck immediately before handing control to UBT.
        $preBuildPortGate = Get-MonolithRecoveryPortGate -Port $port
        if (-not $preBuildPortGate.Valid) {
            Write-Watchdog ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint phase=before_build listener_port={0} listener_count={1} listener_pids={2} listener_gate={3}" -f `
                    $port, $preBuildPortGate.Count, (($preBuildPortGate.Pids) -join ','), $preBuildPortGate.ErrorCode)
            return [PSCustomObject]@{ ExitCode = 3; Result = 'RESULT=BLOCKED' }
        }

        if (-not (Invoke-EditorBuild -Root $Root)) {
            $script:consecutiveBuildFailures++
            $backoffSec = Get-BuildFailureBackoffSeconds
            return [PSCustomObject]@{ ExitCode = 4; Result = 'RESULT=BUILD_FAILED'; BackoffSeconds = $backoffSec }
        }
        $script:consecutiveBuildFailures = 0
    }
    else {
        Write-Watchdog 'build_skipped reason=NoBuildBeforeRestart'
    }

    $preRestartReindexOk = Invoke-PreRestartReindex -Root $Root
    if (-not $preRestartReindexOk) {
        # Index freshness is important, but an offline commandlet/disk failure
        # must not strand the control plane. Recover MCP first, then retry the
        # requested maintenance through the live endpoint.
        Write-Watchdog 'restart_reindex_deferred phase=pre_restart availability_recovery_continues=true'
    }

    $recoverResult = Invoke-Recover -Root $Root -ForceLaunch
    if ($recoverResult.ExitCode -eq 0) {
        $postRecoverReindexOk = Invoke-PostRecoverReindex -Root $Root -RetryPreRestartTargets:(-not $preRestartReindexOk)
        if (-not $postRecoverReindexOk) {
            # Keep the successful availability result. The explicit maintenance
            # failure event remains actionable and the healthy loop can retry
            # scheduled maintenance without killing the recovered editor.
            $recoverResult | Add-Member -NotePropertyName MaintenanceSucceeded -NotePropertyValue $false -Force
        }
    }

    return $recoverResult
}

$script:dailyReindexSchedule = $null
$script:dailyReindexTimeZoneInfo = $null
$script:lastDailyReindexAttemptDate = $null
$script:runDailyReindexNowConsumed = $false

if (-not $DisableDailyReindex -and -not $ProbeOnly -and -not $ProbeBuildLocksOnly) {
    if ($DailyReindexWaitTimeoutSec -lt 0 -or $DailyReindexWaitPollSec -le 0 -or
        $DailyReindexActionTimeoutSec -le 0 -or $DailyGraphCooldownSeconds -lt 0 -or
        @($DailyReindexTargets).Count -eq 0 -or
        (-not $SkipRestartReindex -and @($RestartReindexTargets).Count -eq 0)) {
        Write-Watchdog ("RESULT=BLOCKED reason=invalid_reindex_arguments wait_timeout={0} wait_poll={1} action_timeout={2} graph_cooldown={3} daily_target_count={4} restart_target_count={5}" -f `
                $DailyReindexWaitTimeoutSec, $DailyReindexWaitPollSec, $DailyReindexActionTimeoutSec, $DailyGraphCooldownSeconds, @($DailyReindexTargets).Count, @($RestartReindexTargets).Count)
        exit 3
    }

    $script:dailyReindexSchedule = Get-DailyReindexSchedule
    if ($null -eq $script:dailyReindexSchedule) {
        exit 3
    }

    $script:dailyReindexTimeZoneInfo = Get-DailyReindexTimeZoneInfo
    if ($null -eq $script:dailyReindexTimeZoneInfo) {
        exit 3
    }
}

$restartAttempts = 0
$script:consecutiveBuildFailures = 0

# Terminal logging guarantee: every abnormal end must leave a last line in
# watchdog.jsonl (2026-07-03/04 incidents ended with no terminal record).
trap {
    try {
        Write-Watchdog ("RESULT=FATAL error={0}" -f (Format-WatchdogValue $_.Exception.Message))
    }
    catch {}
    exit 9
}

Write-Watchdog ("watchdog_start watchdog_pid={0} poll_interval_sec={1} max_restart_attempts={2} probe_only={3} once={4} project_root={5}" -f `
        $PID, $PollIntervalSec, $MaxRestartAttempts, [bool]$ProbeOnly, [bool]$Once, $(if ($ProjectRoot) { $ProjectRoot } else { '-' }))

$script:hostRoot = Resolve-HostRoot
if (-not $script:hostRoot) {
    Write-Watchdog 'RESULT=BLOCKED reason=host_root_not_found detail=no *.uproject found upward from the script and no -ProjectRoot given'
    exit 3
}

if ($ProbeBuildLocksOnly) {
    $hostRoot = $script:hostRoot
    $lockProbe = Get-BuildBlockingLocks -Root $hostRoot
    if ($lockProbe.ProbeErrors.Count -gt 0) {
        Write-Watchdog ("RESULT=BUILD_LOCK_PROBE_FAILED probe_error_count={0} locked_count={1} readonly_count={2} probe_errors={3}" -f `
                $lockProbe.ProbeErrors.Count, $lockProbe.Locked.Count, $lockProbe.ReadOnly.Count, `
                (Format-BuildProbeErrors $lockProbe.ProbeErrors))
        exit 11
    }
    if ($lockProbe.Locked.Count -gt 0) {
        $holderPids = Get-BuildLockHolderPids -Root $hostRoot
        Write-Watchdog ("RESULT=BUILD_LOCKS_PRESENT locked_count={0} readonly_count={1} holder_pids_best_effort={2} locked_files={3}" -f `
                $lockProbe.Locked.Count, $lockProbe.ReadOnly.Count, `
                $(if ($holderPids.Count -gt 0) { $holderPids -join ',' } else { '-' }), `
                (Format-WatchdogValue (($lockProbe.Locked | Select-Object -First 8) -join ';')))
        exit 2
    }
    Write-Watchdog ("RESULT=BUILD_LOCKS_CLEAR candidate_count={0} readonly_count={1}" -f `
            (Get-BuildLockCandidateFiles -Root $hostRoot).Count, $lockProbe.ReadOnly.Count)
    exit 0
}

while ($true) {
    $health = Get-MonolithHealth
    if ($health) {
        Write-Watchdog ("RESULT=MCP_UP version={0} tools_registered={1} pid={2} uptime_seconds={3}" -f `
                $health.version, $health.tools_registered, $health.pid, [int]$health.uptime_seconds)
        if ($script:restartAttempts -ne 0) {
            # A healthy endpoint clears the restart budget; without this the
            # counter accumulates across weeks and eventually locks the
            # watchdog into permanent RESTART_LIMIT.
            Write-Watchdog ("restart_attempts_reset previous={0}" -f $script:restartAttempts)
            $script:restartAttempts = 0
            $script:consecutiveBuildFailures = 0
        }
        $dailyResult = Invoke-DailyReindexIfDue
        if ($dailyResult.Ran -and -not $dailyResult.Succeeded -and $Once) {
            exit $dailyResult.ExitCode
        }
        if ($Once -or $ProbeOnly) { exit 0 }
        Start-Sleep -Seconds $PollIntervalSec
        continue
    }

    $hostRoot = $script:hostRoot

    $editorProcs = Get-EditorServerCandidates -Root $hostRoot
    $port = Get-McpHealthPort
    $portGate = Get-MonolithRecoveryPortGate -Port $port
    $invalidHttpIdentity = $script:lastHealthStatusCode -eq 200 -and
        $script:lastHealthErrorClass -in @('invalid_json', 'health_contract', 'health_identity')
    if ($invalidHttpIdentity -or -not $portGate.Valid) {
        Write-Watchdog ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint health_error={0} detail={1} url={2} listener_port={3} listener_count={4} listener_pids={5} listener_gate={6}" -f `
                $script:lastHealthErrorClass, (Format-WatchdogValue $script:lastHealthErrorCode), $healthUrl, `
                $port, $portGate.Count, (($portGate.Pids) -join ','), $portGate.ErrorCode)
        exit 3
    }
    if ($ProbeOnly) {
        Write-Watchdog ("RESULT=MCP_DOWN probe_only=true url={0} editor_candidate_count={1} editor_candidate_pids={2}" -f `
                $healthUrl, $editorProcs.Count, (($editorProcs.Id) -join ','))
        exit 2
    }

    if ($editorProcs.Count -eq 0) {
        Write-Watchdog 'mcp_down editor_missing action=restart_sequence'
        $recoverResult = Invoke-RestartSequence -Root $hostRoot -Reason 'editor_missing'
    }
    else {
        Write-Watchdog ("mcp_down editor_processes_detected pids={0} action=recover_without_build" -f (($editorProcs.Id) -join ','))
        $recoverResult = Invoke-Recover -Root $hostRoot
        if ($recoverResult.ExitCode -ne 0) {
            $headlessProcs = Get-HeadlessEditorCandidates -Root $hostRoot
            if ($headlessProcs.Count -gt 0) {
                # A listener may have appeared while the bounded recovery child
                # was running. Re-probe health and then require a clear port
                # immediately before the only destructive process-stop branch.
                $lateHealth = Get-MonolithHealth
                if ($lateHealth) {
                    Write-Watchdog ("RESULT=MCP_UP version={0} tools_registered={1} pid={2} uptime_seconds={3} phase=before_headless_stop" -f `
                            $lateHealth.version, $lateHealth.tools_registered, $lateHealth.pid, [int]$lateHealth.uptime_seconds)
                    $recoverResult = [PSCustomObject]@{ ExitCode = 0; Result = 'RESULT=MCP_UP' }
                }
                else {
                    $preStopPortGate = Get-MonolithRecoveryPortGate -Port $port
                    if (-not $preStopPortGate.Valid) {
                        Write-Watchdog ("RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint phase=before_headless_stop listener_port={0} listener_count={1} listener_pids={2} listener_gate={3}" -f `
                                $port, $preStopPortGate.Count, (($preStopPortGate.Pids) -join ','), $preStopPortGate.ErrorCode)
                        $recoverResult = [PSCustomObject]@{ ExitCode = 3; Result = 'RESULT=BLOCKED' }
                    }
                    else {
                        Write-Watchdog ("recover_failed_unhealthy_headless restart=true recover_exit_code={0} headless_pids={1}" -f `
                                $recoverResult.ExitCode, (($headlessProcs.Id) -join ','))
                        Stop-HeadlessEditors -Processes $headlessProcs
                        $recoverResult = Invoke-RestartSequence -Root $hostRoot -Reason 'unhealthy_headless_editor'
                    }
                }
            }
            else {
                Write-Watchdog ("recover_failed_no_headless_restart recover_exit_code={0} editor_pids={1}" -f `
                        $recoverResult.ExitCode, (($editorProcs.Id) -join ','))
            }
        }
    }

    if ($Once) {
        if ($recoverResult.ExitCode -eq 0) {
            $dailyResult = Invoke-DailyReindexIfDue
            if ($dailyResult.Ran -and -not $dailyResult.Succeeded) {
                exit $dailyResult.ExitCode
            }
        }
        exit $recoverResult.ExitCode
    }

    $sleepSec = $PollIntervalSec
    if ($recoverResult -and $recoverResult.PSObject.Properties['BackoffSeconds'] -and $recoverResult.BackoffSeconds -gt 0) {
        $sleepSec = $recoverResult.BackoffSeconds
    }
    Start-Sleep -Seconds $sleepSec
}
