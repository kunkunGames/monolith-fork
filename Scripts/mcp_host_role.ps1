# SPDX-License-Identifier: MIT

function Get-MonolithExecCommandsValue {
    param([string]$CommandLine)

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $null
    }

    $match = [regex]::Match(
        $CommandLine,
        '(?i)(?:^|\s)-ExecCmds\s*=\s*(?:"(?<double>[^"]*)"|''(?<single>[^'']*)''|(?<bare>\S+))')
    if (-not $match.Success) {
        return $null
    }

    foreach ($groupName in @('double', 'single', 'bare')) {
        $group = $match.Groups[$groupName]
        if ($group.Success) {
            return $group.Value
        }
    }
    return $null
}

function Test-MonolithEphemeralAutomationCommandLine {
    param([string]$CommandLine)

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $false
    }

    # -TestExit is an explicit planned-shutdown contract used by editor
    # automation sessions. Such a process may register Monolith actions, but it
    # must never own the durable MCP endpoint.
    if ($CommandLine -match '(?i)(?:^|\s)-TestExit(?:=|\s|$)') {
        return $true
    }

    $execCommands = Get-MonolithExecCommandsValue -CommandLine $CommandLine
    if ([string]::IsNullOrWhiteSpace($execCommands)) {
        return $false
    }

    foreach ($command in @($execCommands -split '[;,]')) {
        $trimmed = ([string]$command).Trim()
        if ($trimmed -match '(?i)^Automation\s+(?:RunTests|RunAll)(?:\s|$)') {
            return $true
        }
    }
    return $false
}

function Test-MonolithCommandLineTargetsProject {
    param([string]$CommandLine, [string]$ProjectFile)

    if ([string]::IsNullOrWhiteSpace($CommandLine) -or [string]::IsNullOrWhiteSpace($ProjectFile)) {
        return $false
    }

    try {
        $normalizedCommandLine = $CommandLine.Replace('/', '\')
        $normalizedProjectFile = ([System.IO.Path]::GetFullPath($ProjectFile)).Replace('/', '\')
        return $normalizedCommandLine.IndexOf($normalizedProjectFile, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    }
    catch {
        return $false
    }
}

function Test-MonolithDurableMcpHostCommandLine {
    param([string]$CommandLine, [string]$ProjectFile)

    if (-not (Test-MonolithCommandLineTargetsProject -CommandLine $CommandLine -ProjectFile $ProjectFile)) {
        return $false
    }
    if ($CommandLine -match '(?i)(^|\s)-(game|server)(\s|$)' -or
        $CommandLine -match '(?i)(^|\s)-run(?:=|\s)' -or
        $CommandLine -match '(?i)bMcpServerEnabled\s*[:=]\s*False' -or
        (Test-MonolithEphemeralAutomationCommandLine -CommandLine $CommandLine)) {
        return $false
    }
    return $true
}

function Get-MonolithEditorProcessCandidates {
    param(
        [string]$ProjectFile,
        [ValidateSet('durable', 'ephemeral_automation')]
        [string]$Role,
        [switch]$AllowUnreadableDurable
    )

    if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
        return @()
    }

    $processes = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) {
        return @()
    }

    $commandLines = @{}
    Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { $commandLines[[int]$_.ProcessId] = [string]$_.CommandLine }

    $matches = @()
    foreach ($process in $processes) {
        $commandLine = $commandLines[[int]$process.Id]
        if ([string]::IsNullOrWhiteSpace($commandLine)) {
            if ($Role -eq 'durable' -and $AllowUnreadableDurable) {
                $matches += $process
            }
            continue
        }

        if ($Role -eq 'durable') {
            if (Test-MonolithDurableMcpHostCommandLine -CommandLine $commandLine -ProjectFile $ProjectFile) {
                $matches += $process
            }
            continue
        }

        if ((Test-MonolithCommandLineTargetsProject -CommandLine $commandLine -ProjectFile $ProjectFile) -and
            (Test-MonolithEphemeralAutomationCommandLine -CommandLine $commandLine)) {
            $matches += $process
        }
    }
    return @($matches)
}
