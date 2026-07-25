# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
Reads the durable Monolith server/indexing activation state.

.DESCRIPTION
The authoritative state file is <ProjectRoot>\Saved\Monolith\Activation.ini.
Missing values default to enabled, while malformed values fail closed to
disabled, matching FMonolithActivationState in MonolithCore.

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

function Get-MonolithActivationState {
    param([string]$Root)

    $statePath = Get-MonolithActivationStatePath -Root $Root
    $values = @{
        ServerEnabled = $true
        IndexingEnabled = $true
    }
    $found = @{
        ServerEnabled = $false
        IndexingEnabled = $false
    }
    $invalid = New-Object System.Collections.Generic.List[string]

    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        $currentSection = ''
        foreach ($lineValue in @(Get-Content -LiteralPath $statePath -ErrorAction Stop)) {
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
            if ($currentSection -ine 'Monolith.Activation' -or
                $line -notmatch '^(?<key>[^=]+)=(?<value>.*)$') {
                continue
            }

            $key = $matches['key'].Trim()
            if ($key -notin @('ServerEnabled', 'IndexingEnabled')) {
                continue
            }

            $found[$key] = $true
            $parsed = $false
            if (ConvertFrom-MonolithActivationBool -Value $matches['value'] -ParsedValue ([ref]$parsed)) {
                $values[$key] = $parsed
            }
            else {
                $values[$key] = $false
                if (-not $invalid.Contains($key)) {
                    $invalid.Add($key)
                }
            }
        }
    }

    return [PSCustomObject]@{
        StatePath = $statePath
        Exists = (Test-Path -LiteralPath $statePath -PathType Leaf)
        ServerEnabled = [bool]$values.ServerEnabled
        IndexingEnabled = [bool]$values.IndexingEnabled
        ServerValuePresent = [bool]$found.ServerEnabled
        IndexingValuePresent = [bool]$found.IndexingEnabled
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
