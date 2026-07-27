# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
Reads Monolith's effective server/indexing activation.

.DESCRIPTION
Project defaults are bServerEnabledByDefault and bIndexingEnabledByDefault in
Config/DefaultMonolith.ini. Explicit console choices are per-user overrides in
Saved/Config/WindowsEditor/Monolith.ini under [Monolith.UserActivation].

Saved/Monolith/Activation.ini is read only as a legacy fallback until
UMonolithSettings migrates it. Missing values inherit the project default;
malformed values fail closed to disabled, matching UMonolithSettings.

When dot-sourced, this file only defines helper functions. When invoked
directly with -Feature, it exits 0 for enabled and 2 for disabled.
#>

[CmdletBinding()]
param(
    [Alias('ProjectRoot')]
    [string]$ActivationProjectRoot,

    [Alias('Feature')]
    [ValidateSet('', 'server', 'indexing')]
    [string]$ActivationFeature = '',

    [Alias('Quiet')]
    [switch]$ActivationQuiet
)

function Resolve-MonolithActivationProjectRoot {
    param([string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) {
        $pluginRoot = Split-Path -Parent $PSScriptRoot
        $pluginsRoot = Split-Path -Parent $pluginRoot
        $Root = Split-Path -Parent $pluginsRoot
    }

    try {
        return [System.IO.Path]::GetFullPath($Root)
    }
    catch {
        throw "Invalid project root '$Root': $($_.Exception.Message)"
    }
}

function Get-MonolithActivationStatePath {
    param([string]$Root)

    $resolvedRoot = Resolve-MonolithActivationProjectRoot -Root $Root
    return Join-Path $resolvedRoot 'Saved\Config\WindowsEditor\Monolith.ini'
}

function Get-MonolithLegacyActivationStatePath {
    param([string]$Root)

    $resolvedRoot = Resolve-MonolithActivationProjectRoot -Root $Root
    return Join-Path $resolvedRoot 'Saved\Monolith\Activation.ini'
}

function ConvertFrom-MonolithActivationBool {
    param(
        [string]$Value,
        [ref]$ParsedValue
    )

    $normalized = ([string]$Value).Trim()
    if ($normalized -match '^(?i:true|1)$') {
        $ParsedValue.Value = $true
        return $true
    }
    if ($normalized -match '^(?i:false|0)$') {
        $ParsedValue.Value = $false
        return $true
    }
    $ParsedValue.Value = $false
    return $false
}

function Get-MonolithIniSection {
    param(
        [string]$Path,
        [string]$Section,
        [string[]]$Keys
    )

    $values = @{}
    $present = @{}
    foreach ($key in $Keys) {
        $present[$key] = $false
    }

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $currentSection = ''
        foreach ($lineValue in @(Get-Content -LiteralPath $Path -ErrorAction Stop)) {
            $line = ([string]$lineValue).Trim()
            if ([string]::IsNullOrWhiteSpace($line) -or
                $line.StartsWith(';') -or
                $line.StartsWith('#')) {
                continue
            }

            if ($line -match '^\[(?<section>[^\]]+)\]\s*$') {
                $currentSection = $matches['section'].Trim()
                continue
            }
            if ($currentSection -ine $Section -or
                $line -notmatch '^(?<key>[^=]+)=(?<value>.*)$') {
                continue
            }

            $key = $matches['key'].Trim()
            if ($key -notin $Keys) {
                continue
            }

            $present[$key] = $true
            $values[$key] = $matches['value']
        }
    }

    return [PSCustomObject]@{
        Path = $Path
        Exists = (Test-Path -LiteralPath $Path -PathType Leaf)
        Values = $values
        Present = $present
    }
}

function Read-MonolithActivationBool {
    param(
        [object]$Layer,
        [string]$Key,
        [bool]$Fallback,
        [System.Collections.Generic.List[string]]$InvalidKeys
    )

    if (-not [bool]$Layer.Present[$Key]) {
        return $Fallback
    }

    $parsed = $false
    if (ConvertFrom-MonolithActivationBool -Value $Layer.Values[$Key] -ParsedValue ([ref]$parsed)) {
        return [bool]$parsed
    }

    if (-not $InvalidKeys.Contains($Key)) {
        $InvalidKeys.Add($Key)
    }
    return $false
}

function Get-MonolithActivationState {
    param([string]$Root)

    $resolvedRoot = Resolve-MonolithActivationProjectRoot -Root $Root
    $pluginDefaultPath = Join-Path $resolvedRoot 'Plugins\Monolith\Config\DefaultMonolith.ini'
    $projectDefaultPath = Join-Path $resolvedRoot 'Config\DefaultMonolith.ini'
    $statePath = Get-MonolithActivationStatePath -Root $resolvedRoot
    $legacyPath = Get-MonolithLegacyActivationStatePath -Root $resolvedRoot
    $invalid = New-Object System.Collections.Generic.List[string]

    $settingsSection = '/Script/MonolithCore.MonolithSettings'
    $defaultKeys = @('bServerEnabledByDefault', 'bIndexingEnabledByDefault')
    $pluginDefaults = Get-MonolithIniSection `
        -Path $pluginDefaultPath `
        -Section $settingsSection `
        -Keys $defaultKeys
    $projectDefaults = Get-MonolithIniSection `
        -Path $projectDefaultPath `
        -Section $settingsSection `
        -Keys $defaultKeys

    $serverProjectDefault = Read-MonolithActivationBool `
        -Layer $pluginDefaults `
        -Key 'bServerEnabledByDefault' `
        -Fallback $true `
        -InvalidKeys $invalid
    $indexingProjectDefault = Read-MonolithActivationBool `
        -Layer $pluginDefaults `
        -Key 'bIndexingEnabledByDefault' `
        -Fallback $true `
        -InvalidKeys $invalid
    $serverProjectDefault = Read-MonolithActivationBool `
        -Layer $projectDefaults `
        -Key 'bServerEnabledByDefault' `
        -Fallback $serverProjectDefault `
        -InvalidKeys $invalid
    $indexingProjectDefault = Read-MonolithActivationBool `
        -Layer $projectDefaults `
        -Key 'bIndexingEnabledByDefault' `
        -Fallback $indexingProjectDefault `
        -InvalidKeys $invalid

    $activationKeys = @('ServerEnabled', 'IndexingEnabled')
    $userOverrides = Get-MonolithIniSection `
        -Path $statePath `
        -Section 'Monolith.UserActivation' `
        -Keys $activationKeys
    $legacyOverrides = Get-MonolithIniSection `
        -Path $legacyPath `
        -Section 'Monolith.Activation' `
        -Keys $activationKeys

    $serverUserPresent = [bool]$userOverrides.Present.ServerEnabled
    $indexingUserPresent = [bool]$userOverrides.Present.IndexingEnabled
    $serverLegacyPresent = -not $serverUserPresent -and [bool]$legacyOverrides.Present.ServerEnabled
    $indexingLegacyPresent = -not $indexingUserPresent -and [bool]$legacyOverrides.Present.IndexingEnabled

    $serverEnabled = $serverProjectDefault
    if ($serverUserPresent) {
        $serverEnabled = Read-MonolithActivationBool `
            -Layer $userOverrides `
            -Key 'ServerEnabled' `
            -Fallback $serverProjectDefault `
            -InvalidKeys $invalid
    }
    elseif ($serverLegacyPresent) {
        $serverEnabled = Read-MonolithActivationBool `
            -Layer $legacyOverrides `
            -Key 'ServerEnabled' `
            -Fallback $serverProjectDefault `
            -InvalidKeys $invalid
    }

    $indexingEnabled = $indexingProjectDefault
    if ($indexingUserPresent) {
        $indexingEnabled = Read-MonolithActivationBool `
            -Layer $userOverrides `
            -Key 'IndexingEnabled' `
            -Fallback $indexingProjectDefault `
            -InvalidKeys $invalid
    }
    elseif ($indexingLegacyPresent) {
        $indexingEnabled = Read-MonolithActivationBool `
            -Layer $legacyOverrides `
            -Key 'IndexingEnabled' `
            -Fallback $indexingProjectDefault `
            -InvalidKeys $invalid
    }

    return [PSCustomObject]@{
        StatePath = $statePath
        LegacyStatePath = $legacyPath
        PluginDefaultPath = $pluginDefaultPath
        ProjectDefaultPath = $projectDefaultPath
        Exists = [bool]$userOverrides.Exists
        LegacyExists = [bool]$legacyOverrides.Exists
        ServerEnabled = [bool]$serverEnabled
        IndexingEnabled = [bool]$indexingEnabled
        ServerProjectDefault = [bool]$serverProjectDefault
        IndexingProjectDefault = [bool]$indexingProjectDefault
        ServerValuePresent = [bool]($serverUserPresent -or $serverLegacyPresent)
        IndexingValuePresent = [bool]($indexingUserPresent -or $indexingLegacyPresent)
        ServerUserOverridePresent = $serverUserPresent
        IndexingUserOverridePresent = $indexingUserPresent
        ServerLegacyOverridePresent = $serverLegacyPresent
        IndexingLegacyOverridePresent = $indexingLegacyPresent
        InvalidKeys = @($invalid)
    }
}

function Test-MonolithActivationEnabled {
    param(
        [string]$Root,
        [ValidateSet('server', 'indexing')]
        [string]$Target
    )

    $state = Get-MonolithActivationState -Root $Root
    if ($Target -eq 'server') {
        return [bool]$state.ServerEnabled
    }
    return [bool]$state.IndexingEnabled
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $state = Get-MonolithActivationState -Root $ActivationProjectRoot
        if ([string]::IsNullOrWhiteSpace($ActivationFeature)) {
            if (-not $ActivationQuiet) {
                $state | ConvertTo-Json -Depth 3 -Compress
            }
            exit 0
        }

        $enabled = if ($ActivationFeature -eq 'server') {
            [bool]$state.ServerEnabled
        }
        else {
            [bool]$state.IndexingEnabled
        }

        if (-not $ActivationQuiet) {
            $token = if ($enabled) { 'ENABLED' } else { 'DISABLED' }
            $invalidKeys = @($state.InvalidKeys) -join ','
            if ([string]::IsNullOrWhiteSpace($invalidKeys)) {
                $invalidKeys = '-'
            }
            Write-Output (
                'RESULT={0} feature={1} desired_enabled={2} state_path="{3}" invalid_keys={4}' -f
                    $token, $ActivationFeature, $enabled.ToString().ToLowerInvariant(), $state.StatePath, $invalidKeys)
        }
        if ($enabled) { exit 0 }
        exit 2
    }
    catch {
        if (-not $ActivationQuiet) {
            Write-Output ('RESULT=ERROR reason=activation_state_read_failed detail="{0}"' -f $_.Exception.Message)
        }
        exit 3
    }
}
