param(
    [string]$SkillsRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Skills'),
    [switch]$ClaudeAiExport,
    [string[]]$InstalledRoots = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$errors = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Add-SkillError {
    param([string]$Message)
    $script:errors.Add($Message) | Out-Null
}

function Add-SkillWarning {
    param([string]$Message)
    $script:warnings.Add($Message) | Out-Null
}

function Test-AbsolutePath {
    param([string]$Path)
    if ($Path -match '^[A-Za-z]:[\\/]') { return $true }
    if ($Path -match '^[\\/]') { return $true }
    return $false
}

function Split-MarkdownLinkTarget {
    param([string]$Target)
    $value = $Target.Trim()
    if ($value.StartsWith('<') -and $value.EndsWith('>')) {
        $value = $value.Substring(1, $value.Length - 2)
    }
    $hashIndex = $value.IndexOf('#')
    if ($hashIndex -eq 0) { return '' }
    if ($hashIndex -gt 0) {
        $value = $value.Substring(0, $hashIndex)
    }
    $queryIndex = $value.IndexOf('?')
    if ($queryIndex -gt 0) {
        $value = $value.Substring(0, $queryIndex)
    }
    return $value.Trim()
}

function Read-FrontMatter {
    param(
        [string]$Path,
        [string[]]$Lines
    )

    $result = @{
        Valid = $false
        Values = @{}
        EndIndex = -1
    }

    if ($Lines.Count -eq 0 -or $Lines[0].Trim() -ne '---') {
        Add-SkillError "${Path}: SKILL.md must start with YAML frontmatter ('---')."
        return $result
    }

    $endIndex = -1
    for ($i = 1; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i].Trim() -eq '---') {
            $endIndex = $i
            break
        }
    }

    if ($endIndex -lt 0) {
        Add-SkillError "${Path}: YAML frontmatter closing fence is missing."
        return $result
    }

    $values = @{}
    for ($i = 1; $i -lt $endIndex; $i++) {
        $line = $Lines[$i]
        if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith('#')) {
            continue
        }

        if ($line -match '^\s') {
            Add-SkillError "${Path}: frontmatter line $($i + 1) uses indentation; skill metadata must use top-level scalar keys."
            continue
        }

        if ($line -notmatch '^([A-Za-z0-9_-]+)\s*:\s*(.*)\s*$') {
            Add-SkillError "${Path}: frontmatter line $($i + 1) is not valid 'key: value' YAML."
            continue
        }

        $key = $Matches[1]
        $value = $Matches[2].Trim()
        if ($values.ContainsKey($key)) {
            Add-SkillError "${Path}: duplicate frontmatter key '$key'."
        }

        $startsDoubleQuote = $value.StartsWith('"')
        $endsDoubleQuote = $value.EndsWith('"')
        $startsSingleQuote = $value.StartsWith("'")
        $endsSingleQuote = $value.EndsWith("'")

        if ($startsDoubleQuote -or $endsDoubleQuote) {
            if (-not ($startsDoubleQuote -and $endsDoubleQuote -and $value.Length -ge 2)) {
                Add-SkillError "${Path}: frontmatter key '$key' has an unterminated double-quoted scalar."
            }
            else {
                try {
                    $value = $value | ConvertFrom-Json
                }
                catch {
                    Add-SkillError "${Path}: frontmatter key '$key' is not valid double-quoted YAML scalar syntax."
                    $value = $value.Substring(1, $value.Length - 2)
                }
            }
        }
        elseif ($startsSingleQuote -or $endsSingleQuote) {
            if (-not ($startsSingleQuote -and $endsSingleQuote -and $value.Length -ge 2)) {
                Add-SkillError "${Path}: frontmatter key '$key' has an unterminated single-quoted scalar."
            }
            else {
                $value = $value.Substring(1, $value.Length - 2).Replace("''", "'")
            }
        }
        else {
            if ($value.Contains(': ')) {
                Add-SkillError "${Path}: frontmatter key '$key' contains ': ' in an unquoted scalar; quote it for portable YAML."
            }

            $yamlIndicators = @('-', '?', ':', '{', '}', '[', ']', ',', '&', '*', '#', '!', '|', '>', '@', '`')
            if ($value.Length -gt 0 -and $yamlIndicators -contains [string]$value[0]) {
                Add-SkillError "${Path}: frontmatter key '$key' starts with YAML indicator '$($value[0])'; quote it."
            }
        }

        $values[$key] = [string]$value
    }

    $result.Valid = $true
    $result.Values = $values
    $result.EndIndex = $endIndex
    return $result
}

function Test-SkillFile {
    param(
        [System.IO.DirectoryInfo]$SkillDir
    )

    $skillName = $SkillDir.Name
    $skillPath = Join-Path $SkillDir.FullName 'SKILL.md'
    $legacyPath = Join-Path $SkillDir.FullName "$skillName.md"

    if (-not (Test-Path -LiteralPath $skillPath -PathType Leaf)) {
        Add-SkillError "${skillName}: missing SKILL.md."
        return
    }

    if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
        Add-SkillError "${skillName}: duplicate legacy entrypoint $skillName.md must be removed."
    }

    $content = Get-Content -LiteralPath $skillPath -Raw
    $lines = $content -split "`r?`n"
    $frontMatter = Read-FrontMatter -Path $skillPath -Lines $lines

    if ($frontMatter.Valid) {
        $values = $frontMatter.Values
        $allowedKeys = @('name', 'description')
        foreach ($key in $values.Keys) {
            if ($allowedKeys -notcontains $key) {
                Add-SkillError "${skillName}: unsupported frontmatter key '$key'. Only name and description are supported."
            }
        }

        $name = if ($values.ContainsKey('name')) { [string]$values['name'] } else { '' }
        $description = if ($values.ContainsKey('description')) { [string]$values['description'] } else { '' }

        if ([string]::IsNullOrWhiteSpace($name)) {
            Add-SkillError "${skillName}: frontmatter name is required."
        }
        elseif ($name -ne $skillName) {
            Add-SkillError "${skillName}: frontmatter name '$name' must match directory name."
        }

        if ($name -and $name -notmatch '^[a-z0-9]+(-[a-z0-9]+)*$') {
            Add-SkillError "${skillName}: name must be lowercase hyphen-case."
        }

        if ($name.Length -gt 64) {
            Add-SkillError "${skillName}: name exceeds 64 characters."
        }

        if ([string]::IsNullOrWhiteSpace($description)) {
            Add-SkillError "${skillName}: description is required."
        }
        elseif ($description.Length -gt 1024) {
            Add-SkillError "${skillName}: description exceeds 1024 characters."
        }

        if ($ClaudeAiExport -and $description.Length -gt 200) {
            Add-SkillError "${skillName}: description exceeds Claude.ai export limit of 200 characters."
        }

        $bodyLines = [Math]::Max(0, $lines.Count - ($frontMatter.EndIndex + 1))
        if ($bodyLines -gt 500) {
            Add-SkillError "${skillName}: body has $bodyLines lines; move bulk reference material out of SKILL.md."
        }
        elseif ($bodyLines -gt 300) {
            Add-SkillWarning "${skillName}: body has $bodyLines lines; consider moving reference tables to references/."
        }
    }

    if ($content -match 'C:\\Users\\[^\\\s)]+' -or
        $content -match '(?i)\b(api[_-]?key|openai_api_key)\b\s*[:=]\s*["'']?[^"''\s<>{}]+' -or
        $content -match '(?i)\bsk-[A-Za-z0-9_-]{16,}\b' -or
        $content -match '(?i)\bbearer\s+[A-Za-z0-9._~+/-]+=*' -or
        $content -match '(?i)\bauthorization\s*:' -or
        $content -match '(?i)\bprivate\s+key\b' -or
        $content -match '(?i)\bcookie\s*:') {
        Add-SkillError "${skillName}: SKILL.md appears to contain an absolute maintainer path or secret-like text."
    }

    $linkRegex = [regex]'!?\[[^\]]*\]\(([^)]+)\)'
    foreach ($match in $linkRegex.Matches($content)) {
        $target = Split-MarkdownLinkTarget -Target $match.Groups[1].Value
        if ([string]::IsNullOrWhiteSpace($target)) { continue }
        if ($target -match '^[A-Za-z][A-Za-z0-9+.-]*:' -and $target -notmatch '^[A-Za-z]:[\\/]') { continue }
        if (Test-AbsolutePath -Path $target) { continue }

        $resolved = Join-Path $SkillDir.FullName $target
        if (-not (Test-Path -LiteralPath $resolved)) {
            Add-SkillError "${skillName}: markdown link target '$target' does not exist."
        }
    }
}

function Test-InstalledRoot {
    param(
        [string]$Root,
        [System.IO.DirectoryInfo[]]$SkillDirs
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        Add-SkillError "Installed root '$Root' does not exist."
        return
    }

    foreach ($skillDir in $SkillDirs) {
        $sourceSkill = Join-Path $skillDir.FullName 'SKILL.md'
        $targetDir = Join-Path $Root $skillDir.Name
        $targetSkill = Join-Path $targetDir 'SKILL.md'

        if (-not (Test-Path -LiteralPath $targetDir)) {
            Add-SkillError "${Root}: missing installed skill '$($skillDir.Name)'."
            continue
        }

        $item = Get-Item -LiteralPath $targetDir -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) {
            Add-SkillError "${targetDir}: installed Monolith skill must be a directory link or junction."
            continue
        }

        if (-not (Test-Path -LiteralPath $targetSkill -PathType Leaf)) {
            Add-SkillError "${targetDir}: linked skill is missing SKILL.md."
            continue
        }

        $sourceHash = (Get-FileHash -LiteralPath $sourceSkill -Algorithm SHA256).Hash
        $targetHash = (Get-FileHash -LiteralPath $targetSkill -Algorithm SHA256).Hash
        if ($sourceHash -ne $targetHash) {
            Add-SkillError "${targetDir}: linked SKILL.md hash does not match repository source."
        }
    }
}

if (-not (Test-Path -LiteralPath $SkillsRoot -PathType Container)) {
    Write-Error "Skills root does not exist: $SkillsRoot"
}

$skillDirs = @(Get-ChildItem -LiteralPath $SkillsRoot -Directory | Sort-Object Name)
if ($skillDirs.Count -eq 0) {
    Add-SkillError "No skill directories found under $SkillsRoot."
}

foreach ($skillDir in $skillDirs) {
    Test-SkillFile -SkillDir $skillDir
}

$readme = Join-Path $SkillsRoot 'README.md'
if (Test-Path -LiteralPath $readme -PathType Leaf) {
    $readmeText = Get-Content -LiteralPath $readme -Raw
    if ($readmeText -match '~Actions' -and $readmeText -notmatch '(?i)snapshot') {
        Add-SkillError 'Skills/README.md contains static action counts but does not mark them as snapshots.'
    }
}

foreach ($root in $InstalledRoots) {
    if (-not [string]::IsNullOrWhiteSpace($root)) {
        Test-InstalledRoot -Root $root -SkillDirs $skillDirs
    }
}

foreach ($warning in $warnings) {
    Write-Warning $warning
}

if ($errors.Count -gt 0) {
    foreach ($errorMessage in $errors) {
        Write-Error $errorMessage -ErrorAction Continue
    }
    Write-Host "Monolith skill validation failed: $($errors.Count) error(s), $($warnings.Count) warning(s)."
    exit 1
}

Write-Host "Monolith skill validation passed: $($skillDirs.Count) skill(s), $($warnings.Count) warning(s)."
