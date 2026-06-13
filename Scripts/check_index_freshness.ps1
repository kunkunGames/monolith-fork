<#
.SYNOPSIS
Deterministic source/project index freshness chain: health -> stale detection
-> repair recommendation -> optional sanctioned repair -> re-verification.

.DESCRIPTION
Wraps Binaries/monolith_query.exe health checks for EngineSource.db and
ProjectIndex.db, extracts the repair action each health warning itself
indicates (the "... -> repair_crg_cache" hint format), and prints exact
offline and live-MCP repair commands. With -Execute it runs only those
warning-indicated repairs (repair_crg_cache / repair_fts) through the offline
CLI and re-runs health to confirm the result.

Writing to the on-disk DBs while the editor's Monolith MCP server is up is
refused by default; pass -AllowLiveEditor to override, or run the matching
live repair action through MCP instead.

.PARAMETER Target
Which index to check: source, project, or all (default all).

.PARAMETER Execute
Run the warning-indicated repairs offline instead of only reporting them.

.PARAMETER AllowLiveEditor
Permit -Execute repairs while the MCP /health endpoint is reachable.

.PARAMETER QueryExe
Override path to monolith_query.exe (default: <plugin>/Binaries/monolith_query.exe).

.PARAMETER McpUrl
MCP endpoint used only to detect a live editor (default: MONOLITH_URL env var
or http://localhost:9316/mcp).

.OUTPUTS
Line-oriented status: DB= / WARNING / RECOMMEND / REPAIR / RESULT= tokens.

Exit codes:
  0  every checked index is ok (or repairs brought them back to ok)
  2  warnings or health errors found (report mode)
  3  blocked: monolith_query.exe not found or health output unparseable
  4  -Execute ran but warnings remain
  6  -Execute refused: MCP endpoint is up and -AllowLiveEditor was not passed
#>
[CmdletBinding()]
param(
    [ValidateSet('source', 'project', 'all')]
    [string]$Target = 'all',
    [switch]$Execute,
    [switch]$AllowLiveEditor,
    [string]$QueryExe,
    [string]$McpUrl = $(if ($env:MONOLITH_URL) { $env:MONOLITH_URL } else { 'http://localhost:9316/mcp' })
)

$ErrorActionPreference = 'Continue'

if (-not $QueryExe) {
    $QueryExe = Join-Path (Split-Path -Parent $PSScriptRoot) 'Binaries\monolith_query.exe'
}
if (-not (Test-Path $QueryExe)) {
    Write-Output "RESULT=BLOCKED reason=query_exe_missing path=$QueryExe"
    exit 3
}

$namespaces = if ($Target -eq 'all') { @('source', 'project') } else { @($Target) }

function Invoke-QueryJson {
    param([string[]]$Arguments)
    $raw = & $QueryExe @Arguments 2>&1
    $stdout = ($raw | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] }) -join "`n"
    $stderr = ($raw | Where-Object { $_ -is [System.Management.Automation.ErrorRecord] }) -join "`n"
    $json = $null
    if ($stdout.Trim()) {
        try { $json = $stdout | ConvertFrom-Json } catch { $json = $null }
    }
    return [PSCustomObject]@{ ExitCode = $LASTEXITCODE; Json = $json; Stdout = $stdout; Stderr = $stderr }
}

function Test-McpUp {
    $healthUrl = $McpUrl -replace '/mcp/?$', '/health'
    try {
        $resp = Invoke-WebRequest -Uri $healthUrl -Method Get -TimeoutSec 3 -UseBasicParsing
        return ($resp.StatusCode -eq 200)
    }
    catch { return $false }
}

# Map a "... -> repair_xxx" hint from a health warning to the exact offline repair argv.
function Get-RepairArguments {
    param([string]$Namespace, [string]$Hint)
    switch ($Hint) {
        'repair_crg_cache' { return @($Namespace, 'repair_crg_cache', '--execute') }
        'repair_fts' {
            if ($Namespace -eq 'project') { return @('project', 'repair_fts', '--target=all', '--execute') }
            return @('source', 'repair_fts', '--execute')
        }
        default { return $null }
    }
}

function Get-HealthState {
    param([string]$Namespace)
    $result = Invoke-QueryJson -Arguments @($Namespace, 'health', '--include-counts=true')
    $state = [PSCustomObject]@{
        Namespace = $Namespace
        Status    = 'error'
        Summary   = ''
        Warnings  = @()
        Repairs   = @()
        Error     = $null
    }
    if ($result.ExitCode -ne 0 -or -not $result.Json) {
        $detail = if ($result.Stderr) { $result.Stderr } else { $result.Stdout }
        $state.Error = ($detail -replace '\s+', ' ').Trim()
        return $state
    }
    $state.Status = [string]$result.Json.status
    $state.Summary = [string]$result.Json.summary
    $state.Warnings = @($result.Json.warnings)
    foreach ($warning in $state.Warnings) {
        if ($warning -match '->\s*(repair_[a-z_]+)') {
            $arguments = Get-RepairArguments -Namespace $Namespace -Hint $Matches[1]
            if ($arguments) { $state.Repairs += , $arguments }
            else { $state.Repairs += , @() } # unknown hint: counted, never auto-run
        }
    }
    return $state
}

function Write-HealthReport {
    param($State)
    if ($State.Error) {
        Write-Output ("DB={0} STATUS=error WARNINGS=- DETAIL={1}" -f $State.Namespace, $State.Error)
        return
    }
    Write-Output ("DB={0} STATUS={1} WARNINGS={2} SUMMARY={3}" -f $State.Namespace, $State.Status, $State.Warnings.Count, $State.Summary)
    foreach ($warning in $State.Warnings) {
        Write-Output ("  WARNING {0}" -f $warning)
    }
    $seen = @{}
    foreach ($arguments in $State.Repairs) {
        if ($arguments.Count -eq 0) { continue }
        $key = $arguments -join ' '
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        Write-Output ("  RECOMMEND offline: {0} {1}" -f $QueryExe, $key)
        Write-Output ('  RECOMMEND live: {0}_query("{1}", {{ "execute": true }})' -f $State.Namespace, $arguments[1])
    }
}

$mcpUp = Test-McpUp
Write-Output ("INFO mcp_endpoint={0} url={1}" -f $(if ($mcpUp) { 'up' } else { 'down' }), $McpUrl)

$states = @()
foreach ($namespace in $namespaces) {
    $state = Get-HealthState -Namespace $namespace
    Write-HealthReport -State $state
    $states += $state
}

$needsAttention = @($states | Where-Object { $_.Error -or $_.Status -ne 'ok' })
$pendingRepairs = @($states | Where-Object { @($_.Repairs | Where-Object { $_.Count -gt 0 }).Count -gt 0 })

if (-not $Execute) {
    if ($needsAttention.Count -eq 0) {
        Write-Output 'RESULT=OK'
        exit 0
    }
    Write-Output ("RESULT=ATTENTION dbs={0}" -f (($needsAttention.Namespace) -join ','))
    exit 2
}

# -Execute mode
if ($needsAttention.Count -eq 0) {
    Write-Output 'RESULT=OK nothing_to_repair=true'
    exit 0
}
if ($pendingRepairs.Count -eq 0) {
    Write-Output 'RESULT=ATTENTION no_auto_repair_available=true (health problems exist but no warning carries a repair_* hint; fix the source data or run indexing instead of masking)'
    exit 2
}
if ($mcpUp -and -not $AllowLiveEditor) {
    Write-Output 'RESULT=REFUSED reason=mcp_endpoint_up detail=run the matching live repair action through MCP, or pass -AllowLiveEditor to write the on-disk DB anyway'
    exit 6
}

foreach ($state in $pendingRepairs) {
    $seen = @{}
    foreach ($arguments in $state.Repairs) {
        if ($arguments.Count -eq 0) { continue }
        $key = $arguments -join ' '
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        Write-Output ("REPAIR db={0} cmd={1}" -f $state.Namespace, $key)
        $result = Invoke-QueryJson -Arguments $arguments
        if ($result.ExitCode -ne 0) {
            $detail = if ($result.Stderr) { $result.Stderr } else { $result.Stdout }
            Write-Output ("REPAIR_RESULT db={0} exit={1} DETAIL={2}" -f $state.Namespace, $result.ExitCode, (($detail -replace '\s+', ' ').Trim()))
        }
        elseif ($result.Json -and $result.Json.PSObject.Properties['status']) {
            Write-Output ("REPAIR_RESULT db={0} exit=0 status={1}" -f $state.Namespace, $result.Json.status)
        }
        else {
            Write-Output ("REPAIR_RESULT db={0} exit=0" -f $state.Namespace)
        }
    }
}

Write-Output 'INFO re-running health after repair'
$stillBad = @()
foreach ($state in $pendingRepairs) {
    $after = Get-HealthState -Namespace $state.Namespace
    Write-HealthReport -State $after
    Write-Output ("VERIFY db={0} before={1} after={2}" -f $state.Namespace, $state.Status, $(if ($after.Error) { 'error' } else { $after.Status }))
    if ($after.Error -or $after.Status -ne 'ok') { $stillBad += $after }
}

# Indexes that needed attention but had no runnable repair stay unresolved by design.
$unrepairable = @($needsAttention | Where-Object { $pendingRepairs -notcontains $_ })
if ($stillBad.Count -eq 0 -and $unrepairable.Count -eq 0) {
    Write-Output 'RESULT=REPAIRED'
    exit 0
}
Write-Output ("RESULT=WARNINGS_REMAIN dbs={0}" -f ((@($stillBad + $unrepairable).Namespace | Select-Object -Unique) -join ','))
exit 4
