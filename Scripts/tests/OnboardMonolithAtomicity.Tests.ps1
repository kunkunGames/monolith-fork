# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

$ScriptsRoot = Split-Path -Parent $PSScriptRoot
$PluginRoot = Split-Path -Parent $ScriptsRoot
$OnboardPath = Join-Path $ScriptsRoot 'onboard_monolith.ps1'
$PowerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$Tokens = $null
$ParseErrors = $null
$OnboardAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $OnboardPath,
    [ref]$Tokens,
    [ref]$ParseErrors)
if ($ParseErrors.Count -gt 0) {
    throw ($ParseErrors | ForEach-Object { $_.Message } | Out-String)
}
foreach ($functionName in @('Test-ByteArraysEqual', 'Get-FileSha256', 'Get-McpProxyVersion', 'Resolve-McpProxyCommand')) {
    $functionAst = $OnboardAst.Find(
        {
            param($Node)
            $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $Node.Name -eq $functionName
        },
        $true)
    if (-not $functionAst) {
        throw "$functionName was not found in onboard_monolith.ps1"
    }
    Invoke-Expression $functionAst.Extent.Text
}

function Get-CurrentImmutableProxyFixture {
    $manifestPath = Join-Path $PluginRoot 'Binaries\monolith_proxy.current.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    return [pscustomobject]@{
        Manifest = $manifest
        Path = Join-Path (Split-Path -Parent $manifestPath) ([string]$manifest.file)
    }
}

function Write-TestProxyManifest {
    param(
        [string]$PluginRoot,
        [object]$Manifest
    )

    $path = Join-Path $PluginRoot 'Binaries\monolith_proxy.current.json'
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, ($Manifest | ConvertTo-Json -Compress), $utf8NoBom)
}

function Invoke-OnboardingTestProcess {
    param(
        [string[]]$Arguments,
        [hashtable]$Environment = @{},
        [string]$ScriptPath = $OnboardPath
    )

    $previous = @{}
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        foreach ($name in $Environment.Keys) {
            $previous[$name] = [System.Environment]::GetEnvironmentVariable($name, 'Process')
            [System.Environment]::SetEnvironmentVariable($name, [string]$Environment[$name], 'Process')
        }

        # A deliberately failing fake CLI writes to stderr. Capture that output
        # without allowing the test harness preference to terminate this caller.
        $ErrorActionPreference = 'Continue'
        $output = & $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments 2>&1
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = ($output | Out-String)
        }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
        foreach ($name in $Environment.Keys) {
            [System.Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}

Describe 'onboard_monolith transactional and atomic writes' {
    It 'resolves the authoritative immutable proxy manifest and rejects digest tampering' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $binaries = Join-Path $root 'Binaries'
        New-Item -ItemType Directory -Path $binaries -Force | Out-Null
        $fixture = Get-CurrentImmutableProxyFixture
        $target = Join-Path $binaries ([string]$fixture.Manifest.file)
        Copy-Item -LiteralPath $fixture.Path -Destination $target
        $manifest = [ordered]@{
            schema_version = 1
            file = [string]$fixture.Manifest.file
            version = [string]$fixture.Manifest.version
            source_hash = [string]$fixture.Manifest.source_hash
            sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
            tool = 'monolith-proxy'
            runtime = 'native-cpp'
        }

        try {
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            (Resolve-McpProxyCommand -PluginRoot $root) | Should Be $target

            $manifest.sha256 = ('0' * 64)
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true

            $manifest.sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
            $manifest.tool = 'cmd-wrapper'
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true

            $manifest.tool = 'monolith-proxy'
            $manifest.runtime = 'managed-wrapper'
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true

            $manifest.runtime = 'native-cpp'
            $manifest.version = '0.0.0-manifest-mismatch'
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'rejects manifest traversal, nonnumeric schema, and source-hash filename disagreement' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path (Join-Path $root 'Binaries') -Force | Out-Null
        try {
            $manifest = [ordered]@{
                schema_version = 1
                file = '..\monolith_proxy-0000000000000000.exe'
                version = '1.0.0'
                source_hash = '0000000000000000'
                sha256 = ('0' * 64)
                tool = 'monolith-proxy'
                runtime = 'native-cpp'
            }
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true

            $manifest.file = 'monolith_proxy-0000000000000000.exe'
            $manifest.source_hash = '1111111111111111'
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true

            $manifest.schema_version = '1'
            $manifest.source_hash = '0000000000000000'
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'rejects an immutable image whose --version source hash disagrees with its manifest' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $binaries = Join-Path $root 'Binaries'
        New-Item -ItemType Directory -Path $binaries -Force | Out-Null
        $fixture = Get-CurrentImmutableProxyFixture
        $fakeHash = '0000000000000000'
        if ([string]$fixture.Manifest.source_hash -eq $fakeHash) { $fakeHash = '1111111111111111' }
        $target = Join-Path $binaries "monolith_proxy-$fakeHash.exe"
        Copy-Item -LiteralPath $fixture.Path -Destination $target
        $manifest = [ordered]@{
            schema_version = 1
            file = "monolith_proxy-$fakeHash.exe"
            version = [string]$fixture.Manifest.version
            source_hash = $fakeHash
            sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
            tool = 'monolith-proxy'
            runtime = 'native-cpp'
        }

        try {
            Write-TestProxyManifest -PluginRoot $root -Manifest $manifest
            $rejected = $false
            try { Resolve-McpProxyCommand -PluginRoot $root | Out-Null }
            catch { $rejected = $true }
            $rejected | Should Be $true
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'keeps invalid-manifest plans nonmutating and fails execute before onboarding writes' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $scriptRoot = Join-Path $root 'Scripts'
        $adapterRoot = Join-Path $root 'Templates\Onboarding'
        $copiedOnboardPath = Join-Path $scriptRoot 'onboard_monolith.ps1'
        $target = Join-Path $root 'project\.mcp.json'
        New-Item -ItemType Directory -Path $scriptRoot, $adapterRoot -Force | Out-Null
        Copy-Item -LiteralPath $OnboardPath -Destination $copiedOnboardPath
        Copy-Item -LiteralPath (Join-Path $PluginRoot 'Templates\Onboarding\generic-mcp.json') -Destination $adapterRoot

        try {
            $arguments = @(
                '-Targets', 'GenericMcp',
                '-McpMode', 'Proxy',
                '-ProjectMcpConfig',
                '-McpConfigPath', $target,
                '-SkipSkills',
                '-SkipInstructions'
            )
            $planResult = Invoke-OnboardingTestProcess -Arguments ($arguments + '-Plan') -ScriptPath $copiedOnboardPath
            $planResult.ExitCode | Should Be 0
            $planResult.Output | Should Match 'Immutable proxy validation failed'
            $planResult.Output | Should Match 'SKIP MCP config: immutable proxy manifest validation did not pass'
            Test-Path -LiteralPath $target | Should Be $false

            $executeResult = Invoke-OnboardingTestProcess -Arguments ($arguments + '-Execute') -ScriptPath $copiedOnboardPath
            $executeResult.ExitCode | Should Not Be 0
            $executeResult.Output | Should Match 'No onboarding mutation was attempted'
            Test-Path -LiteralPath $target | Should Be $false
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'rolls back every selected global client config without printing its contents' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        $codexHome = Join-Path $root '.codex'
        $codexConfig = Join-Path $codexHome 'config.toml'
        $claudeConfig = Join-Path $root '.claude.json'
        New-Item -ItemType Directory -Path $fakeBin, $codexHome -Force | Out-Null

        $codexOriginal = [System.Text.Encoding]::UTF8.GetBytes('CODEX_SECRET_SENTINEL=original')
        $claudeOriginal = [System.Text.Encoding]::UTF8.GetBytes('CLAUDE_SECRET_SENTINEL=original')
        [System.IO.File]::WriteAllBytes($codexConfig, $codexOriginal)
        [System.IO.File]::WriteAllBytes($claudeConfig, $claudeOriginal)
        $originalTimestamp = [datetime]::SpecifyKind([datetime]'2025-01-02T03:04:05', [DateTimeKind]::Utc)
        [System.IO.File]::SetLastWriteTimeUtc($codexConfig, $originalTimestamp)
        [System.IO.File]::SetLastWriteTimeUtc($claudeConfig, $originalTimestamp)

        @'
@echo off
if /I "%2"=="get" (
  findstr /c:"codex-new" "%CODEX_HOME%\config.toml" >nul
  if errorlevel 1 (echo http://old.invalid/mcp) else (echo http://localhost:9316/mcp)
  exit /b 0
)
if /I "%2"=="remove" (
  >"%CODEX_HOME%\config.toml" echo codex-removed
  exit /b 0
)
if /I "%2"=="add" (
  >"%CODEX_HOME%\config.toml" echo codex-new
  exit /b 0
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii

        @'
@echo off
if /I "%2"=="get" (
  echo Scope: User config
  echo http://old.invalid/mcp
  exit /b 0
)
if /I "%2"=="remove" (
  >"%USERPROFILE%\.claude.json" echo claude-removed
  exit /b 0
)
if /I "%2"=="add" (
  >"%USERPROFILE%\.claude.json" echo claude-partial
  echo simulated add failure 1>&2
  exit /b 9
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'claude.cmd') -Encoding ascii

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex,Claude',
                '-McpMode', 'Http',
                '-ReplaceMcpConfig',
                '-SkipSkills',
                '-SkipInstructions',
                '-Execute'
            ) -Environment @{
                USERPROFILE = $root
                CODEX_HOME = $codexHome
                CLAUDE_CONFIG_DIR = ''
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Not Be 0
            $result.Output | Should Match 'ROLLBACK restored the selected global MCP client configuration files'
            $result.Output | Should Not Match 'CODEX_SECRET_SENTINEL'
            $result.Output | Should Not Match 'CLAUDE_SECRET_SENTINEL'
            [System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($codexConfig)) |
                Should Be ([System.Convert]::ToBase64String($codexOriginal))
            [System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($claudeConfig)) |
                Should Be ([System.Convert]::ToBase64String($claudeOriginal))
            [System.IO.File]::GetLastWriteTimeUtc($codexConfig) | Should Be $originalTimestamp
            [System.IO.File]::GetLastWriteTimeUtc($claudeConfig) | Should Be $originalTimestamp
            @(Get-ChildItem -LiteralPath $root -Recurse -Filter '*.monolith-*.tmp' -ErrorAction SilentlyContinue).Count |
                Should Be 0
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'removes transaction-created global config files through compare-and-swap rollback' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        $codexHome = Join-Path $root '.codex'
        $codexConfig = Join-Path $codexHome 'config.toml'
        $claudeConfig = Join-Path $root '.claude.json'
        New-Item -ItemType Directory -Path $fakeBin, $codexHome -Force | Out-Null

        @'
@echo off
if /I "%2"=="get" (
  if exist "%CODEX_HOME%\config.toml" (exit /b 0) else (exit /b 1)
)
if /I "%2"=="add" (
  >"%CODEX_HOME%\config.toml" echo codex-created
  exit /b 0
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii

        @'
@echo off
if /I "%2"=="get" (
  if exist "%USERPROFILE%\.claude.json" (exit /b 0) else (exit /b 1)
)
if /I "%2"=="add" (
  >"%USERPROFILE%\.claude.json" echo claude-created-partial
  exit /b 9
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'claude.cmd') -Encoding ascii

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex,Claude',
                '-McpMode', 'Http',
                '-SkipSkills',
                '-SkipInstructions',
                '-Execute'
            ) -Environment @{
                USERPROFILE = $root
                CODEX_HOME = $codexHome
                CLAUDE_CONFIG_DIR = ''
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Not Be 0
            $result.Output | Should Match 'ROLLBACK restored the selected global MCP client configuration files'
            Test-Path -LiteralPath $codexConfig | Should Be $false
            Test-Path -LiteralPath $claudeConfig | Should Be $false
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'atomically replaces project MCP config and preserves the exact old bytes in a timestamped backup' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $target = Join-Path $root '.mcp.json'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $original = [System.Text.Encoding]::UTF8.GetBytes('PROJECT_MCP_ORIGINAL')
        [System.IO.File]::WriteAllBytes($target, $original)

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'GenericMcp',
                '-McpMode', 'Http',
                '-ProjectMcpConfig',
                '-McpConfigPath', $target,
                '-ReplaceMcpConfig',
                '-SkipSkills',
                '-SkipInstructions',
                '-Execute'
            )

            $result.ExitCode | Should Be 0
            $backups = @(Get-ChildItem -LiteralPath $root -Filter '.mcp.json.backup-*')
            $backups.Count | Should Be 1
            [System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($backups[0].FullName)) |
                Should Be ([System.Convert]::ToBase64String($original))
            (Get-Content -LiteralPath $target -Raw) |
                Should Be (Get-Content -LiteralPath (Join-Path $PluginRoot 'Templates\.mcp.json.example') -Raw)
            @(Get-ChildItem -LiteralPath $root -Filter '*.monolith-*.tmp').Count | Should Be 0
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'writes the validated absolute immutable proxy path to project MCP config' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $target = Join-Path $root '.mcp.json'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $fixture = Get-CurrentImmutableProxyFixture
        $expectedPath = [System.IO.Path]::GetFullPath($fixture.Path)

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'GenericMcp',
                '-McpMode', 'Proxy',
                '-ProjectMcpConfig',
                '-McpConfigPath', $target,
                '-SkipSkills',
                '-SkipInstructions',
                '-Execute'
            )

            $result.ExitCode | Should Be 0
            $config = Get-Content -LiteralPath $target -Raw | ConvertFrom-Json
            [string]$config.mcpServers.monolith.command | Should Be $expectedPath
            [System.IO.Path]::IsPathRooted([string]$config.mcpServers.monolith.command) | Should Be $true
            [string]$config.mcpServers.monolith.command | Should Match 'monolith_proxy-[0-9a-f]{16}\.exe$'
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'uses the validated immutable proxy path in global client add plans' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Path $fakeBin -Force | Out-Null
        '@echo off' + [Environment]::NewLine + 'exit /b 1' |
            Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii
        $expectedPath = [System.IO.Path]::GetFullPath((Get-CurrentImmutableProxyFixture).Path)

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex',
                '-McpMode', 'Proxy',
                '-SkipSkills',
                '-SkipInstructions',
                '-Plan'
            ) -Environment @{
                CODEX_HOME = (Join-Path $root '.codex')
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Be 0
            $result.Output | Should Match ([regex]::Escape($expectedPath))
            $result.Output | Should Not Match 'Binaries\\monolith_proxy\.exe(?:\s|$)'
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'rejects a wrapper command even when its arguments mention the immutable proxy path' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Path $fakeBin -Force | Out-Null
        @'
@echo off
if /I "%1"=="mcp" if /I "%2"=="get" (
  echo monolith
  echo   enabled: true
  echo   transport: stdio
  echo   command: C:\Windows\System32\cmd.exe
  echo   args: /d /c "%EXPECTED_MONOLITH_PROXY%"
  exit /b 0
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii
        $expectedPath = [System.IO.Path]::GetFullPath((Get-CurrentImmutableProxyFixture).Path)

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex',
                '-McpMode', 'Proxy',
                '-SkipSkills',
                '-SkipInstructions',
                '-Plan'
            ) -Environment @{
                CODEX_HOME = (Join-Path $root '.codex')
                EXPECTED_MONOLITH_PROXY = $expectedPath
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Be 0
            $result.Output | Should Match 'CONFLICT Codex MCP config'
            $result.Output | Should Not Match "SKIP Codex MCP config already matches"
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'accepts exactly one normalized command field for the immutable proxy path' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Path $fakeBin -Force | Out-Null
        @'
@echo off
if /I "%1"=="mcp" if /I "%2"=="get" (
  echo monolith
  echo   enabled: true
  echo   transport: stdio
  echo   command: "%EXPECTED_MONOLITH_PROXY%"
  echo   args: -
  exit /b 0
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii
        $expectedPath = [System.IO.Path]::GetFullPath((Get-CurrentImmutableProxyFixture).Path)

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex',
                '-McpMode', 'Proxy',
                '-SkipSkills',
                '-SkipInstructions',
                '-Plan'
            ) -Environment @{
                CODEX_HOME = (Join-Path $root '.codex')
                EXPECTED_MONOLITH_PROXY = $expectedPath
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Be 0
            $result.Output | Should Match "SKIP Codex MCP config already matches"
            $result.Output | Should Not Match 'CONFLICT Codex MCP config'
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'preserves a concurrent external config edit instead of overwriting it during rollback' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        $claudeConfig = Join-Path $root '.claude.json'
        New-Item -ItemType Directory -Path $fakeBin -Force | Out-Null
        [System.IO.File]::WriteAllText($claudeConfig, 'CLAUDE_ORIGINAL')

        @'
@echo off
if /I "%2"=="get" (
  findstr /c:"claude-new" "%USERPROFILE%\.claude.json" >nul
  if not errorlevel 1 (
    >"%USERPROFILE%\.claude.json" echo CLAUDE_EXTERNAL_CONCURRENT_EDIT
    echo Scope: User config
    echo URL: http://external.invalid/mcp
    exit /b 0
  )
  echo Scope: User config
  echo URL: http://old.invalid/mcp
  exit /b 0
)
if /I "%2"=="remove" (
  >"%USERPROFILE%\.claude.json" echo claude-removed
  exit /b 0
)
if /I "%2"=="add" (
  >"%USERPROFILE%\.claude.json" echo claude-new
  exit /b 0
)
exit /b 2
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'claude.cmd') -Encoding ascii

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Claude',
                '-McpMode', 'Http',
                '-ReplaceMcpConfig',
                '-SkipSkills',
                '-SkipInstructions',
                '-Execute'
            ) -Environment @{
                USERPROFILE = $root
                CLAUDE_CONFIG_DIR = ''
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Not Be 0
            $result.Output | Should Match 'Rollback compare-and-swap rejected a concurrent modification'
            (Get-Content -LiteralPath $claudeConfig -Raw).Trim() | Should Be 'CLAUDE_EXTERNAL_CONCURRENT_EDIT'
            $result.Output | Should Not Match 'CLAUDE_EXTERNAL_CONCURRENT_EDIT'
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'counts nonmutating Plan conflicts instead of reporting a false clean result' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        $fakeBin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Path $fakeBin -Force | Out-Null
        @'
@echo off
if /I "%1"=="mcp" if /I "%2"=="get" (
  echo transport: http
  exit /b 0
)
exit /b 1
'@ | Set-Content -LiteralPath (Join-Path $fakeBin 'codex.cmd') -Encoding ascii

        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex',
                '-McpMode', 'Proxy',
                '-SkipSkills',
                '-SkipInstructions',
                '-Plan'
            ) -Environment @{
                CODEX_HOME = (Join-Path $root '.codex')
                PATH = "$fakeBin;$env:PATH"
            }

            $result.ExitCode | Should Be 0
            $result.Output | Should Match 'CONFLICT Codex MCP config'
            $result.Output | Should Match 'completed with 1 conflict\(s\)'
            $result.Output | Should Not Match 'completed without conflicts'
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'creates managed instruction files through the same atomic writer' {
        $root = Join-Path ([System.IO.Path]::GetTempPath()) "monolith-onboard-$([guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        try {
            $result = Invoke-OnboardingTestProcess -Arguments @(
                '-Targets', 'Codex',
                '-McpMode', 'Http',
                '-InstructionRoot', $root,
                '-SkipSkills',
                '-SkipMcpConfig',
                '-Execute'
            )

            $result.ExitCode | Should Be 0
            (Get-Content -LiteralPath (Join-Path $root 'AGENTS.md') -Raw) |
                Should Match '<!-- BEGIN MONOLITH ONBOARDING -->'
            @(Get-ChildItem -LiteralPath $root -Filter '*.monolith-*.tmp').Count | Should Be 0
        }
        finally {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
