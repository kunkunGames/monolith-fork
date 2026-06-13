<#
.SYNOPSIS
Retention pruning for Monolith tool-invocation daily logs (Logs/yyyyMMdd folders).

.DESCRIPTION
SPEC_MonolithToolInvocationLogs.md keeps the daily JSONL logs append-only and
manually managed; this script is the manual management entry point. It removes
whole date folders that fall outside the retention window, oldest first. Only
folders named exactly yyyyMMdd are candidates — loose files and other folders
under the log root are never touched. Default is a dry run that lists what
would be deleted; pass -Execute to delete.

.PARAMETER LogRoot
Log root containing yyyyMMdd folders (default: <plugin>/Logs).

.PARAMETER KeepDays
Keep date folders newer than this many days (default 30). 0 disables the age rule.

.PARAMETER MaxTotalMB
Optional cap on the total size of all date folders. After the age rule, the
oldest remaining folders are pruned until the total fits. 0 disables the cap.

.PARAMETER Execute
Actually delete. Without it the script only reports.

.OUTPUTS
Line-oriented: KEEP / PRUNE lines plus a RESULT= token.

Exit codes:
  0  nothing to prune, or pruning completed
  2  dry run found folders to prune (re-run with -Execute)
  3  blocked: log root missing
#>
[CmdletBinding()]
param(
    [string]$LogRoot,
    [int]$KeepDays = 30,
    [double]$MaxTotalMB = 0,
    [switch]$Execute
)

$ErrorActionPreference = 'Continue'

if (-not $LogRoot) {
    $LogRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'Logs'
}
if (-not (Test-Path $LogRoot)) {
    Write-Output "RESULT=BLOCKED reason=log_root_missing path=$LogRoot"
    exit 3
}

$folders = @(Get-ChildItem -Path $LogRoot -Directory | Where-Object { $_.Name -match '^\d{8}$' } | Sort-Object Name)
if ($folders.Count -eq 0) {
    Write-Output "RESULT=OK no_date_folders=true root=$LogRoot"
    exit 0
}

function Get-FolderMB {
    param($Folder)
    $bytes = (Get-ChildItem -Path $Folder.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if (-not $bytes) { $bytes = 0 }
    return [math]::Round($bytes / 1MB, 2)
}

$cutoff = if ($KeepDays -gt 0) { (Get-Date).Date.AddDays(-$KeepDays).ToString('yyyyMMdd') } else { '' }
$prune = New-Object System.Collections.Generic.List[object]
$keep = New-Object System.Collections.Generic.List[object]
foreach ($folder in $folders) {
    if ($cutoff -and $folder.Name -lt $cutoff) { $prune.Add($folder) } else { $keep.Add($folder) }
}

if ($MaxTotalMB -gt 0) {
    $sized = @($keep | ForEach-Object { [PSCustomObject]@{ Folder = $_; MB = Get-FolderMB $_ } })
    $total = ($sized | Measure-Object MB -Sum).Sum
    foreach ($entry in $sized) {  # oldest first (already name-sorted)
        if ($total -le $MaxTotalMB) { break }
        $prune.Add($entry.Folder)
        $keep.Remove($entry.Folder) | Out-Null
        $total -= $entry.MB
    }
}

foreach ($folder in $keep) {
    Write-Output ("KEEP {0}" -f $folder.Name)
}
if ($prune.Count -eq 0) {
    Write-Output ("RESULT=OK kept={0} pruned=0 root={1}" -f $keep.Count, $LogRoot)
    exit 0
}

$prunedMB = 0.0
foreach ($folder in ($prune | Sort-Object Name)) {
    $mb = Get-FolderMB $folder
    $prunedMB += $mb
    if ($Execute) {
        Remove-Item -Path $folder.FullName -Recurse -Force -Confirm:$false
        Write-Output ("PRUNE {0} ({1} MB) deleted" -f $folder.Name, $mb)
    }
    else {
        Write-Output ("PRUNE {0} ({1} MB) dry_run" -f $folder.Name, $mb)
    }
}

if ($Execute) {
    Write-Output ("RESULT=PRUNED kept={0} pruned={1} freed_mb={2} root={3}" -f $keep.Count, $prune.Count, [math]::Round($prunedMB, 2), $LogRoot)
    exit 0
}
Write-Output ("RESULT=DRY_RUN kept={0} prunable={1} reclaimable_mb={2} (re-run with -Execute)" -f $keep.Count, $prune.Count, [math]::Round($prunedMB, 2))
exit 2
