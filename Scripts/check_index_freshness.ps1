<#
.SYNOPSIS
Deterministic source/project index freshness chain: health -> stale detection
-> repair recommendation -> optional sanctioned repair -> re-verification.

.DESCRIPTION
Wraps Binaries/monolith_query.exe health checks for EngineSource.db and
ProjectIndex.db, extracts source repairs from the structured
maintenance_recommendation / next_actions contract, and prints exact offline
and live-MCP repair commands. Source FTS repairs must name one bounded target;
the script never widens a graph_nodes, symbols, console_objects, or source
recommendation to target=all. With -Execute it runs only those health-indicated
repairs (repair_crg_cache / repair_fts) through the offline CLI and re-runs
health to confirm the result.

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

# Map a legacy project "... -> repair_xxx" warning to exact offline argv.
# Source repairs deliberately do not use this text parser: source health owns a
# structured, target-bearing next_actions contract and must fail closed if it
# is missing or malformed.
function Get-LegacyProjectRepairArguments {
    param([string]$Namespace, [string]$Hint)
    if ($Namespace -ne 'project') { return $null }
    switch ($Hint) {
        'repair_crg_cache' { return @('project', 'repair_crg_cache', '--execute') }
        'repair_fts' { return @('project', 'repair_fts', '--target=all', '--execute') }
        default { return $null }
    }
}

function Convert-SourceRepairAction {
    param([AllowNull()][object]$ActionValue)

    $result = [PSCustomObject]@{
        IsRepair = $false
        Valid = $true
        Arguments = @()
        Reason = ''
    }

    if ($ActionValue -isnot [string]) {
        $result.IsRepair = $true
        $result.Valid = $false
        $result.Reason = 'source next_actions contains a non-string entry'
        return $result
    }

    $actionText = $ActionValue.Trim()
    if ($actionText -notmatch '(^|\.)repair_[a-z_]+') {
        return $result
    }

    $result.IsRepair = $true
    if ($actionText -match '^source\.repair_fts(?<tail>.*)$') {
        $tail = $Matches['tail']
        if ($tail -notmatch '^\s+target=(?<target>[a-z_]+)\s*$') {
            $result.Valid = $false
            $result.Reason = "malformed source.repair_fts next_action '$actionText'; exactly one target=<name> argument is required"
            return $result
        }

        $target = $Matches['target']
        $allowedTargets = @('graph_nodes', 'symbols', 'console_objects', 'source')
        if ($target -notin $allowedTargets) {
            $result.Valid = $false
            $result.Reason = "unsupported or over-broad source.repair_fts target '$target'; expected graph_nodes|symbols|console_objects|source"
            return $result
        }

        $result.Arguments = @('source', 'repair_fts', "--target=$target", '--execute')
        return $result
    }

    if ($actionText -match '^source\.repair_crg_cache(?<tail>.*)$') {
        $tail = $Matches['tail']
        if ([string]::IsNullOrWhiteSpace($tail)) {
            # Canonicalize the legacy/default spelling so plan comparison and
            # logging always make the expensive full scope explicit.
            $result.Arguments = @('source', 'repair_crg_cache', '--scope=all', '--execute')
            return $result
        }
        if ($tail -match '^\s+scope=(?<scope>all|override_edges)\s*$') {
            $result.Arguments = @('source', 'repair_crg_cache', "--scope=$($Matches['scope'])", '--execute')
            return $result
        }

        $result.Valid = $false
        $result.Reason = "malformed source.repair_crg_cache next_action '$actionText'"
        return $result
    }

    $result.Valid = $false
    $result.Reason = "unknown source repair next_action '$actionText'"
    return $result
}

function Get-BooleanPropertyState {
    param([AllowNull()][object]$Object, [string]$Name)
    if (-not $Object) {
        return [PSCustomObject]@{ Present = $false; Valid = $false; Value = $false }
    }
    $property = $Object.PSObject.Properties[$Name]
    if (-not $property) {
        return [PSCustomObject]@{ Present = $false; Valid = $true; Value = $false }
    }
    $isBoolean = $property.Value -is [bool]
    return [PSCustomObject]@{
        Present = $true
        Valid = $isBoolean
        Value = $(if ($isBoolean) { [bool]$property.Value } else { $false })
    }
}

function Format-LiveRepairRecommendation {
    param([string[]]$Arguments)
    $params = [ordered]@{ execute = $true }
    foreach ($argument in @($Arguments | Select-Object -Skip 2)) {
        if ($argument -match '^--(?<name>target|scope)=(?<value>[a-z_]+)$') {
            $params[$Matches['name']] = $Matches['value']
        }
    }
    $paramsJson = $params | ConvertTo-Json -Compress
    return ('{0}_query("{1}", {2})' -f $Arguments[0], $Arguments[1], $paramsJson)
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
        RepairErrors = @()
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

    if ($Namespace -eq 'source') {
        $maintenanceProperty = $result.Json.PSObject.Properties['maintenance_recommendation']
        $nextActionsProperty = $result.Json.PSObject.Properties['next_actions']
        $maintenance = if ($maintenanceProperty) { $maintenanceProperty.Value } else { $null }
        $maintenanceRequiredState = Get-BooleanPropertyState -Object $maintenance -Name 'maintenance_required'

        if (-not $maintenanceProperty -or -not $maintenanceRequiredState.Present -or -not $maintenanceRequiredState.Valid) {
            if ($state.Status -ne 'ok') {
                $state.RepairErrors += 'source health warning lacks a valid maintenance_recommendation.maintenance_required boolean'
            }
        }
        elseif ($maintenanceRequiredState.Value) {
            if (-not $nextActionsProperty -or $null -eq $nextActionsProperty.Value) {
                $state.RepairErrors += 'source maintenance is required but next_actions is missing'
            }
            else {
                foreach ($nextAction in @($nextActionsProperty.Value)) {
                    $parsed = Convert-SourceRepairAction -ActionValue $nextAction
                    if (-not $parsed.IsRepair) { continue }
                    if (-not $parsed.Valid) {
                        $state.RepairErrors += $parsed.Reason
                        continue
                    }
                    $state.Repairs += , $parsed.Arguments
                }
            }

            # Full CRG repair rebuilds source_override_edges as part of the same
            # transaction. Accept older health payloads that list both scopes,
            # but execute only the full superset once.
            $hasFullCrgRepair = @($state.Repairs | Where-Object {
                $_.Count -ge 2 -and $_[1] -eq 'repair_crg_cache' -and
                $_ -contains '--scope=all'
            }).Count -gt 0
            if ($hasFullCrgRepair) {
                $state.Repairs = @($state.Repairs | Where-Object {
                    -not ($_.Count -ge 2 -and $_[1] -eq 'repair_crg_cache' -and
                        $_ -contains '--scope=override_edges')
                })
            }

            $requiredChecks = @(
                @{ Flag = 'repair_graph_nodes_fts_required'; Action = 'repair_fts'; Option = '--target=graph_nodes' },
                @{ Flag = 'repair_symbols_fts_required'; Action = 'repair_fts'; Option = '--target=symbols' }
            )
            foreach ($required in $requiredChecks) {
                $flag = Get-BooleanPropertyState -Object $maintenance -Name $required.Flag
                if (-not $flag.Valid) {
                    $state.RepairErrors += "source maintenance flag '$($required.Flag)' is not boolean"
                    continue
                }
                if (-not $flag.Value) { continue }
                $matching = @($state.Repairs | Where-Object {
                    $_.Count -ge 2 -and $_[1] -eq $required.Action -and
                    (-not $required.Option -or $_ -contains $required.Option)
                })
                if ($matching.Count -eq 0) {
                    $expected = if ($required.Option) { "$($required.Action) $($required.Option)" } else { $required.Action }
                    $state.RepairErrors += "source maintenance flag '$($required.Flag)' lacks exact next_action '$expected'"
                }
            }

            $fullCrgRequired = Get-BooleanPropertyState -Object $maintenance -Name 'repair_crg_cache_required'
            $overrideRequired = Get-BooleanPropertyState -Object $maintenance -Name 'repair_override_edges_required'
            if (-not $fullCrgRequired.Valid) {
                $state.RepairErrors += "source maintenance flag 'repair_crg_cache_required' is not boolean"
            }
            if (-not $overrideRequired.Valid) {
                $state.RepairErrors += "source maintenance flag 'repair_override_edges_required' is not boolean"
            }
            if ($fullCrgRequired.Valid -and $overrideRequired.Valid) {
                $hasFullCrgRepair = @($state.Repairs | Where-Object {
                    $_.Count -ge 2 -and $_[1] -eq 'repair_crg_cache' -and
                    $_ -contains '--scope=all'
                }).Count -gt 0
                $hasOverrideRepair = @($state.Repairs | Where-Object {
                    $_.Count -ge 2 -and $_[1] -eq 'repair_crg_cache' -and
                    $_ -contains '--scope=override_edges'
                }).Count -gt 0

                if ($fullCrgRequired.Value -and -not $hasFullCrgRepair) {
                    $state.RepairErrors += "source maintenance flag 'repair_crg_cache_required' lacks exact next_action 'repair_crg_cache --scope=all'"
                }
                elseif (-not $fullCrgRequired.Value -and $overrideRequired.Value -and -not $hasOverrideRepair) {
                    $state.RepairErrors += "source maintenance flag 'repair_override_edges_required' lacks exact next_action 'repair_crg_cache --scope=override_edges'"
                }
                elseif (-not $fullCrgRequired.Value -and -not $overrideRequired.Value -and ($hasFullCrgRepair -or $hasOverrideRepair)) {
                    $state.RepairErrors += 'source next_actions contains an unindicated CRG repair'
                }
            }

            $ftsRequired = Get-BooleanPropertyState -Object $maintenance -Name 'repair_fts_required'
            if (-not $ftsRequired.Valid) {
                $state.RepairErrors += "source maintenance flag 'repair_fts_required' is not boolean"
            }
            elseif ($ftsRequired.Value -and @($state.Repairs | Where-Object { $_.Count -ge 2 -and $_[1] -eq 'repair_fts' }).Count -eq 0) {
                $state.RepairErrors += 'source maintenance requires FTS repair but no exact target-bearing repair_fts next_action exists'
            }

            if ($state.RepairErrors.Count -gt 0) {
                # One malformed structured repair invalidates the whole source
                # repair plan. Never execute a valid-looking subset.
                $state.Repairs = @()
            }
        }
    }
    else {
        foreach ($warning in $state.Warnings) {
            if ($warning -match '->\s*(repair_[a-z_]+)') {
                $arguments = Get-LegacyProjectRepairArguments -Namespace $Namespace -Hint $Matches[1]
                if ($arguments) { $state.Repairs += , $arguments }
                else { $state.RepairErrors += "unknown project repair hint '$($Matches[1])'" }
            }
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
    foreach ($repairError in $State.RepairErrors) {
        Write-Output ("  REPAIR_REJECTED {0}" -f $repairError)
    }
    $seen = @{}
    foreach ($arguments in $State.Repairs) {
        if ($arguments.Count -eq 0) { continue }
        $key = $arguments -join ' '
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        Write-Output ("  RECOMMEND offline: {0} {1}" -f $QueryExe, $key)
        Write-Output ("  RECOMMEND live: {0}" -f (Format-LiveRepairRecommendation -Arguments $arguments))
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
