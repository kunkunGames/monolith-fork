# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

$WatchdogPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'watch_mcp.ps1'
$RecoverPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'recover_mcp.ps1'
$Tokens = $null
$ParseErrors = $null
$Ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $WatchdogPath,
    [ref]$Tokens,
    [ref]$ParseErrors)

if ($ParseErrors.Count -gt 0) {
    throw ($ParseErrors | ForEach-Object { $_.Message } | Out-String)
}

$FunctionAst = $Ast.Find(
    {
        param($Node)
        $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $Node.Name -eq 'Start-WatchdogChildProcess'
    },
    $true)

if (-not $FunctionAst) {
    throw 'Start-WatchdogChildProcess was not found in watch_mcp.ps1'
}

# Load only the pure child-process helper. Dot-sourcing watch_mcp.ps1 would start
# the supervisor loop and is intentionally avoided in this unit test.
Invoke-Expression $FunctionAst.Extent.Text

Describe 'watch_mcp child process exit-code capture' {
    It 'preserves a redirected child exit code in Windows PowerShell 5.1' {
        $StdoutPath = [System.IO.Path]::GetTempFileName()
        $StderrPath = [System.IO.Path]::GetTempFileName()
        try {
            $PowerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $Process = Start-WatchdogChildProcess `
                -FilePath $PowerShellExe `
                -ArgumentList '-NoProfile -Command "exit 7"' `
                -StandardOutputPath $StdoutPath `
                -StandardErrorPath $StderrPath

            $Process.WaitForExit(10000) | Should Be $true
            $Process.ExitCode | Should Be 7
        }
        finally {
            Remove-Item -LiteralPath $StdoutPath, $StderrPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-ScriptFunctionText {
    param(
        [System.Management.Automation.Language.ScriptBlockAst]$ScriptAst,
        [string]$Name
    )

    $match = $ScriptAst.Find(
        {
            param($Node)
            $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $Node.Name -eq $Name
        },
        $true)
    if (-not $match) { throw ("Function {0} was not found" -f $Name) }
    return $match.Extent.Text
}

$RecoverTokens = $null
$RecoverParseErrors = $null
$RecoverAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $RecoverPath,
    [ref]$RecoverTokens,
    [ref]$RecoverParseErrors)
if ($RecoverParseErrors.Count -gt 0) {
    throw ($RecoverParseErrors | ForEach-Object { $_.Message } | Out-String)
}

Describe 'Monolith recovery health contract and project identity' {
    foreach ($case in @(
            @{ Name = 'recover_mcp'; Ast = $RecoverAst },
            @{ Name = 'watch_mcp'; Ast = $Ast }
        )) {
        Context $case.Name {
            It 'accepts only the complete health JSON contract' {
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithJsonNumber')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithHealthContract')

                $valid = [PSCustomObject]@{
                    status = 'ok'
                    port = 9316
                    pid = 1234
                    version = '0.20.3'
                    uptime_seconds = 1.25
                    tools_registered = 1840
                    mcp_transport = [PSCustomObject]@{ primary_route = '/mcp' }
                }
                (Test-MonolithHealthContract -Health $valid -ExpectedPort 9316).Valid | Should Be $true

                $missingTransport = $valid.PSObject.Copy()
                $missingTransport.mcp_transport = $null
                $result = Test-MonolithHealthContract -Health $missingTransport -ExpectedPort 9316
                $result.Valid | Should Be $false
                $result.ErrorCode | Should Be 'primary_route_invalid'

                $wrongPort = $valid.PSObject.Copy()
                $wrongPort.port = 9317
                (Test-MonolithHealthContract -Health $wrongPort -ExpectedPort 9316).ErrorCode | Should Be 'port_mismatch'
            }

            It 'binds the reported PID to this project and rejects game, server, commandlet, and foreign-project modes' {
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Get-ProjectFile')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithEditorCommandLineForProject')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithHealthProcessIdentity')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithHealthListenerIdentity')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithRecoveryPortClear')

                $root = Join-Path $TestDrive $case.Name
                New-Item -ItemType Directory -Path $root -Force | Out-Null
                $projectFile = Join-Path $root 'Speed.uproject'
                Set-Content -LiteralPath $projectFile -Value '{}'
                $health = [PSCustomObject]@{ pid = 1234 }

                $editor = [PSCustomObject]@{
                    Name = 'UnrealEditor.exe'
                    CommandLine = ('"C:\Engine\UnrealEditor.exe" "{0}" -Unattended -NullRHI' -f $projectFile)
                }
                (Test-MonolithHealthProcessIdentity -Health $health -Root $root -ProcessRecord $editor).Valid | Should Be $true

                foreach ($badCommandLine in @(
                        '"C:\Engine\UnrealEditor.exe" "D:\Other\Other.uproject" -NullRHI',
                        ('"C:\Engine\UnrealEditor.exe" "{0}" -game' -f $projectFile),
                        ('"C:\Engine\UnrealEditor.exe" "{0}" -server' -f $projectFile),
                        ('"C:\Engine\UnrealEditor-Cmd.exe" "{0}" -run=MonolithReindex' -f $projectFile)
                    )) {
                    $bad = [PSCustomObject]@{ Name = 'UnrealEditor.exe'; CommandLine = $badCommandLine }
                    (Test-MonolithHealthProcessIdentity -Health $health -Root $root -ProcessRecord $bad).Valid | Should Be $false
                }

                $wrongProcess = [PSCustomObject]@{ Name = 'python.exe'; CommandLine = $editor.CommandLine }
                (Test-MonolithHealthProcessIdentity -Health $health -Root $root -ProcessRecord $wrongProcess).ErrorCode |
                    Should Be 'health_pid_not_unreal_editor'

                $ownedListener = [PSCustomObject]@{ Count = 1; Pids = @(1234) }
                (Test-MonolithHealthListenerIdentity -Health $health -Port 9316 -ListenerSummary $ownedListener).Valid |
                    Should Be $true
                $foreignListener = [PSCustomObject]@{ Count = 1; Pids = @(5678) }
                (Test-MonolithHealthListenerIdentity -Health $health -Port 9316 -ListenerSummary $foreignListener).ErrorCode |
                    Should Be 'health_pid_not_listener_owner'
                $multiOwnerListener = [PSCustomObject]@{ Count = 2; Pids = @(1234, 5678) }
                (Test-MonolithHealthListenerIdentity -Health $health -Port 9316 -ListenerSummary $multiOwnerListener).ErrorCode |
                    Should Be 'health_pid_not_exclusive_listener_owner'
                $unknownListener = [PSCustomObject]@{ Count = -1; Pids = @() }
                (Test-MonolithHealthListenerIdentity -Health $health -Port 9316 -ListenerSummary $unknownListener).ErrorCode |
                    Should Be 'listener_ownership_unavailable'

                (Test-MonolithRecoveryPortClear -ListenerSummary ([PSCustomObject]@{ Count = 0; Pids = @() })).Valid |
                    Should Be $true
                (Test-MonolithRecoveryPortClear -ListenerSummary $foreignListener).ErrorCode |
                    Should Be 'listener_present_without_trusted_health'
                (Test-MonolithRecoveryPortClear -ListenerSummary $unknownListener).ErrorCode |
                    Should Be 'listener_ownership_unavailable'
            }
        }
    }

    It 'does not promote malformed HTTP 200 JSON to healthy' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $RecoverAst -Name 'Get-MonolithHealthProbe')
        function Invoke-WebRequest {
            return [PSCustomObject]@{ StatusCode = 200; Content = 'not-json' }
        }
        try {
            $healthUrl = 'http://localhost:9316/health'
            $probe = Get-MonolithHealthProbe
            $probe.Health | Should Be $null
            $probe.ErrorClass | Should Be 'invalid_json'
        }
        finally {
            Remove-Item Function:\Invoke-WebRequest -ErrorAction SilentlyContinue
        }
    }
}

Describe 'watch_mcp fail-closed mutation gates' {
    It 'does not start a build when a non-health listener owns the MCP port' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Invoke-RestartSequence')

        $script:restartAttempts = 0
        $script:buildCalled = $false
        $MaxRestartAttempts = 0
        $NoBuildBeforeRestart = $false

        function Write-Watchdog { param([string]$Message) }
        function Get-McpHealthPort { return 9316 }
        function Get-MonolithRecoveryPortGate {
            param([int]$Port)
            return [PSCustomObject]@{
                Valid = $false
                ErrorCode = 'listener_present_without_trusted_health'
                Count = 1
                Pids = @(5678)
            }
        }
        function Invoke-EditorBuild { param([string]$Root); $script:buildCalled = $true; return $true }

        $result = Invoke-RestartSequence -Root 'D:\P4\speed' -Reason 'test_foreign_listener'
        $result.ExitCode | Should Be 3
        $result.Result | Should Be 'RESULT=BLOCKED'
        $script:restartAttempts | Should Be 0
        $script:buildCalled | Should Be $false
    }

    It 'surfaces unexpected DLL write-probe exceptions and skips UBT' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Get-BuildBlockingLocks')
        function Get-BuildLockCandidateFiles {
            param([string]$Root)
            return ,@([PSCustomObject]@{ FullName = ''; IsReadOnly = $false })
        }

        $probe = Get-BuildBlockingLocks -Root 'D:\P4\speed'
        $probe.Locked.Count | Should Be 0
        $probe.ReadOnly.Count | Should Be 0
        $probe.ProbeErrors.Count | Should Be 1
        [string]::IsNullOrWhiteSpace([string]$probe.ProbeErrors[0].ExceptionType) | Should Be $false

        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Invoke-RestartSequence')
        $script:restartAttempts = 0
        $script:consecutiveBuildFailures = 0
        $script:buildCalled = $false
        $MaxRestartAttempts = 0
        $NoBuildBeforeRestart = $false

        function Write-Watchdog { param([string]$Message) }
        function Get-McpHealthPort { return 9316 }
        function Get-MonolithRecoveryPortGate {
            param([int]$Port)
            return [PSCustomObject]@{ Valid = $true; ErrorCode = $null; Count = 0; Pids = @() }
        }
        function Get-BuildBlockingLocks {
            param([string]$Root)
            return [PSCustomObject]@{
                Locked = @()
                ReadOnly = @()
                ProbeErrors = @([PSCustomObject]@{
                        Path = 'bad-output.dll'
                        ExceptionType = 'System.InvalidOperationException'
                        Message = 'synthetic probe failure'
                    })
            }
        }
        function Get-BuildFailureBackoffSeconds { return 15 }
        function Format-BuildProbeErrors { param($ProbeErrors); return 'bounded-probe-error' }
        function Invoke-EditorBuild { param([string]$Root); $script:buildCalled = $true; return $true }

        $result = Invoke-RestartSequence -Root 'D:\P4\speed' -Reason 'test_probe_error'
        $result.ExitCode | Should Be 11
        $result.Result | Should Be 'RESULT=BUILD_LOCK_PROBE_FAILED'
        $script:buildCalled | Should Be $false
    }
}

Describe 'watch_mcp recovery availability ordering' {
    It 'recovers the endpoint and retries maintenance after a pre-restart indexing failure' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Invoke-RestartSequence')

        $script:restartAttempts = 0
        $script:recoverCalled = $false
        $script:postRecoverRetryAll = $false
        $MaxRestartAttempts = 0
        $NoBuildBeforeRestart = $true

        function Write-Watchdog { param([string]$Message) }
        function Get-McpHealthPort { return 9316 }
        function Get-MonolithRecoveryPortGate {
            param([int]$Port)
            return [PSCustomObject]@{ Valid = $true; ErrorCode = $null; Count = 0; Pids = @() }
        }
        function Invoke-PreRestartReindex { param([string]$Root); return $false }
        function Invoke-Recover {
            param([string]$Root, [switch]$ForceLaunch)
            $script:recoverCalled = [bool]$ForceLaunch
            return [PSCustomObject]@{ ExitCode = 0; Result = 'RESULT=MCP_UP' }
        }
        function Invoke-PostRecoverReindex {
            param([string]$Root, [switch]$RetryPreRestartTargets)
            $script:postRecoverRetryAll = [bool]$RetryPreRestartTargets
            return $false
        }

        $result = Invoke-RestartSequence -Root 'D:\P4\speed' -Reason 'test'
        $result.ExitCode | Should Be 0
        $result.MaintenanceSucceeded | Should Be $false
        $script:recoverCalled | Should Be $true
        $script:postRecoverRetryAll | Should Be $true
    }
}
