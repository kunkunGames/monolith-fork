# Requires -Version 5.1

Describe 'Monolith activation state parser' {
    BeforeAll {
        $helper = Join-Path (Split-Path -Parent $PSScriptRoot) 'monolith_activation_state.ps1'
        . $helper
    }

    It 'defaults both features to enabled when the state file is missing' {
        $root = Join-Path $TestDrive 'missing'
        New-Item -ItemType Directory -Path $root -Force | Out-Null

        $state = Get-MonolithActivationState -Root $root
        $state.Exists | Should Be $false
        $state.ServerEnabled | Should Be $true
        $state.IndexingEnabled | Should Be $true
        @($state.InvalidKeys).Count | Should Be 0
    }

    It 'honors an explicit stop while a missing sibling key stays default-on' {
        $root = Join-Path $TestDrive 'partial'
        $statePath = Get-MonolithActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        @'
[Monolith.Activation]
ServerEnabled=False
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $true
        $state.ServerValuePresent | Should Be $true
        $state.IndexingValuePresent | Should Be $false
    }

    It 'reads both enabled flags from the canonical section' {
        $root = Join-Path $TestDrive 'enabled'
        $statePath = Get-MonolithActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        @'
[Other]
ServerEnabled=False

[Monolith.Activation]
ServerEnabled=True
IndexingEnabled=1
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $true
        $state.IndexingEnabled | Should Be $true
        $state.ServerValuePresent | Should Be $true
        $state.IndexingValuePresent | Should Be $true
    }

    It 'fails only the malformed key closed' {
        $root = Join-Path $TestDrive 'invalid'
        $statePath = Get-MonolithActivationStatePath -Root $root
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        @'
[Monolith.Activation]
ServerEnabled=surprise
IndexingEnabled=False
'@ | Set-Content -LiteralPath $statePath -Encoding UTF8

        $state = Get-MonolithActivationState -Root $root
        $state.ServerEnabled | Should Be $false
        $state.IndexingEnabled | Should Be $false
        (@($state.InvalidKeys) -contains 'ServerEnabled') | Should Be $true
        (@($state.InvalidKeys) -contains 'IndexingEnabled') | Should Be $false
    }
}
