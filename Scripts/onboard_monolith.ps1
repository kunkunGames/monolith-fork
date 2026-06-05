param(
    [string[]]$Targets = @('Codex', 'Claude'),
    [switch]$Plan,
    [switch]$Execute,
    [switch]$ReplaceCopies,
    [switch]$ReplaceMcpConfig,
    [switch]$UseSymlink,
    [ValidateSet('Http', 'Proxy')]
    [string]$McpMode = 'Http',
    [string]$ProjectRoot = '',
    [string]$InstructionRoot = '',
    [string]$McpConfigPath = '',
    [switch]$ProjectMcpConfig,
    [switch]$SkipSkills,
    [switch]$SkipMcpConfig,
    [switch]$SkipInstructions
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Plan -and $Execute) {
    Write-Error 'Use either -Plan or -Execute, not both.'
}

if ($env:OS -ne 'Windows_NT') {
    Write-Error 'Monolith onboarding automation is Windows-only. Use Templates\Onboarding\Onboarding.md for manual setup elsewhere.'
}

$repoRoot = (Split-Path -Parent $PSScriptRoot)
$templatesRoot = Join-Path $repoRoot 'Templates'
$adapterRoot = Join-Path $templatesRoot 'Onboarding'
$issues = New-Object System.Collections.Generic.List[string]

function Add-Issue {
    param([string]$Message)
    $script:issues.Add($Message) | Out-Null
    Write-Host "CONFLICT $Message"
}

function Resolve-MonolithPath {
    param(
        [string]$Path,
        [string]$BasePath
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }
    $expanded = [System.Environment]::ExpandEnvironmentVariables($Path)
    if ($expanded.StartsWith('~')) {
        $expanded = Join-Path $HOME $expanded.Substring(1).TrimStart('\', '/')
    }
    if ([System.IO.Path]::IsPathRooted($expanded)) {
        return [System.IO.Path]::GetFullPath($expanded)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $expanded))
}

function Get-BoolProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $false }
    return [bool]$prop.Value
}

function Get-StringProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop -or $null -eq $prop.Value) { return '' }
    return [string]$prop.Value
}

function Get-StringArrayProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop -or $null -eq $prop.Value) { return @() }
    if ($prop.Value -is [array]) { return @($prop.Value | ForEach-Object { [string]$_ }) }
    return @([string]$prop.Value)
}

function Read-OnboardingAdapters {
    if (-not (Test-Path -LiteralPath $adapterRoot -PathType Container)) {
        Write-Error "Onboarding adapter directory does not exist: $adapterRoot"
    }

    $adapters = @{}
    foreach ($file in Get-ChildItem -LiteralPath $adapterRoot -Filter '*.json' -File | Sort-Object Name) {
        $adapter = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        $id = Get-StringProperty -Object $adapter -Name 'id'
        if ([string]::IsNullOrWhiteSpace($id)) {
            Write-Error "$($file.FullName): adapter id is required."
        }
        $adapters[$id] = $adapter
    }

    if ($adapters.Count -eq 0) {
        Write-Error "No onboarding adapters found under $adapterRoot"
    }
    return $adapters
}

function Normalize-Targets {
    param([hashtable]$Adapters)

    $values = New-Object System.Collections.Generic.List[string]
    foreach ($targetArg in $Targets) {
        foreach ($part in ([string]$targetArg -split ',')) {
            $value = $part.Trim()
            if ([string]::IsNullOrWhiteSpace($value)) { continue }
            if ($value -eq 'All') {
                foreach ($key in ($Adapters.Keys | Sort-Object)) {
                    if (-not $values.Contains($key)) {
                        $values.Add($key) | Out-Null
                    }
                }
                continue
            }

            $matched = $null
            foreach ($key in $Adapters.Keys) {
                if ($key.Equals($value, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $matched = $key
                    break
                }
            }
            if ($null -eq $matched) {
                $supported = (($Adapters.Keys + @('All')) | Sort-Object) -join ', '
                Write-Error "Unsupported onboarding target '$value'. Expected one of: $supported."
            }
            if (-not $values.Contains($matched)) {
                $values.Add($matched) | Out-Null
            }
        }
    }
    if ($values.Count -eq 0) {
        Write-Error 'No onboarding targets were selected.'
    }
    return @($values)
}

function Get-DefaultProjectRoot {
    $pluginsRoot = Split-Path -Parent $repoRoot
    if ((Split-Path -Leaf $pluginsRoot) -eq 'Plugins') {
        return (Split-Path -Parent $pluginsRoot)
    }
    return $repoRoot
}

function Get-RelativePathText {
    param(
        [string]$FromDirectory,
        [string]$ToPath
    )

    try {
        return ([System.IO.Path]::GetRelativePath($FromDirectory, $ToPath)).Replace('\', '/')
    }
    catch {
        return $ToPath
    }
}

function Invoke-SkillValidation {
    $validator = Join-Path $repoRoot 'Scripts\validate_monolith_skills.ps1'
    Write-Host "VALIDATE repository skills"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $validator
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Skill validation failed with exit code $LASTEXITCODE."
    }
}

function Invoke-SkillInstall {
    param(
        [pscustomobject[]]$SelectedAdapters
    )

    $skillAdapters = @($SelectedAdapters | Where-Object { Get-BoolProperty -Object $_ -Name 'supportsSkills' })
    if ($skillAdapters.Count -eq 0) {
        Write-Host 'SKIP skill links: selected targets do not support skills.'
        return
    }

    $targetNames = New-Object System.Collections.Generic.List[string]
    $targetSpecs = New-Object System.Collections.Generic.List[string]
    foreach ($adapter in $skillAdapters) {
        $id = Get-StringProperty -Object $adapter -Name 'id'
        $root = Get-StringProperty -Object $adapter -Name 'skillRoot'
        if ([string]::IsNullOrWhiteSpace($root)) {
            Add-Issue "$id supports skills but has no skillRoot."
            continue
        }
        $expandedRoot = Resolve-MonolithPath -Path $root -BasePath $repoRoot
        $targetNames.Add($id) | Out-Null
        $targetSpecs.Add("$id=$expandedRoot") | Out-Null
    }

    if ($targetNames.Count -eq 0) { return }

    $installer = Join-Path $repoRoot 'Scripts\install_monolith_skills.ps1'
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $installer,
        '-Targets', ($targetNames -join ','),
        '-SourceRoot', (Join-Path $repoRoot 'Skills'),
        '-TargetRootSpecs'
    )
    $args += @($targetSpecs)
    if ($Execute) { $args += '-Execute' }
    if ($ReplaceCopies) { $args += '-ReplaceCopies' }
    if ($UseSymlink) { $args += '-UseSymlink' }

    Write-Host "SKILLS $($targetNames -join ',')"
    & powershell @args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Skill installation step failed with exit code $LASTEXITCODE."
    }
}

function Get-McpServerName {
    param([pscustomobject]$Adapter)

    $name = Get-StringProperty -Object $Adapter -Name 'mcpServerName'
    if ([string]::IsNullOrWhiteSpace($name)) { return 'monolith' }
    return $name
}

function Get-McpUrl {
    param([pscustomobject]$Adapter)

    $url = Get-StringProperty -Object $Adapter -Name 'mcpUrl'
    if ([string]::IsNullOrWhiteSpace($url)) { return 'http://localhost:9316/mcp' }
    return $url
}

function Get-McpScope {
    param([pscustomobject]$Adapter)

    $scope = Get-StringProperty -Object $Adapter -Name 'mcpScope'
    if ([string]::IsNullOrWhiteSpace($scope)) { return 'user' }
    return $scope
}

function Get-McpProxyCommand {
    return (Join-Path $repoRoot 'Binaries\monolith_proxy.exe')
}

function Test-McpOutputMatches {
    param(
        [string]$Output,
        [pscustomobject]$Adapter
    )

    if ($McpMode -eq 'Http') {
        return ($Output -match [regex]::Escape((Get-McpUrl -Adapter $Adapter)))
    }

    return ($Output -match [regex]::Escape((Get-McpProxyCommand)))
}

function Format-CommandLine {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    $parts = @($Command) + @($Arguments | ForEach-Object {
        if ($_ -match '\s') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    })
    return ($parts -join ' ')
}

function Invoke-OnboardingCommand {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    $display = Format-CommandLine -Command $Command -Arguments $Arguments
    if (-not $Execute) {
        Write-Host "DRYRUN $display"
        return
    }

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Command failed with exit code $LASTEXITCODE`: $display"
    }
}

function Install-CodexMcpConfig {
    param([pscustomobject]$Adapter)

    if ($null -eq (Get-Command codex -ErrorAction SilentlyContinue)) {
        Add-Issue 'Codex MCP config requested, but codex CLI is not available.'
        return
    }

    $serverName = Get-McpServerName -Adapter $Adapter
    $getOutput = & codex mcp get $serverName 2>&1
    $exists = ($LASTEXITCODE -eq 0)
    $outputText = ($getOutput | Out-String)

    if ($exists -and (Test-McpOutputMatches -Output $outputText -Adapter $Adapter)) {
        Write-Host "SKIP Codex MCP config already matches '$serverName'."
        return
    }

    $addArgs = if ($McpMode -eq 'Http') {
        @('mcp', 'add', $serverName, '--url', (Get-McpUrl -Adapter $Adapter))
    }
    else {
        @('mcp', 'add', $serverName, '--', (Get-McpProxyCommand))
    }

    if ($exists) {
        $message = "Codex MCP config '$serverName' exists and differs. Use -ReplaceMcpConfig to replace it through codex mcp remove/add."
        if (-not $ReplaceMcpConfig) {
            if ($Execute) { Add-Issue $message } else { Write-Host "CONFLICT $message" }
            return
        }

        Invoke-OnboardingCommand -Command 'codex' -Arguments @('mcp', 'remove', $serverName)
    }

    Invoke-OnboardingCommand -Command 'codex' -Arguments $addArgs
}

function Install-ClaudeMcpConfig {
    param([pscustomobject]$Adapter)

    if ($null -eq (Get-Command claude -ErrorAction SilentlyContinue)) {
        Add-Issue 'Claude MCP config requested, but claude CLI is not available.'
        return
    }

    $serverName = Get-McpServerName -Adapter $Adapter
    $scope = Get-McpScope -Adapter $Adapter
    $getOutput = & claude mcp get $serverName 2>&1
    $exists = ($LASTEXITCODE -eq 0)
    $outputText = ($getOutput | Out-String)

    if ($exists -and (Test-McpOutputMatches -Output $outputText -Adapter $Adapter) -and $outputText -match "Scope:\s+User config") {
        Write-Host "SKIP Claude MCP config already matches '$serverName' in user scope."
        return
    }

    $addArgs = if ($McpMode -eq 'Http') {
        @('mcp', 'add', '--scope', $scope, '--transport', 'http', $serverName, (Get-McpUrl -Adapter $Adapter))
    }
    else {
        @('mcp', 'add', '--scope', $scope, $serverName, '--', (Get-McpProxyCommand))
    }

    if ($exists) {
        $message = "Claude MCP config '$serverName' exists and differs. Use -ReplaceMcpConfig to replace it through claude mcp remove/add."
        if (-not $ReplaceMcpConfig) {
            if ($Execute) { Add-Issue $message } else { Write-Host "CONFLICT $message" }
            return
        }

        Invoke-OnboardingCommand -Command 'claude' -Arguments @('mcp', 'remove', $serverName, '-s', $scope)
    }

    Invoke-OnboardingCommand -Command 'claude' -Arguments $addArgs
}

function Install-ProjectMcpConfig {
    param(
        [string]$ResolvedProjectRoot
    )

    $templateName = if ($McpMode -eq 'Proxy') { '.mcp.json.proxy.example' } else { '.mcp.json.example' }
    $templatePath = Join-Path $templatesRoot $templateName
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
        Write-Error "MCP template does not exist: $templatePath"
    }

    $targetPath = if ([string]::IsNullOrWhiteSpace($McpConfigPath)) {
        Join-Path $ResolvedProjectRoot '.mcp.json'
    }
    else {
        Resolve-MonolithPath -Path $McpConfigPath -BasePath $ResolvedProjectRoot
    }

    $templateText = Get-Content -LiteralPath $templatePath -Raw
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        if ($Execute) {
            $targetDir = Split-Path -Parent $targetPath
            if (-not (Test-Path -LiteralPath $targetDir -PathType Container)) {
                New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
            }
            Set-Content -LiteralPath $targetPath -Value $templateText -NoNewline -Encoding utf8
            Write-Host "CREATE MCP config $targetPath from $templateName"
        }
        else {
            Write-Host "DRYRUN create MCP config $targetPath from $templateName"
        }
        return
    }

    $existingText = Get-Content -LiteralPath $targetPath -Raw
    if ($existingText -eq $templateText) {
        Write-Host "SKIP MCP config already matches $targetPath"
        return
    }

    if (-not $ReplaceMcpConfig) {
        $message = "MCP config exists and differs: $targetPath. Use -ReplaceMcpConfig to back it up and replace it."
        if ($Execute) {
            Add-Issue $message
        }
        else {
            Write-Host "CONFLICT $message"
        }
        return
    }

    $backupPath = "$targetPath.backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    if ($Execute) {
        Copy-Item -LiteralPath $targetPath -Destination $backupPath
        Set-Content -LiteralPath $targetPath -Value $templateText -NoNewline -Encoding utf8
        Write-Host "REPLACE MCP config $targetPath, backup $backupPath"
    }
    else {
        Write-Host "DRYRUN replace MCP config $targetPath, backup $backupPath"
    }
}

function Install-McpConfig {
    param(
        [pscustomobject[]]$SelectedAdapters,
        [string]$ResolvedProjectRoot
    )

    $mcpAdapters = @($SelectedAdapters | Where-Object { Get-BoolProperty -Object $_ -Name 'supportsMcpConfig' })
    if ($mcpAdapters.Count -eq 0) {
        Write-Host 'SKIP MCP config: selected targets do not consume MCP config.'
        return
    }

    if ($ProjectMcpConfig -or -not [string]::IsNullOrWhiteSpace($McpConfigPath)) {
        Install-ProjectMcpConfig -ResolvedProjectRoot $ResolvedProjectRoot
        return
    }

    foreach ($adapter in $mcpAdapters) {
        $kind = Get-StringProperty -Object $adapter -Name 'mcpConfigKind'
        $id = Get-StringProperty -Object $adapter -Name 'id'
        switch ($kind) {
            'codex-cli' { Install-CodexMcpConfig -Adapter $adapter }
            'claude-cli' { Install-ClaudeMcpConfig -Adapter $adapter }
            default {
                Write-Host "SKIP MCP config for $id`: no global MCP config handler. Use -ProjectMcpConfig for .mcp.json clients."
            }
        }
    }
}

function Get-InstructionBlock {
    param([string]$ResolvedInstructionRoot)

    $onboardingPath = Join-Path $templatesRoot 'Onboarding\Onboarding.md'
    $onboardingRef = Get-RelativePathText -FromDirectory $ResolvedInstructionRoot -ToPath $onboardingPath
    $pluginRef = Get-RelativePathText -FromDirectory $ResolvedInstructionRoot -ToPath $repoRoot

    return @"
<!-- BEGIN MONOLITH ONBOARDING -->
## Monolith MCP
This project uses Monolith MCP from $pluginRef.

- Use monolith_status() before editor-backed actions.
- Use monolith_find("<task>") for routing and focused monolith_discover({ "namespace": "<ns>", "action": "<action>", "mode": "schema" }) for exact parameters.
- Use Binaries\monolith_query.exe only as the read-only offline fallback when MCP/editor access is down.
- Do not copy Monolith skills into global skill roots; link them with Scripts\onboard_monolith.ps1 or Scripts\install_monolith_skills.ps1.
- See $onboardingRef for setup, verification, and recovery steps.
<!-- END MONOLITH ONBOARDING -->
"@
}

function Update-InstructionFile {
    param(
        [string]$Path,
        [string]$Block
    )

    $begin = '<!-- BEGIN MONOLITH ONBOARDING -->'
    $end = '<!-- END MONOLITH ONBOARDING -->'

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($Execute) {
            $dir = Split-Path -Parent $Path
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
                New-Item -ItemType Directory -Path $dir -Force | Out-Null
            }
            Set-Content -LiteralPath $Path -Value ($Block + [Environment]::NewLine) -Encoding utf8
            Write-Host "CREATE instruction file $Path"
        }
        else {
            Write-Host "DRYRUN create instruction file $Path"
        }
        return
    }

    $content = Get-Content -LiteralPath $Path -Raw
    if ($content.Contains($begin) -and $content.Contains($end)) {
        $escapedBegin = [regex]::Escape($begin)
        $escapedEnd = [regex]::Escape($end)
        $newContent = [regex]::Replace($content, "$escapedBegin.*?$escapedEnd", [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $Block }, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if ($newContent -eq $content) {
            Write-Host "SKIP instruction block unchanged $Path"
            return
        }
        if ($Execute) {
            Set-Content -LiteralPath $Path -Value $newContent -NoNewline -Encoding utf8
            Write-Host "UPDATE instruction block $Path"
        }
        else {
            Write-Host "DRYRUN update instruction block $Path"
        }
        return
    }

    if ($content -match '(?m)^##\s+\d*\.?\s*Monolith Onboarding and Skills' -or
        ($content -match 'Onboarding\.md' -and $content -match 'install_monolith_skills\.ps1|onboard_monolith\.ps1')) {
        Write-Host "SKIP instruction file already contains Monolith onboarding guidance $Path"
        return
    }

    if ($Execute) {
        Add-Content -LiteralPath $Path -Value ([Environment]::NewLine + $Block) -Encoding utf8
        Write-Host "APPEND instruction block $Path"
    }
    else {
        Write-Host "DRYRUN append instruction block $Path"
    }
}

function Update-Instructions {
    param(
        [pscustomobject[]]$SelectedAdapters,
        [string]$ResolvedInstructionRoot
    )

    $files = New-Object System.Collections.Generic.List[string]
    foreach ($adapter in $SelectedAdapters) {
        if (-not (Get-BoolProperty -Object $adapter -Name 'supportsInstructions')) { continue }
        foreach ($file in (Get-StringArrayProperty -Object $adapter -Name 'instructionFiles')) {
            if ([string]::IsNullOrWhiteSpace($file)) { continue }
            if (-not $files.Contains($file)) {
                $files.Add($file) | Out-Null
            }
        }
    }

    if ($files.Count -eq 0) {
        Write-Host 'SKIP instructions: selected targets do not use project instruction files.'
        return
    }

    $block = Get-InstructionBlock -ResolvedInstructionRoot $ResolvedInstructionRoot
    foreach ($file in $files) {
        $path = Resolve-MonolithPath -Path $file -BasePath $ResolvedInstructionRoot
        Update-InstructionFile -Path $path -Block $block
    }
}

$resolvedProjectRoot = if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    Get-DefaultProjectRoot
}
else {
    Resolve-MonolithPath -Path $ProjectRoot -BasePath $repoRoot
}

$resolvedInstructionRoot = if ([string]::IsNullOrWhiteSpace($InstructionRoot)) {
    $repoRoot
}
else {
    Resolve-MonolithPath -Path $InstructionRoot -BasePath $repoRoot
}

$adapters = Read-OnboardingAdapters
$targetIds = Normalize-Targets -Adapters $adapters
$selectedAdapters = @($targetIds | ForEach-Object { $adapters[$_] })
$mode = if ($Execute) { 'EXECUTE' } else { 'PLAN' }

Write-Host "Monolith onboarding: mode=$mode targets=$($targetIds -join ',') mcp=$McpMode"
Write-Host "RepoRoot: $repoRoot"
Write-Host "ProjectRoot: $resolvedProjectRoot"
Write-Host "InstructionRoot: $resolvedInstructionRoot"

Invoke-SkillValidation

if ($SkipSkills) {
    Write-Host 'SKIP skill links by request.'
}
else {
    Invoke-SkillInstall -SelectedAdapters $selectedAdapters
}

if ($SkipMcpConfig) {
    Write-Host 'SKIP MCP config by request.'
}
else {
    Install-McpConfig -SelectedAdapters $selectedAdapters -ResolvedProjectRoot $resolvedProjectRoot
}

if ($SkipInstructions) {
    Write-Host 'SKIP instructions by request.'
}
else {
    Update-Instructions -SelectedAdapters $selectedAdapters -ResolvedInstructionRoot $resolvedInstructionRoot
}

if ($issues.Count -gt 0) {
    Write-Host "Monolith onboarding completed with $($issues.Count) conflict(s)."
    if ($Execute) {
        exit 1
    }
}
else {
    Write-Host 'Monolith onboarding completed without conflicts.'
}
