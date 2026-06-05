param(
    [string[]]$Targets = @('Codex', 'Claude'),
    [string]$SourceRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Skills'),
    [string]$CodexRoot = (Join-Path $HOME '.codex\skills'),
    [string]$ClaudeRoot = (Join-Path $HOME '.claude\skills'),
    [string[]]$TargetRootSpecs = @(),
    [switch]$Execute,
    [switch]$ReplaceCopies,
    [switch]$UseSymlink
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$errors = New-Object System.Collections.Generic.List[string]
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'

function Add-InstallError {
    param([string]$Message)
    $script:errors.Add($Message) | Out-Null
    Write-Host "ERROR $Message"
}

function Get-IsWindows {
    return $env:OS -eq 'Windows_NT'
}

function Get-ReparseTargetText {
    param([System.IO.FileSystemInfo]$Item)
    try {
        $target = $Item.Target
        if ($null -eq $target) { return '' }
        if ($target -is [array]) { return ($target -join ';') }
        return [string]$target
    }
    catch {
        return ''
    }
}

function Test-ReparsePoint {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    return (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Get-SkillHash {
    param([string]$SkillDir)
    $skillPath = Join-Path $SkillDir 'SKILL.md'
    if (-not (Test-Path -LiteralPath $skillPath -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $skillPath -Algorithm SHA256).Hash
}

function New-SkillDirectoryLink {
    param(
        [string]$Source,
        [string]$Target
    )

    $itemType = 'SymbolicLink'
    if ((Get-IsWindows) -and -not $UseSymlink) {
        $itemType = 'Junction'
    }

    New-Item -ItemType $itemType -Path $Target -Target $Source | Out-Null
}

function Get-BackupPath {
    param([string]$TargetPath)
    $candidate = "${TargetPath}.backup-$script:timestamp"
    $index = 1
    while (Test-Path -LiteralPath $candidate) {
        $candidate = "${TargetPath}.backup-$script:timestamp-$index"
        $index++
    }
    return $candidate
}

function Install-Skill {
    param(
        [System.IO.DirectoryInfo]$SourceDir,
        [string]$TargetRoot,
        [string]$TargetName
    )

    $skillName = $SourceDir.Name
    $targetDir = Join-Path $TargetRoot $skillName
    $sourceHash = Get-SkillHash -SkillDir $SourceDir.FullName

    if ($null -eq $sourceHash) {
        Add-InstallError "${skillName}: source SKILL.md is missing."
        return
    }

    if (-not (Test-Path -LiteralPath $TargetRoot -PathType Container)) {
        if ($Execute) {
            New-Item -ItemType Directory -Path $TargetRoot -Force | Out-Null
            Write-Host "CREATE root $TargetRoot"
        }
        else {
            Write-Host "DRYRUN create root $TargetRoot"
        }
    }

    if (-not (Test-Path -LiteralPath $targetDir)) {
        if ($Execute) {
            New-SkillDirectoryLink -Source $SourceDir.FullName -Target $targetDir
            $targetHash = Get-SkillHash -SkillDir $targetDir
            if ($targetHash -ne $sourceHash) {
                Add-InstallError "${TargetName}/${skillName}: created link but hash verification failed."
                return
            }
            Write-Host "CREATE $TargetName/$skillName -> $($SourceDir.FullName)"
        }
        else {
            $linkType = if ((Get-IsWindows) -and -not $UseSymlink) { 'junction' } else { 'symlink' }
            Write-Host "DRYRUN create $linkType $TargetName/$skillName -> $($SourceDir.FullName)"
        }
        return
    }

    $item = Get-Item -LiteralPath $targetDir -Force
    $isLink = (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    $targetHashExisting = Get-SkillHash -SkillDir $targetDir
    $hashStatus = if ($targetHashExisting -eq $sourceHash) { 'hash-ok' } elseif ($null -eq $targetHashExisting) { 'missing-skill' } else { 'drift' }

    if ($isLink) {
        $linkTarget = Get-ReparseTargetText -Item $item
        if ($hashStatus -eq 'hash-ok') {
            Write-Host "SKIP $TargetName/$skillName linked $hashStatus $linkTarget"
        }
        else {
            Add-InstallError "${TargetName}/${skillName}: existing link hash status is $hashStatus."
        }
        return
    }

    if (-not $ReplaceCopies) {
        $message = "${TargetName}/${skillName}: copied install exists ($hashStatus); rerun with -Execute -ReplaceCopies to back it up and link it."
        if ($Execute) {
            Add-InstallError $message
        }
        else {
            Write-Host "CONFLICT $message"
        }
        return
    }

    $backupPath = Get-BackupPath -TargetPath $targetDir
    if ($Execute) {
        Move-Item -LiteralPath $targetDir -Destination $backupPath
        New-SkillDirectoryLink -Source $SourceDir.FullName -Target $targetDir
        $targetHashAfter = Get-SkillHash -SkillDir $targetDir
        if ($targetHashAfter -ne $sourceHash) {
            Add-InstallError "${TargetName}/${skillName}: replaced copied install but hash verification failed."
            return
        }
        Write-Host "REPLACE $TargetName/$skillName copied install moved to $backupPath"
    }
    else {
        Write-Host "DRYRUN replace $TargetName/$skillName copied install -> backup $backupPath, then link $($SourceDir.FullName)"
    }
}

if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    Write-Error "SourceRoot does not exist: $SourceRoot"
}

$skillDirs = @(Get-ChildItem -LiteralPath $SourceRoot -Directory | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'SKILL.md') -PathType Leaf
} | Sort-Object Name)

if ($skillDirs.Count -eq 0) {
    Write-Error "No skills with SKILL.md found under $SourceRoot"
}

$targetMap = @{
    Codex = $CodexRoot
    Claude = $ClaudeRoot
}

foreach ($spec in $TargetRootSpecs) {
    if ([string]::IsNullOrWhiteSpace($spec)) { continue }
    $separator = $spec.IndexOf('=')
    if ($separator -le 0) {
        Write-Error "Invalid TargetRootSpecs entry '$spec'. Expected Name=Path."
    }
    $name = $spec.Substring(0, $separator).Trim()
    $path = $spec.Substring($separator + 1).Trim()
    if ([string]::IsNullOrWhiteSpace($name) -or [string]::IsNullOrWhiteSpace($path)) {
        Write-Error "Invalid TargetRootSpecs entry '$spec'. Expected non-empty Name=Path."
    }
    $targetMap[$name] = [System.Environment]::ExpandEnvironmentVariables($path)
}

$normalizedTargets = New-Object System.Collections.Generic.List[string]
foreach ($targetArg in $Targets) {
    foreach ($part in ([string]$targetArg -split ',')) {
        $value = $part.Trim()
        if ([string]::IsNullOrWhiteSpace($value)) { continue }
        if (-not $targetMap.ContainsKey($value)) {
            $supported = ($targetMap.Keys | Sort-Object) -join ', '
            Write-Error "Unsupported target '$value'. Expected one of: $supported."
        }
        if (-not $normalizedTargets.Contains($value)) {
            $normalizedTargets.Add($value) | Out-Null
        }
    }
}

if ($normalizedTargets.Count -eq 0) {
    Write-Error 'No install targets were selected.'
}

$mode = if ($Execute) { 'EXECUTE' } else { 'DRYRUN' }
$linkMode = if ((Get-IsWindows) -and -not $UseSymlink) { 'junction' } else { 'symlink' }
Write-Host "Monolith skill installer: mode=$mode link=$linkMode skills=$($skillDirs.Count)"

foreach ($target in $normalizedTargets) {
    $root = $targetMap[$target]
    foreach ($skillDir in $skillDirs) {
        Install-Skill -SourceDir $skillDir -TargetRoot $root -TargetName $target
    }
}

if ($errors.Count -gt 0) {
    Write-Host "Monolith skill install failed: $($errors.Count) error(s)."
    exit 1
}

if (-not $Execute) {
    Write-Host 'Dry-run complete. Re-run with -Execute to create links. Add -ReplaceCopies to back up existing copied installs before linking.'
}
else {
    Write-Host 'Monolith skill install complete.'
}
