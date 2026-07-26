# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

$WatchdogPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'watch_mcp.ps1'
$RecoverPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'recover_mcp.ps1'
$HostRolePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'mcp_host_role.ps1'
if (-not (Test-Path -LiteralPath $HostRolePath -PathType Leaf)) {
    throw ("mcp_host_role.ps1 was not found at {0}" -f $HostRolePath)
}
. $HostRolePath
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

Describe 'recover_mcp headless wrapper argument contract' {
    It 'uses UE_EDITOR_EXTRA_ARGS for the batch boundary and restores the caller environment' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $RecoverAst -Name 'Start-MonolithHeadlessEditorWrapper')

        $previousExtraArgs = [Environment]::GetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', 'Process')
        $script:capturedExtraArgs = $null
        $script:capturedArgumentList = 'not-called'
        function Start-Process {
            [CmdletBinding()]
            param(
                [string]$FilePath,
                [string]$WorkingDirectory,
                [object]$ArgumentList,
                [System.Diagnostics.ProcessWindowStyle]$WindowStyle,
                [switch]$PassThru
            )
            $script:capturedExtraArgs = [Environment]::GetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', 'Process')
            $script:capturedArgumentList = $ArgumentList
            return [PSCustomObject]@{ Id = 1234 }
        }

        try {
            [Environment]::SetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', '-CallerFlag=Preserved', 'Process')
            $process = Start-MonolithHeadlessEditorWrapper `
                -Wrapper 'D:\Project Root\Build\BatchFiles\RunHeadlessEditor.bat' `
                -WorkingDirectory 'D:\Project Root' `
                -EditorArguments @(
                    '-EditorLayoutINI="D:\Project Root\Saved\HeadlessMcp\Config\WindowsEditor\EditorLayout.ini"',
                    '-ini:EditorPerProjectUserSettings:[/Script/UnrealEd.EditorLoadingSavingSettings]:RestoreOpenAssetTabsOnRestart=NeverRestore'
                )

            $process.Id | Should Be 1234
            $script:capturedExtraArgs | Should Match '^-CallerFlag=Preserved '
            $script:capturedExtraArgs | Should Match '-EditorLayoutINI="D:\\Project Root\\Saved\\HeadlessMcp'
            $script:capturedExtraArgs | Should Match 'RestoreOpenAssetTabsOnRestart=NeverRestore$'
            $script:capturedArgumentList | Should Be $null
            [Environment]::GetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', 'Process') |
                Should Be '-CallerFlag=Preserved'
        }
        finally {
            [Environment]::SetEnvironmentVariable('UE_EDITOR_EXTRA_ARGS', $previousExtraArgs, 'Process')
            Remove-Item Function:\Start-Process, Function:\Start-MonolithHeadlessEditorWrapper -ErrorAction SilentlyContinue
        }
    }
}

Describe 'Monolith health request timeout contract' {
    foreach ($case in @(
            @{ Name = 'recover_mcp'; Ast = $RecoverAst; HealthFunction = 'Get-MonolithHealthProbe'; ScriptPath = $RecoverPath },
            @{ Name = 'watch_mcp'; Ast = $Ast; HealthFunction = 'Get-MonolithHealth'; ScriptPath = $WatchdogPath }
        )) {
        Context $case.Name {
            It 'declares a validated five-second health timeout and uses it only for the health request' {
                $scriptText = Get-Content -LiteralPath $case.ScriptPath -Raw
                $scriptText | Should Match '\[ValidateRange\(1,\s*60\)\]\s*\[int\]\$HealthTimeoutSec\s*=\s*5'
                $healthFunctionText = Get-ScriptFunctionText -ScriptAst $case.Ast -Name $case.HealthFunction
                $healthFunctionText | Should Match '-TimeoutSec\s+\$HealthTimeoutSec'
            }

            It 'accepts a valid health response delayed beyond the historical three-second timeout' {
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name $case.HealthFunction)

                function Get-McpHealthPort { return 9316 }
                function Test-MonolithHealthContract {
                    param($Health, [int]$ExpectedPort)
                    return [PSCustomObject]@{ Valid = ($Health.port -eq $ExpectedPort); ErrorCode = $null }
                }
                function Test-MonolithHealthProcessIdentity {
                    param($Health, [string]$Root)
                    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
                }
                function Test-MonolithHealthListenerIdentity {
                    param($Health, [int]$Port, $ListenerSummary)
                    return [PSCustomObject]@{ Valid = $true; ErrorCode = $null }
                }

                $script:capturedHealthTimeoutSec = $null
                function Invoke-WebRequest {
                    [CmdletBinding()]
                    param(
                        [string]$Uri,
                        [string]$Method,
                        [int]$TimeoutSec,
                        [switch]$UseBasicParsing
                    )
                    $script:capturedHealthTimeoutSec = $TimeoutSec
                    Start-Sleep -Milliseconds 3200
                    $content = [ordered]@{
                        status = 'ok'
                        port = 9316
                        pid = 1234
                        version = '0.20.3'
                        uptime_seconds = 1.25
                        tools_registered = 1840
                        mcp_transport = [ordered]@{ primary_route = '/mcp' }
                    } | ConvertTo-Json -Compress
                    return [PSCustomObject]@{ StatusCode = 200; Content = $content }
                }

                try {
                    $healthUrl = 'http://localhost:9316/health'
                    $HealthTimeoutSec = 5
                    $script:hostRoot = $TestDrive
                    $health = if ($case.HealthFunction -eq 'Get-MonolithHealthProbe') {
                        (Get-MonolithHealthProbe).Health
                    }
                    else {
                        Get-MonolithHealth
                    }
                    $script:capturedHealthTimeoutSec | Should Be 5
                    $health.status | Should Be 'ok'
                }
                finally {
                    Remove-Item Function:\Invoke-WebRequest,
                        Function:\Get-McpHealthPort,
                        Function:\Test-MonolithHealthContract,
                        Function:\Test-MonolithHealthProcessIdentity,
                        Function:\Test-MonolithHealthListenerIdentity -ErrorAction SilentlyContinue
                }
            }
        }
    }

    It 'forwards the watchdog health timeout to the recover child' {
        $invokeRecoverText = Get-ScriptFunctionText -ScriptAst $Ast -Name 'Invoke-Recover'
        $invokeRecoverText | Should Match "'-HealthTimeoutSec',\s*\r?\n\s*\`$HealthTimeoutSec"
    }
}

Describe 'Monolith trusted-busy endpoint state' {
    foreach ($case in @(
            @{ Name = 'recover_mcp'; Ast = $RecoverAst; ErrorClass = 'timeout' },
            @{ Name = 'watch_mcp'; Ast = $Ast; ErrorClass = 'request_failed' }
        )) {
        Context $case.Name {
            It 'accepts only one live exact-project editor listener as trusted-busy' {
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Get-ProjectFile')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithEditorCommandLineForProject')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithHealthProcessIdentity')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Test-MonolithTrustedBusyListener')
                Invoke-Expression (Get-ScriptFunctionText -ScriptAst $case.Ast -Name 'Get-MonolithUnavailableEndpointState')

                $root = Join-Path $TestDrive ("trusted_busy_{0}" -f $case.Name)
                New-Item -ItemType Directory -Path $root -Force | Out-Null
                $projectFile = Join-Path $root 'Speed.uproject'
                Set-Content -LiteralPath $projectFile -Value '{}'
                $validEditor = [PSCustomObject]@{
                    ProcessId = 1234
                    Name = 'UnrealEditor.exe'
                    CommandLine = ('"C:\Engine\UnrealEditor.exe" "{0}" -Unattended -NullRHI' -f $projectFile)
                }
                $ownedListener = [PSCustomObject]@{
                    Valid = $false
                    ErrorCode = 'listener_present_without_trusted_health'
                    Count = 2
                    Pids = @(1234)
                }

                $trusted = Get-MonolithUnavailableEndpointState `
                    -HealthErrorClass $case.ErrorClass `
                    -HealthStatusCode $null `
                    -Root $root `
                    -PortGate $ownedListener `
                    -ProcessRecord $validEditor
                $trusted.State | Should Be 'trusted_busy'
                $trusted.Pid | Should Be 1234

                $invalidHttp = Get-MonolithUnavailableEndpointState `
                    -HealthErrorClass 'health_contract' `
                    -HealthStatusCode 200 `
                    -Root $root `
                    -PortGate $ownedListener `
                    -ProcessRecord $validEditor
                $invalidHttp.State | Should Be 'blocked'

                $ephemeralEditor = [PSCustomObject]@{
                    ProcessId = 1234
                    Name = 'UnrealEditor.exe'
                    CommandLine = ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Automation RunTests X; Quit"' -f $projectFile)
                }
                $ephemeral = Get-MonolithUnavailableEndpointState `
                    -HealthErrorClass 'health_identity' `
                    -HealthErrorCode 'health_pid_ephemeral_automation' `
                    -HealthStatusCode 200 `
                    -HealthPid 1234 `
                    -Root $root `
                    -PortGate $ownedListener `
                    -ProcessRecord $ephemeralEditor
                $ephemeral.State | Should Be 'ephemeral_automation'
                $ephemeral.Pid | Should Be 1234

                (Get-MonolithUnavailableEndpointState `
                        -HealthErrorClass 'health_identity' `
                        -HealthErrorCode 'health_pid_ephemeral_automation' `
                        -HealthStatusCode 200 `
                        -HealthPid 9999 `
                        -Root $root `
                        -PortGate $ownedListener `
                        -ProcessRecord $ephemeralEditor).State | Should Be 'blocked'

                foreach ($badEditor in @(
                        [PSCustomObject]@{ ProcessId = 1234; Name = 'python.exe'; CommandLine = $validEditor.CommandLine },
                        [PSCustomObject]@{ ProcessId = 1234; Name = 'UnrealEditor.exe'; CommandLine = '"C:\Engine\UnrealEditor.exe" "D:\Other\Other.uproject" -NullRHI' },
                        [PSCustomObject]@{ ProcessId = 1234; Name = 'UnrealEditor-Cmd.exe'; CommandLine = ('"C:\Engine\UnrealEditor-Cmd.exe" "{0}" -run=MonolithReindex' -f $projectFile) },
                        [PSCustomObject]@{ ProcessId = 9999; Name = 'UnrealEditor.exe'; CommandLine = $validEditor.CommandLine },
                        [PSCustomObject]@{ ProcessId = 1234; Name = 'UnrealEditor.exe'; CommandLine = $null }
                    )) {
                    $rejected = Get-MonolithUnavailableEndpointState `
                        -HealthErrorClass $case.ErrorClass `
                        -HealthStatusCode $null `
                        -Root $root `
                        -PortGate $ownedListener `
                        -ProcessRecord $badEditor
                    $rejected.State | Should Be 'blocked'
                }

                $multiOwner = [PSCustomObject]@{
                    Valid = $false
                    ErrorCode = 'listener_present_without_trusted_health'
                    Count = 2
                    Pids = @(1234, 5678)
                }
                (Get-MonolithUnavailableEndpointState `
                        -HealthErrorClass $case.ErrorClass `
                        -HealthStatusCode $null `
                        -Root $root `
                        -PortGate $multiOwner `
                        -ProcessRecord $validEditor).State | Should Be 'blocked'

                $clearPort = [PSCustomObject]@{ Valid = $true; ErrorCode = $null; Count = 0; Pids = @() }
                (Get-MonolithUnavailableEndpointState `
                        -HealthErrorClass $case.ErrorClass `
                        -HealthStatusCode $null `
                        -Root $root `
                        -PortGate $clearPort `
                        -ProcessRecord $validEditor).State | Should Be 'down'
            }
        }
    }

    It 'classifies a post-index health request that exceeds five seconds as trusted-busy' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Get-MonolithHealth')
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Get-ProjectFile')
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Test-MonolithEditorCommandLineForProject')
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Test-MonolithHealthProcessIdentity')
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Test-MonolithTrustedBusyListener')
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Get-MonolithUnavailableEndpointState')

        $script:capturedHealthTimeoutSec = $null
        function Invoke-WebRequest {
            [CmdletBinding()]
            param(
                [string]$Uri,
                [string]$Method,
                [int]$TimeoutSec,
                [switch]$UseBasicParsing
            )
            $script:capturedHealthTimeoutSec = $TimeoutSec
            Start-Sleep -Milliseconds 5200
            throw [System.TimeoutException]::new('synthetic post-index health timeout')
        }

        try {
            $healthUrl = 'http://localhost:9316/health'
            $HealthTimeoutSec = 5
            $root = Join-Path $TestDrive 'post_index_timeout'
            New-Item -ItemType Directory -Path $root -Force | Out-Null
            $projectFile = Join-Path $root 'Speed.uproject'
            Set-Content -LiteralPath $projectFile -Value '{}'
            $script:hostRoot = $root
            $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
            (Get-MonolithHealth) | Should Be $null
            $elapsed.Stop()
            $elapsed.Elapsed.TotalSeconds -ge 5 | Should Be $true
            $script:capturedHealthTimeoutSec | Should Be 5
            $script:lastHealthErrorClass | Should Be 'request_failed'

            $editor = [PSCustomObject]@{
                ProcessId = 1234
                Name = 'UnrealEditor.exe'
                CommandLine = ('"C:\Engine\UnrealEditor.exe" "{0}" -Unattended -NullRHI' -f $projectFile)
            }
            $portGate = [PSCustomObject]@{
                Valid = $false
                ErrorCode = 'listener_present_without_trusted_health'
                Count = 1
                Pids = @(1234)
            }
            (Get-MonolithUnavailableEndpointState `
                    -HealthErrorClass $script:lastHealthErrorClass `
                    -HealthStatusCode $script:lastHealthStatusCode `
                    -Root $root `
                    -PortGate $portGate `
                    -ProcessRecord $editor).State | Should Be 'trusted_busy'
        }
        finally {
            Remove-Item Function:\Invoke-WebRequest -ErrorAction SilentlyContinue
        }
    }

    It 'bounds trusted-busy retry backoff' {
        Invoke-Expression (Get-ScriptFunctionText -ScriptAst $Ast -Name 'Get-TrustedBusyBackoffSeconds')
        $PollIntervalSec = 15
        $TrustedBusyBackoffMaxSec = 60
        (Get-TrustedBusyBackoffSeconds -ConsecutiveCount 1) | Should Be 15
        (Get-TrustedBusyBackoffSeconds -ConsecutiveCount 2) | Should Be 30
        (Get-TrustedBusyBackoffSeconds -ConsecutiveCount 3) | Should Be 60
        (Get-TrustedBusyBackoffSeconds -ConsecutiveCount 20) | Should Be 60
    }

    It 'keeps the supervisor alive without mutation and makes recovery wait without launching' {
        $watchText = Get-Content -LiteralPath $WatchdogPath -Raw
        $busyStart = $watchText.IndexOf("if (`$endpointState.State -eq 'trusted_busy')", [System.StringComparison]::Ordinal)
        $blockedStart = $watchText.IndexOf("if (`$endpointState.State -eq 'blocked')", $busyStart, [System.StringComparison]::Ordinal)
        $busyStart -ge 0 | Should Be $true
        $blockedStart -gt $busyStart | Should Be $true
        $watchBusyBranch = $watchText.Substring($busyStart, $blockedStart - $busyStart)
        $watchBusyBranch | Should Match 'Start-Sleep\s+-Seconds\s+\$backoffSec'
        $watchBusyBranch | Should Match '\bcontinue\b'
        $watchBusyBranch | Should Not Match 'Invoke-RestartSequence|Invoke-Recover|Stop-HeadlessEditors|Invoke-EditorBuild'
        $watchText | Should Match '(?s)if \(\$script:consecutiveTrustedBusy -gt 0\).*trusted_editor_busy_reset.*\$script:consecutiveTrustedBusy = 0'

        $recoverText = Get-Content -LiteralPath $RecoverPath -Raw
        $recoverText | Should Match '(?s)if \(\$endpointState.State -eq ''trusted_busy''\).*RESULT=MCP_BUSY.*exit 2'
        $recoverText | Should Match '(?s)if \(\$endpointState.State -eq ''trusted_busy''\) \{\s*Write-Output \("INFO trusted_editor_process.*skipping_launch=true'

        $watchText | Should Match '(?s)EPHEMERAL_AUTOMATION_ACTIVE.*mutation=none.*Start-Sleep\s+-Seconds\s+\$PollIntervalSec.*continue'
        $recoverText | Should Match '(?s)MCP_EPHEMERAL_AUTOMATION.*mutation=none.*waiting_for_ephemeral_automation_exit'
    }
}

Describe 'Monolith durable MCP host role contract' {
    It 'is sourced by both recovery entry points' {
        (Get-Content -LiteralPath $RecoverPath -Raw) | Should Match "Join-Path\s+\`$PSScriptRoot\s+'mcp_host_role\.ps1'"
        (Get-Content -LiteralPath $WatchdogPath -Raw) | Should Match "Join-Path\s+\`$PSScriptRoot\s+'mcp_host_role\.ps1'"
    }

    It 'accepts persistent editor roles and rejects planned-exit automation roles' {
        $root = Join-Path $TestDrive 'host_role'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $projectFile = Join-Path $root 'Speed.uproject'
        Set-Content -LiteralPath $projectFile -Value '{}'

        foreach ($accepted in @(
                ('"C:\Engine\UnrealEditor.exe" "{0}"' -f $projectFile),
                ('"C:\Engine\UnrealEditor.exe" "{0}" -Unattended -NullRHI -AbsLog="{1}\Saved\HeadlessMcp\Headless.log"' -f $projectFile, $root),
                ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Log LogTemp Warning Automation RunTests is deferred"' -f $projectFile)
            )) {
            Test-MonolithDurableMcpHostCommandLine -CommandLine $accepted -ProjectFile $projectFile |
                Should Be $true
        }

        foreach ($rejected in @(
                ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Automation RunTests Speed.PCG; Quit"' -f $projectFile),
                ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Automation RunAll; Quit"' -f $projectFile),
                ('"C:\Engine\UnrealEditor.exe" "{0}" -TestExit="Automation Test Queue Empty"' -f $projectFile),
                ('"C:\Engine\UnrealEditor-Cmd.exe" "{0}" -run=MonolithReindex' -f $projectFile),
                ('"C:\Engine\UnrealEditor.exe" "{0}" -game' -f $projectFile)
            )) {
            Test-MonolithDurableMcpHostCommandLine -CommandLine $rejected -ProjectFile $projectFile |
                Should Be $false
        }
    }
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

            It 'binds the reported PID to this project and rejects game, server, commandlet, planned-exit automation, and foreign-project modes' {
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
                        ('"C:\Engine\UnrealEditor-Cmd.exe" "{0}" -run=MonolithReindex' -f $projectFile),
                        ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Automation RunTests Speed.PCG; Quit"' -f $projectFile),
                        ('"C:\Engine\UnrealEditor.exe" "{0}" -TestExit="Automation Test Queue Empty"' -f $projectFile)
                    )) {
                    $bad = [PSCustomObject]@{ Name = 'UnrealEditor.exe'; CommandLine = $badCommandLine }
                    (Test-MonolithHealthProcessIdentity -Health $health -Root $root -ProcessRecord $bad).Valid | Should Be $false
                }

                $ephemeral = [PSCustomObject]@{
                    Name = 'UnrealEditor.exe'
                    CommandLine = ('"C:\Engine\UnrealEditor.exe" "{0}" -ExecCmds="Automation RunTests Speed.PCG; Quit"' -f $projectFile)
                }
                (Test-MonolithHealthProcessIdentity -Health $health -Root $root -ProcessRecord $ephemeral).ErrorCode |
                    Should Be 'health_pid_ephemeral_automation'

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
        function Get-MonolithActivationState {
            param([string]$Root)
            return [PSCustomObject]@{
                ServerEnabled = $true
                IndexingEnabled = $true
                StatePath = 'D:\P4\speed\Saved\Config\WindowsEditor\Monolith.ini'
            }
        }
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
        function Get-MonolithActivationState {
            param([string]$Root)
            return [PSCustomObject]@{
                ServerEnabled = $true
                IndexingEnabled = $true
                StatePath = 'D:\P4\speed\Saved\Config\WindowsEditor\Monolith.ini'
            }
        }
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
        function Get-MonolithActivationState {
            param([string]$Root)
            return [PSCustomObject]@{
                ServerEnabled = $true
                IndexingEnabled = $true
                StatePath = 'D:\P4\speed\Saved\Config\WindowsEditor\Monolith.ini'
            }
        }
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

Describe 'watch_mcp graph export retirement contract' {
    It 'keeps daily and restart maintenance targets bounded to assets and source' {
        foreach ($parameterName in @('DailyReindexTargets', 'RestartReindexTargets')) {
            $parameter = $Ast.ParamBlock.Parameters | Where-Object {
                $_.Name.VariablePath.UserPath -eq $parameterName
            }
            $parameter | Should Not BeNullOrEmpty

            $validateSet = $parameter.Attributes | Where-Object {
                $_.TypeName.Name -eq 'ValidateSet'
            }
            $validateSet | Should Not BeNullOrEmpty
            $allowed = @($validateSet.PositionalArguments | ForEach-Object {
                    [string]$_.SafeGetValue()
                })
            ($allowed -join ',') | Should Be 'assets,source'

            $defaultText = $parameter.DefaultValue.Extent.Text
            $defaultText | Should Match "'assets'"
            $defaultText | Should Match "'source'"
            $defaultText | Should Not Match "'graph'"
        }
    }

    It 'contains no retired graph builder, target, or cooldown path' {
        $retiredParameters = @($Ast.ParamBlock.Parameters | Where-Object {
                $_.Name.VariablePath.UserPath -eq 'DailyGraphCooldownSeconds'
            })
        $retiredParameters.Count | Should Be 0

        $retiredFunctions = @($Ast.FindAll({
                    param($node)
                    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                    $node.Name -eq 'Invoke-GraphIndex'
                }, $true))
        $retiredFunctions.Count | Should Be 0

        $scriptText = [System.IO.File]::ReadAllText($WatchdogPath)
        foreach ($retiredToken in @(
                'build_crg_graph',
                "Test-IndexTarget -Targets `$Targets -Target 'graph'",
                "Test-IndexTarget -Targets `$RestartReindexTargets -Target 'graph'"
            )) {
            $scriptText.Contains($retiredToken) | Should Be $false
        }
    }

    It 'keeps explicit editor-down source maintenance on the indexer-owned scoped CRG path' {
        $pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        $projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
        $postBuildPath = Join-Path $projectRoot 'Build\BatchFiles\PostBuildSourceIndex.bat'
        Test-Path -LiteralPath $postBuildPath | Should Be $true

        $postBuildText = [System.IO.File]::ReadAllText($postBuildPath)
        $postBuildText.Contains('MonolithReindex') | Should Be $true
        $postBuildText.Contains('repair_crg_cache') | Should Be $false
        $postBuildText.Contains('build_crg_graph') | Should Be $false
        $postBuildText.Contains('MONOLITH_SKIP_CRG') | Should Be $false

        $targetPath = Join-Path $projectRoot 'Source\SpeedEditor.Target.cs'
        Test-Path -LiteralPath $targetPath | Should Be $true
        $targetText = [System.IO.File]::ReadAllText($targetPath)
        $targetText.Contains('PostBuildSourceIndex.bat') | Should Be $false
    }
}
