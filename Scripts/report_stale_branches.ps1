<#
.SYNOPSIS
Non-destructive report for stale remote branch review candidates.

.DESCRIPTION
Reports remote-tracking branches that may be worth reviewing for cleanup:
branches already merged into the base ref, branches older than -StaleDays,
branches that look like no-op agent output, and branches with large numeric
suffix/id tokens. Protected branch names and prefixes are classified but never
receive delete suggestions.

The script only reads Git state. It does not fetch, push, delete branches, close
PRs, or call GitHub APIs. Delete commands are printed as suggestions only.

.PARAMETER Remote
Remote name to inspect. Default: origin.

.PARAMETER BaseRef
Base ref used for merged/no-op checks. Default: <Remote>/master.

.PARAMETER StaleDays
Branch age threshold in days. Default: 14.

.PARAMETER ProtectedPrefixes
Local branch names or prefixes that must never receive cleanup suggestions.
Prefix entries should end with '/'.

.PARAMETER NoDiffCheck
Skip the git diff --quiet <base>...<branch> no-op content check.

.PARAMETER ShowAll
Also print non-candidate KEEP lines.

.PARAMETER Limit
Maximum CANDIDATE lines to print. 0 means no limit.

.OUTPUTS
Line-oriented INFO / SUMMARY / CANDIDATE / PROTECTED / KEEP / RESULT output.

Exit codes:
  0  report completed, regardless of candidate count
  1  script/runtime error (missing git, invalid repo/ref, git command failure)
#>
[CmdletBinding()]
param(
    [string]$Remote = 'origin',
    [string]$BaseRef,
    [int]$StaleDays = 14,
    [string[]]$ProtectedPrefixes = @(
        'HEAD',
        'master',
        'main',
        'develop',
        'dev',
        'release/',
        'hotfix/',
        'production',
        'prod',
        'staging',
        'stable',
        'gh-pages'
    ),
    [switch]$NoDiffCheck,
    [switch]$ShowAll,
    [int]$Limit = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Format-LogValue {
    param([object]$Value)
    if ($null -eq $Value) { return '""' }
    $text = [string]$Value
    if ($text.Length -eq 0) { return '""' }
    if ($text -match '^[A-Za-z0-9_.:/@+=,\-]+$') { return $text }
    return ($text | ConvertTo-Json -Compress)
}

function Quote-PowerShellArgument {
    param([string]$Value)
    if ($Value -match '^[A-Za-z0-9._/\-]+$') { return $Value }
    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [int[]]$AllowedExitCodes = @(0)
    )

    $output = & git @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) { $exitCode = 0 }

    if ($AllowedExitCodes -notcontains $exitCode) {
        $commandText = 'git ' + ($Arguments -join ' ')
        $detail = (@($output) -join "`n").Trim()
        if ($detail.Length -eq 0) { $detail = '<no output>' }
        throw "$commandText failed with exit code $exitCode. $detail"
    }

    return [PSCustomObject]@{
        ExitCode = $exitCode
        Output   = @($output)
    }
}

function Test-ProtectedBranch {
    param(
        [string]$LocalName,
        [string]$RemoteName,
        [string[]]$Prefixes
    )

    foreach ($rawPrefix in $Prefixes) {
        if ([string]::IsNullOrWhiteSpace($rawPrefix)) { continue }
        $prefix = $rawPrefix.Trim()
        $remotePrefix = $RemoteName + '/'
        if ($prefix.StartsWith($remotePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $prefix = $prefix.Substring($remotePrefix.Length)
        }

        if ($prefix.EndsWith('/')) {
            if ($LocalName.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                return $true
            }
            continue
        }

        if ($LocalName.Equals($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }

    return $false
}

function Get-ConfidenceRank {
    param([string]$Confidence)
    switch ($Confidence) {
        'high' { return 0 }
        'medium' { return 1 }
        'low' { return 2 }
        'protected' { return 3 }
        default { return 4 }
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($Remote)) {
        throw 'Remote must not be empty.'
    }
    if ($StaleDays -lt 0) {
        throw 'StaleDays must be zero or greater.'
    }
    if ($Limit -lt 0) {
        throw 'Limit must be zero or greater.'
    }
    if ([string]::IsNullOrWhiteSpace($BaseRef)) {
        $BaseRef = "$Remote/master"
    }

    $gitVersion = (Invoke-Git -Arguments @('--version')).Output[0]
    $insideWorkTree = ((Invoke-Git -Arguments @('rev-parse', '--is-inside-work-tree')).Output[0]).Trim()
    if ($insideWorkTree -ne 'true') {
        throw 'Current directory is not inside a Git work tree.'
    }

    $repoRoot = ((Invoke-Git -Arguments @('rev-parse', '--show-toplevel')).Output[0]).Trim()
    Invoke-Git -Arguments @('rev-parse', '--verify', "$BaseRef^{commit}") | Out-Null

    $format = '%(refname:short)%09%(committerdate:short)%09%(committerdate:unix)%09%(objectname:short)%09%(subject)'
    $refRoot = "refs/remotes/$Remote"
    $refLines = (Invoke-Git -Arguments @('for-each-ref', "--format=$format", $refRoot)).Output
    if ($refLines.Count -eq 0) {
        throw "No remote-tracking refs found under $refRoot."
    }

    $mergedLines = (Invoke-Git -Arguments @('branch', '-r', '--merged', $BaseRef, '--format=%(refname:short)')).Output
    $mergedSet = @{}
    foreach ($line in $mergedLines) {
        $name = ([string]$line).Trim()
        if ($name.Length -gt 0) { $mergedSet[$name] = $true }
    }

    $epochStart = [datetime]'1970-01-01T00:00:00Z'
    $nowUnix = [int64][math]::Floor(((Get-Date).ToUniversalTime() - $epochStart).TotalSeconds)
    $remotePrefixText = $Remote + '/'
    $records = New-Object System.Collections.Generic.List[object]

    foreach ($line in $refLines) {
        if ([string]::IsNullOrWhiteSpace([string]$line)) { continue }
        $parts = ([string]$line) -split "`t", 5
        if ($parts.Count -lt 4) {
            throw "Unable to parse git for-each-ref row: $line"
        }

        $shortName = $parts[0]
        if (-not $shortName.StartsWith($remotePrefixText, [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $localName = $shortName.Substring($remotePrefixText.Length)
        if ($localName -eq 'HEAD') { continue }

        [int64]$commitUnix = 0
        if (-not [int64]::TryParse($parts[2], [ref]$commitUnix)) {
            $commitUnix = 0
        }
        $ageDays = 0
        if ($commitUnix -gt 0) {
            $ageDays = [int][math]::Floor(($nowUnix - $commitUnix) / 86400)
            if ($ageDays -lt 0) { $ageDays = 0 }
        }

        $commitDate = $parts[1]
        $shortHash = $parts[3]
        $subject = ''
        if ($parts.Count -ge 5) { $subject = $parts[4] }

        $isProtected = Test-ProtectedBranch -LocalName $localName -RemoteName $Remote -Prefixes $ProtectedPrefixes
        $isMerged = $mergedSet.ContainsKey($shortName)
        $isStale = ($ageDays -ge $StaleDays)
        $noOpByName = ($localName -match '(?i)(^|[\/_-])no[-_]?op($|[\/_-])')
        $noOpBySubject = ($subject -match '(?i)\bno[-_ ]?op\b')
        $hasNumericSuffix = ($localName -match '(?i)([-/]\d{6,}$|[-/]\d{6,}-[0-9a-f]{6,}$|^jules-\d{8,}([-/.]|$))')
        $hasLargeNumericToken = ($localName -match '(^|[-/])\d{8,}($|[-/])')

        $noOpByDiff = $false
        if (-not $NoDiffCheck -and -not $isProtected) {
            $diffSpec = "$BaseRef...$shortName"
            $diffResult = Invoke-Git -Arguments @('diff', '--quiet', $diffSpec, '--') -AllowedExitCodes @(0, 1)
            $noOpByDiff = ($diffResult.ExitCode -eq 0)
        }

        $reasons = New-Object System.Collections.Generic.List[string]
        if ($isProtected) { $reasons.Add('protected_prefix') | Out-Null }
        if ($isMerged) { $reasons.Add("merged_into_$BaseRef") | Out-Null }
        if ($isStale) { $reasons.Add("stale_${ageDays}d") | Out-Null }
        if ($noOpByName) { $reasons.Add('no_op_name') | Out-Null }
        if ($noOpBySubject) { $reasons.Add('no_op_subject') | Out-Null }
        if ($noOpByDiff) { $reasons.Add('no_op_diff') | Out-Null }
        if ($hasNumericSuffix) { $reasons.Add('numeric_suffix') | Out-Null }
        elseif ($hasLargeNumericToken) { $reasons.Add('large_numeric_token') | Out-Null }

        $isNoOp = ($noOpByName -or $noOpBySubject -or $noOpByDiff)
        $confidence = 'none'
        if ($isProtected) {
            $confidence = 'protected'
        }
        elseif ($isMerged -or $isNoOp) {
            $confidence = 'high'
        }
        elseif ($isStale -and ($hasNumericSuffix -or $hasLargeNumericToken)) {
            $confidence = 'medium'
        }
        elseif ($isStale -or $hasNumericSuffix -or $hasLargeNumericToken) {
            $confidence = 'low'
        }

        $candidate = (-not $isProtected -and $confidence -ne 'none')
        $suggestion = ''
        if ($candidate) {
            $suggestion = 'git push ' + (Quote-PowerShellArgument -Value $Remote) + ' --delete ' + (Quote-PowerShellArgument -Value $localName)
        }

        $records.Add([PSCustomObject]@{
                Branch        = $shortName
                LocalName     = $localName
                Date          = $commitDate
                AgeDays       = $ageDays
                Hash          = $shortHash
                Subject       = $subject
                Protected     = $isProtected
                Merged        = $isMerged
                Stale         = $isStale
                NoOp          = $isNoOp
                Numeric       = ($hasNumericSuffix -or $hasLargeNumericToken)
                Confidence    = $confidence
                Candidate     = $candidate
                Reasons       = @($reasons)
                Suggestion    = $suggestion
                ConfidenceKey = (Get-ConfidenceRank -Confidence $confidence)
            }) | Out-Null
    }

    $candidateRecords = @($records | Where-Object { $_.Candidate } | Sort-Object ConfidenceKey, @{ Expression = 'AgeDays'; Descending = $true }, Branch)
    $protectedRecords = @($records | Where-Object { $_.Protected } | Sort-Object Branch)
    $keepRecords = @($records | Where-Object { -not $_.Candidate -and -not $_.Protected } | Sort-Object Branch)

    $printedCandidates = $candidateRecords
    if ($Limit -gt 0) {
        $printedCandidates = @($candidateRecords | Select-Object -First $Limit)
    }

    $highCount = @($candidateRecords | Where-Object { $_.Confidence -eq 'high' }).Count
    $mediumCount = @($candidateRecords | Where-Object { $_.Confidence -eq 'medium' }).Count
    $lowCount = @($candidateRecords | Where-Object { $_.Confidence -eq 'low' }).Count
    $mergedCount = @($records | Where-Object { $_.Merged }).Count
    $staleCount = @($records | Where-Object { $_.Stale }).Count
    $noOpCount = @($records | Where-Object { $_.NoOp }).Count
    $numericCount = @($records | Where-Object { $_.Numeric }).Count

    Write-Output ("INFO git={0} repo={1} remote={2} base={3} stale_days={4} diff_check={5}" -f `
            (Format-LogValue $gitVersion), (Format-LogValue $repoRoot), (Format-LogValue $Remote), `
            (Format-LogValue $BaseRef), $StaleDays, ([bool](-not $NoDiffCheck)))
    Write-Output ("SUMMARY total={0} candidates={1} high={2} medium={3} low={4} protected={5} merged={6} stale={7} noop={8} numeric={9}" -f `
            $records.Count, $candidateRecords.Count, $highCount, $mediumCount, $lowCount, $protectedRecords.Count, `
            $mergedCount, $staleCount, $noOpCount, $numericCount)

    foreach ($record in $printedCandidates) {
        Write-Output ("CANDIDATE confidence={0} branch={1} age_days={2} date={3} hash={4} reasons={5} suggestion={6} subject={7}" -f `
                $record.Confidence, (Format-LogValue $record.Branch), $record.AgeDays, (Format-LogValue $record.Date), `
                (Format-LogValue $record.Hash), (Format-LogValue ($record.Reasons -join ',')), `
                (Format-LogValue $record.Suggestion), (Format-LogValue $record.Subject))
    }

    foreach ($record in $protectedRecords) {
        Write-Output ("PROTECTED branch={0} age_days={1} date={2} reasons={3} subject={4}" -f `
                (Format-LogValue $record.Branch), $record.AgeDays, (Format-LogValue $record.Date), `
                (Format-LogValue ($record.Reasons -join ',')), (Format-LogValue $record.Subject))
    }

    if ($ShowAll) {
        foreach ($record in $keepRecords) {
            Write-Output ("KEEP branch={0} age_days={1} date={2} reasons={3} subject={4}" -f `
                    (Format-LogValue $record.Branch), $record.AgeDays, (Format-LogValue $record.Date), `
                    (Format-LogValue ($record.Reasons -join ',')), (Format-LogValue $record.Subject))
        }
    }

    if ($Limit -gt 0 -and $candidateRecords.Count -gt $printedCandidates.Count) {
        Write-Output ("INFO candidates_truncated=true printed={0} total_candidates={1}" -f $printedCandidates.Count, $candidateRecords.Count)
    }

    Write-Output ("RESULT=OK candidates={0} protected={1} keep={2} note={3}" -f `
            $candidateRecords.Count, $protectedRecords.Count, $keepRecords.Count, `
            (Format-LogValue 'suggestions are not executed; review PR/open-work state before deleting remote branches'))
    exit 0
}
catch {
    Write-Output ("RESULT=ERROR message={0}" -f (Format-LogValue $_.Exception.Message))
    exit 1
}
