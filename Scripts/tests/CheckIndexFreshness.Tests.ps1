# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

$CheckScriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'check_index_freshness.ps1'
$PowerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

function ConvertTo-TestBase64 {
    param([string]$Text)
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text))
}

function New-SourceHealthJson {
    param(
        [string[]]$NextActions,
        [string]$SpecificFtsTarget,
        [bool]$FullCrgRequired = $false,
        [bool]$OverrideRequired = $false,
        [bool]$MaintenanceRequired = $true
    )

    $maintenance = [ordered]@{
        maintenance_required = $MaintenanceRequired
        repair_fts_required = $MaintenanceRequired -and -not [string]::IsNullOrWhiteSpace($SpecificFtsTarget)
        repair_graph_nodes_fts_required = $MaintenanceRequired -and $SpecificFtsTarget -eq 'graph_nodes'
        repair_symbols_fts_required = $MaintenanceRequired -and $SpecificFtsTarget -eq 'symbols'
        repair_crg_cache_required = $MaintenanceRequired -and $FullCrgRequired
        repair_override_edges_required = $MaintenanceRequired -and $OverrideRequired
    }
    return ([ordered]@{
        status = $(if ($MaintenanceRequired) { 'warning' } else { 'ok' })
        summary = $(if ($MaintenanceRequired) { 'repair required' } else { 'healthy' })
        warnings = $(if ($MaintenanceRequired) { @('structured repair required') } else { @() })
        maintenance_recommendation = $maintenance
        next_actions = @($NextActions)
    } | ConvertTo-Json -Compress -Depth 8)
}

function Invoke-IndexFreshnessTest {
    param([switch]$Execute)
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $CheckScriptPath,
        '-Target', 'source',
        '-QueryExe', $script:FakeQueryPath,
        '-McpUrl', 'http://127.0.0.1:1/mcp'
    )
    if ($Execute) { $arguments += '-Execute' }
    $output = & $PowerShellExe @arguments 2>&1
    return [PSCustomObject]@{
        ExitCode = $LASTEXITCODE
        Output = ($output | Out-String)
    }
}

Describe 'check_index_freshness structured source repair routing' {
    BeforeEach {
        $script:FakeQueryPath = Join-Path $TestDrive 'fake_monolith_query.ps1'
        $script:QueryLogPath = Join-Path $TestDrive 'query.log'
        $script:RepairStatePath = Join-Path $TestDrive 'repaired.state'
        Remove-Item -LiteralPath $script:QueryLogPath, $script:RepairStatePath -Force -ErrorAction SilentlyContinue

        @'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$CliArgs)

Add-Content -LiteralPath $env:MONOLITH_TEST_QUERY_LOG -Value ($CliArgs -join ' ')
if ($CliArgs.Count -ge 2 -and $CliArgs[1] -eq 'health') {
    $payload = if (Test-Path -LiteralPath $env:MONOLITH_TEST_REPAIR_STATE) {
        $env:MONOLITH_TEST_HEALTHY_B64
    }
    else {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64
    }
    [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payload))
    exit 0
}

if ($CliArgs.Count -ge 2 -and $CliArgs[1] -match '^repair_') {
    New-Item -ItemType File -Path $env:MONOLITH_TEST_REPAIR_STATE -Force | Out-Null
    '{"status":"ok","summary":"repair complete","warnings":[]}'
    exit 0
}

'{"status":"error","summary":"unexpected fake query call","warnings":[]}'
exit 9
'@ | Set-Content -LiteralPath $script:FakeQueryPath -Encoding UTF8

        $env:MONOLITH_TEST_QUERY_LOG = $script:QueryLogPath
        $env:MONOLITH_TEST_REPAIR_STATE = $script:RepairStatePath
        $env:MONOLITH_TEST_HEALTHY_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson -NextActions @() -MaintenanceRequired $false)
    }

    AfterEach {
        Remove-Item Env:MONOLITH_TEST_QUERY_LOG -ErrorAction SilentlyContinue
        Remove-Item Env:MONOLITH_TEST_REPAIR_STATE -ErrorAction SilentlyContinue
        Remove-Item Env:MONOLITH_TEST_INITIAL_HEALTH_B64 -ErrorAction SilentlyContinue
        Remove-Item Env:MONOLITH_TEST_HEALTHY_B64 -ErrorAction SilentlyContinue
    }

    It 'preserves every bounded FTS target in offline and live recommendations' {
        foreach ($target in @('graph_nodes', 'symbols', 'console_objects', 'source')) {
            Remove-Item -LiteralPath $script:QueryLogPath, $script:RepairStatePath -Force -ErrorAction SilentlyContinue
            $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
                New-SourceHealthJson `
                    -NextActions @("source.repair_fts target=$target") `
                    -SpecificFtsTarget $target)

            $run = Invoke-IndexFreshnessTest

            $run.ExitCode | Should Be 2
            $run.Output | Should Match ([regex]::Escape("source repair_fts --target=$target --execute"))
            $run.Output | Should Match ([regex]::Escape("source_query(`"repair_fts`", {`"execute`":true,`"target`":`"$target`"})"))
            $run.Output | Should Not Match '--target=all'
        }
    }

    It 'executes only the exact structured target and verifies health afterward' {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson `
                -NextActions @('source.repair_fts target=graph_nodes') `
                -SpecificFtsTarget 'graph_nodes')

        $run = Invoke-IndexFreshnessTest -Execute
        $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

        $run.ExitCode | Should Be 0
        $run.Output | Should Match 'RESULT=REPAIRED'
        $calls.Count | Should Be 3
        $calls[0] | Should Be 'source health --include-counts=true'
        $calls[1] | Should Be 'source repair_fts --target=graph_nodes --execute'
        $calls[2] | Should Be 'source health --include-counts=true'
        ($calls -join "`n") | Should Not Match '--target=all'
    }

    It 'executes only the override-edge scope when core CRG is healthy' {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson `
                -NextActions @('source.repair_crg_cache scope=override_edges') `
                -OverrideRequired $true)

        $run = Invoke-IndexFreshnessTest -Execute
        $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

        $run.ExitCode | Should Be 0
        $run.Output | Should Match 'RESULT=REPAIRED'
        $calls.Count | Should Be 3
        $calls[0] | Should Be 'source health --include-counts=true'
        $calls[1] | Should Be 'source repair_crg_cache --scope=override_edges --execute'
        $calls[2] | Should Be 'source health --include-counts=true'
        ($calls -join "`n") | Should Not Match '--scope=all'
    }

    It 'collapses a legacy full plus override plan to one full CRG repair' {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson `
                -NextActions @(
                    'source.repair_crg_cache',
                    'source.repair_crg_cache scope=override_edges'
                ) `
                -FullCrgRequired $true `
                -OverrideRequired $true)

        $run = Invoke-IndexFreshnessTest -Execute
        $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

        $run.ExitCode | Should Be 0
        $run.Output | Should Match 'RESULT=REPAIRED'
        $calls.Count | Should Be 3
        $calls[1] | Should Be 'source repair_crg_cache --scope=all --execute'
        ($calls -join "`n") | Should Not Match '--scope=override_edges'
    }

    It 'fails closed instead of widening an override-only requirement to full CRG' {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson `
                -NextActions @('source.repair_crg_cache scope=all') `
                -OverrideRequired $true)

        $run = Invoke-IndexFreshnessTest -Execute
        $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

        $run.ExitCode | Should Be 2
        $run.Output | Should Match 'repair_override_edges_required.*lacks exact next_action'
        $run.Output | Should Match 'no_auto_repair_available=true'
        $calls.Count | Should Be 1
    }

    It 'fails closed for missing, unknown, over-broad, or retired repair actions' {
        $invalidActions = @(
            'source.repair_fts',
            'source.repair_fts target=widgets',
            'source.repair_fts target=all',
            'source.repair_crg_graph'
        )
        foreach ($action in $invalidActions) {
            Remove-Item -LiteralPath $script:QueryLogPath, $script:RepairStatePath -Force -ErrorAction SilentlyContinue
            $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
                New-SourceHealthJson -NextActions @($action))

            $run = Invoke-IndexFreshnessTest -Execute
            $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

            $run.ExitCode | Should Be 2
            $run.Output | Should Match 'REPAIR_REJECTED'
            $run.Output | Should Match 'no_auto_repair_available=true'
            $calls.Count | Should Be 1
            $calls[0] | Should Be 'source health --include-counts=true'
        }
    }

    It 'fails closed when a required graph repair lacks its exact next_action' {
        $env:MONOLITH_TEST_INITIAL_HEALTH_B64 = ConvertTo-TestBase64 (
            New-SourceHealthJson `
                -NextActions @('source.trigger_project_reindex') `
                -SpecificFtsTarget 'graph_nodes')

        $run = Invoke-IndexFreshnessTest -Execute
        $calls = @(Get-Content -LiteralPath $script:QueryLogPath)

        $run.ExitCode | Should Be 2
        $run.Output | Should Match 'repair_graph_nodes_fts_required.*lacks exact next_action'
        $run.Output | Should Match 'no_auto_repair_available=true'
        $calls.Count | Should Be 1
    }
}
