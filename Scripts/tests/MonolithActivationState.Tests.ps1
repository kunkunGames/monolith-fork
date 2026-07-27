# Requires -Version 5.1

Describe 'Monolith activation config hierarchy' {
    BeforeAll {
        $helper = Join-Path (Split-Path -Parent $PSScriptRoot) 'monolith_activation_state.ps1'
        . $helper
    }

    It 'uses the generated WindowsEditor Monolith.ini as the canonical user path' {
        $root = Join-Path $TestDrive 'canonical-path'
        $expected = Join-Path $root 'Saved\Config\WindowsEditor\Monolith.ini'

        (Get-MonolithActivationStatePath -Root $root) | Should Be $expected
    }

    It 'defaults both services on when no config layer defines a value' {
        $root = Join-Path $TestDrive 'missing'
        New-Item -ItemType Directory -Path $root -Force | Out-Null

        $state = Get-MonolithActivationState -Root $root
        $state.Exists | Should Be $false
        $state.ServerEnabled | Should Be $true
        $state.IndexingEnabled | Should Be $true
        $state.ServerProjectDefault | Should Be $true
        $state.IndexingProjectDefault | Should Be $true
        @($state.InvalidKeys).Count | Should Be 0
    }

    It 'reads plugin DefaultMonolith.ini as the base project policy' {
        $root = Join-Path $TestDrive 'plugin-default'
        $path = Join-Path $root 'Plugins\Monolith\Config\DefaultMonolith.ini'
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        @'
[/Script/MonolithCore.MonolithSettings]
bServerEnabledByDefault=False
bIndexingEnabledByDefault=True
'@ | Set-Content -LiteralPath $path -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $true
        $state.ServerValuePresent | Should Be $false
        $state.IndexingValuePresent | Should Be $false
    }

    It 'lets project Config DefaultMonolith.ini override the plugin default' {
        $root = Join-Path $TestDrive 'project-default'
        $pluginPath = Join-Path $root 'Plugins\Monolith\Config\DefaultMonolith.ini'
        $projectPath = Join-Path $root 'Config\DefaultMonolith.ini'
        New-Item -ItemType Directory -Path (Split-Path -Parent $pluginPath) -Force | Out-Null
        New-Item -ItemType Directory -Path (Split-Path -Parent $projectPath) -Force | Out-Null
        @'
[/Script/MonolithCore.MonolithSettings]
bServerEnabledByDefault=False
bIndexingEnabledByDefault=False
'@ | Set-Content -LiteralPath $pluginPath -Encoding UTF8
        @'
[/Script/MonolithCore.MonolithSettings]
bServerEnabledByDefault=True
bIndexingEnabledByDefault=False
'@ | Set-Content -LiteralPath $projectPath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerProjectDefault | Should Be $true
        $state.IndexingProjectDefault | Should Be $false
        $state.ServerEnabled | Should Be $true
        $state.IndexingEnabled | Should Be $false
    }

    It 'applies a partial user override while the sibling inherits project policy' {
        $root = Join-Path $TestDrive 'user-partial'
        $projectPath = Join-Path $root 'Config\DefaultMonolith.ini'
        $statePath = Get-MonolithActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $projectPath) -Force | Out-Null
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        @'
[/Script/MonolithCore.MonolithSettings]
bServerEnabledByDefault=True
bIndexingEnabledByDefault=False
'@ | Set-Content -LiteralPath $projectPath -Encoding UTF8
        @'
[Monolith.UserActivation]
ServerEnabled=False
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $false
        $state.ServerUserOverridePresent | Should Be $true
        $state.IndexingUserOverridePresent | Should Be $false
    }

    It 'fails only the malformed user key closed' {
        $root = Join-Path $TestDrive 'invalid-user'
        $statePath = Get-MonolithActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        @'
[Monolith.UserActivation]
ServerEnabled=surprise
IndexingEnabled=True
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $true
        (@($state.InvalidKeys) -contains 'ServerEnabled') | Should Be $true
        (@($state.InvalidKeys) -contains 'IndexingEnabled') | Should Be $false
    }

    It 'uses the legacy activation file only when the generated user key is absent' {
        $root = Join-Path $TestDrive 'legacy-fallback'
        $legacyPath = Get-MonolithLegacyActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $legacyPath) -Force | Out-Null
        @'
[Monolith.Activation]
ServerEnabled=False
IndexingEnabled=True
'@ | Set-Content -LiteralPath $legacyPath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $true
        $state.ServerLegacyOverridePresent | Should Be $true
        $state.IndexingLegacyOverridePresent | Should Be $true
    }

    It 'gives generated user values precedence over conflicting legacy values' {
        $root = Join-Path $TestDrive 'user-wins-legacy'
        $statePath = Get-MonolithActivationStatePath -Root $root
        $legacyPath = Get-MonolithLegacyActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        New-Item -ItemType Directory -Path (Split-Path -Parent $legacyPath) -Force | Out-Null
        @'
[Monolith.UserActivation]
ServerEnabled=True
IndexingEnabled=False
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8
        @'
[Monolith.Activation]
ServerEnabled=False
IndexingEnabled=True
'@ | Set-Content -LiteralPath $legacyPath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $true
        $state.IndexingEnabled | Should Be $false
        $state.ServerUserOverridePresent | Should Be $true
        $state.IndexingUserOverridePresent | Should Be $true
        $state.ServerLegacyOverridePresent | Should Be $false
        $state.IndexingLegacyOverridePresent | Should Be $false
    }
}
