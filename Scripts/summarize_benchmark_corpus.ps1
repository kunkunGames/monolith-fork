# Monolith Benchmark Corpus Size Reporter
#
# Reports the largest tracked or working-tree benchmark artifacts so release hygiene
# reviews can spot generated corpora before they silently bloat runtime packages.

param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$BenchmarkPath = "Benchmarks",
    [int]$Top = 30,
    [decimal]$MinMB = 0,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path $Root).Path.TrimEnd('\', '/')
$benchmarkRoot = Join-Path $rootPath $BenchmarkPath
if (-not (Test-Path $benchmarkRoot)) {
    throw "Benchmark path not found: $benchmarkRoot"
}

$generatedCorpusPatterns = @(
    "Benchmarks/AssetEditing/tasks.jsonl",
    "Benchmarks/AssetEditing/manifest.json",
    "Benchmarks/AssetEditing/asset_types.json",
    "Benchmarks/AssetEditing/testsets/*",
    "Benchmarks/AssetEditing/*/index.json",
    "Benchmarks/AssetEditing/*/tasks.jsonl",
    "Benchmarks/AssetEditing/*/testcases/*"
)

function Convert-ToRepoPath {
    param([string]$FullName)

    if ($FullName.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullName.Substring($rootPath.Length).TrimStart('\', '/') -replace '\\', '/'
    }
    return $FullName -replace '\\', '/'
}

function Test-GeneratedCorpusPath {
    param([string]$Path)

    foreach ($pattern in $generatedCorpusPatterns) {
        if ($Path -like $pattern) {
            return $true
        }
    }
    return $false
}

$files = Get-ChildItem -Path $benchmarkRoot -Recurse -File | ForEach-Object {
    $repoPath = Convert-ToRepoPath -FullName $_.FullName
    [PSCustomObject]@{
        Path = $repoPath
        MB = [math]::Round($_.Length / 1MB, 3)
        Bytes = $_.Length
        GeneratedCorpus = Test-GeneratedCorpusPath -Path $repoPath
    }
}

if ($MinMB -gt 0) {
    $files = $files | Where-Object { $_.MB -ge [double]$MinMB }
}

$largest = @($files | Sort-Object Bytes -Descending | Select-Object -First $Top)
$generated = @($files | Where-Object { $_.GeneratedCorpus })
$summary = [PSCustomObject]@{
    Root = $rootPath
    BenchmarkPath = (Convert-ToRepoPath -FullName $benchmarkRoot)
    FileCount = @($files).Count
    TotalMB = [math]::Round((@($files) | Measure-Object -Property Bytes -Sum).Sum / 1MB, 3)
    GeneratedCorpusFileCount = $generated.Count
    GeneratedCorpusMB = [math]::Round(($generated | Measure-Object -Property Bytes -Sum).Sum / 1MB, 3)
    Largest = $largest
}

if ($Json) {
    $summary | ConvertTo-Json -Depth 5
    exit 0
}

Write-Host "Benchmark corpus summary" -ForegroundColor Cyan
$summary | Select-Object Root, BenchmarkPath, FileCount, TotalMB, GeneratedCorpusFileCount, GeneratedCorpusMB | Format-List

Write-Host "Largest benchmark files" -ForegroundColor Cyan
$largest | Select-Object MB, GeneratedCorpus, Path | Format-Table -AutoSize
