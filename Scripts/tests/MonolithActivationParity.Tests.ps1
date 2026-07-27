# Shared with Source/MonolithCore/Private/Tests/MonolithActivationParityTests.cpp.
# The offline scripts must resolve activation with no editor running, so the
# precedence rule is necessarily implemented twice. Both suites drive the same
# ActivationParityCases.json so drift between them fails a test instead of
# surfacing as two green suites that disagree in production.

Describe 'Monolith activation parity matrix' {
    BeforeAll {
        $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
        . (Join-Path $script:ScriptsRoot 'monolith_activation_state.ps1')
        $script:CasesPath = Join-Path $PSScriptRoot 'ActivationParityCases.json'
        $script:Cases = @((Get-Content -LiteralPath $script:CasesPath -Raw | ConvertFrom-Json).cases)
    }

    It 'declares shared cases' {
        ($script:Cases.Count -gt 0) | Should Be $true
    }

    It 'resolves every shared case exactly like UMonolithSettings' {
        foreach ($case in $script:Cases) {
            $root = Join-Path ([System.IO.Path]::GetTempPath()) ('MonolithParity-' + [guid]::NewGuid().ToString('N'))
            try {
                $pluginDefault = Join-Path $root 'Plugins\Monolith\Config\DefaultMonolith.ini'
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $pluginDefault) | Out-Null
                Set-Content -LiteralPath $pluginDefault -Value @(
                    '[/Script/MonolithCore.MonolithSettings]',
                    ('bServerEnabledByDefault={0}' -f $case.projectDefault.server),
                    ('bIndexingEnabledByDefault={0}' -f $case.projectDefault.indexing))

                if ($null -ne $case.user) {
                    $userPath = Join-Path $root 'Saved\Config\WindowsEditor\Monolith.ini'
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $userPath) | Out-Null
                    $userLines = @('[Monolith.UserActivation]')
                    if ($null -ne $case.user.server) { $userLines += "ServerEnabled=$($case.user.server)" }
                    if ($null -ne $case.user.indexing) { $userLines += "IndexingEnabled=$($case.user.indexing)" }
                    Set-Content -LiteralPath $userPath -Value $userLines
                }

                if ($null -ne $case.legacy) {
                    $legacyPath = Join-Path $root 'Saved\Monolith\Activation.ini'
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $legacyPath) | Out-Null
                    $legacyLines = @('[Monolith.Activation]')
                    if ($null -ne $case.legacy.server) { $legacyLines += "ServerEnabled=$($case.legacy.server)" }
                    if ($null -ne $case.legacy.indexing) { $legacyLines += "IndexingEnabled=$($case.legacy.indexing)" }
                    Set-Content -LiteralPath $legacyPath -Value $legacyLines
                }

                $state = Get-MonolithActivationState -Root $root

                "$($case.name)|server=$($state.ServerEnabled)" |
                    Should Be "$($case.name)|server=$($case.expect.server)"
                "$($case.name)|indexing=$($state.IndexingEnabled)" |
                    Should Be "$($case.name)|indexing=$($case.expect.indexing)"
                "$($case.name)|serverUserSet=$($state.ServerValuePresent)" |
                    Should Be "$($case.name)|serverUserSet=$($case.expect.serverUserSet)"
                "$($case.name)|indexingUserSet=$($state.IndexingValuePresent)" |
                    Should Be "$($case.name)|indexingUserSet=$($case.expect.indexingUserSet)"
            }
            finally {
                if (Test-Path -LiteralPath $root) {
                    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}
