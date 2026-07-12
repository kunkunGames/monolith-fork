param(
    [string[]]$Targets = @('Codex', 'Claude'),
    [switch]$Plan,
    [switch]$Execute,
    [switch]$ReplaceCopies,
    [switch]$ReplaceMcpConfig,
    [switch]$UseSymlink,
    [ValidateSet('Http', 'Proxy')]
    [string]$McpMode = 'Proxy',
    [string]$ProjectRoot = '',
    [string]$InstructionRoot = '',
    [string]$McpConfigPath = '',
    [switch]$ProjectMcpConfig,
    [switch]$SkipSkills,
    [switch]$SkipMcpConfig,
    [switch]$SkipInstructions
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Plan -and $Execute) {
    Write-Error 'Use either -Plan or -Execute, not both.'
}

if ($env:OS -ne 'Windows_NT') {
    Write-Error 'Monolith onboarding automation is Windows-only. Use Templates\Onboarding\Onboarding.md for manual setup elsewhere.'
}

$repoRoot = (Split-Path -Parent $PSScriptRoot)
$templatesRoot = Join-Path $repoRoot 'Templates'
$adapterRoot = Join-Path $templatesRoot 'Onboarding'
$issues = New-Object System.Collections.Generic.List[string]
$resolvedMcpProxyCommand = ''
$mcpProxyPrerequisiteReady = $false

function Add-Issue {
    param([string]$Message)
    $script:issues.Add($Message) | Out-Null
    Write-Host "CONFLICT $Message"
}

function Resolve-MonolithPath {
    param(
        [string]$Path,
        [string]$BasePath
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }
    $expanded = [System.Environment]::ExpandEnvironmentVariables($Path)
    if ($expanded.StartsWith('~')) {
        $expanded = Join-Path $HOME $expanded.Substring(1).TrimStart('\', '/')
    }
    if ([System.IO.Path]::IsPathRooted($expanded)) {
        return [System.IO.Path]::GetFullPath($expanded)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $expanded))
}

function Get-BoolProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $false }
    return [bool]$prop.Value
}

function Get-StringProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop -or $null -eq $prop.Value) { return '' }
    return [string]$prop.Value
}

function Get-StringArrayProperty {
    param(
        [pscustomobject]$Object,
        [string]$Name
    )

    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop -or $null -eq $prop.Value) { return @() }
    if ($prop.Value -is [array]) { return @($prop.Value | ForEach-Object { [string]$_ }) }
    return @([string]$prop.Value)
}

function Read-OnboardingAdapters {
    if (-not (Test-Path -LiteralPath $adapterRoot -PathType Container)) {
        Write-Error "Onboarding adapter directory does not exist: $adapterRoot"
    }

    $adapters = @{}
    foreach ($file in Get-ChildItem -LiteralPath $adapterRoot -Filter '*.json' -File | Sort-Object Name) {
        $adapter = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        $id = Get-StringProperty -Object $adapter -Name 'id'
        if ([string]::IsNullOrWhiteSpace($id)) {
            Write-Error "$($file.FullName): adapter id is required."
        }
        $adapters[$id] = $adapter
    }

    if ($adapters.Count -eq 0) {
        Write-Error "No onboarding adapters found under $adapterRoot"
    }
    return $adapters
}

function Normalize-Targets {
    param([hashtable]$Adapters)

    $values = New-Object System.Collections.Generic.List[string]
    foreach ($targetArg in $Targets) {
        foreach ($part in ([string]$targetArg -split ',')) {
            $value = $part.Trim()
            if ([string]::IsNullOrWhiteSpace($value)) { continue }
            if ($value -eq 'All') {
                foreach ($key in ($Adapters.Keys | Sort-Object)) {
                    if (-not $values.Contains($key)) {
                        $values.Add($key) | Out-Null
                    }
                }
                continue
            }

            $matched = $null
            foreach ($key in $Adapters.Keys) {
                if ($key.Equals($value, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $matched = $key
                    break
                }
            }
            if ($null -eq $matched) {
                $supported = (($Adapters.Keys + @('All')) | Sort-Object) -join ', '
                Write-Error "Unsupported onboarding target '$value'. Expected one of: $supported."
            }
            if (-not $values.Contains($matched)) {
                $values.Add($matched) | Out-Null
            }
        }
    }
    if ($values.Count -eq 0) {
        Write-Error 'No onboarding targets were selected.'
    }
    return @($values)
}

function Get-DefaultProjectRoot {
    $pluginsRoot = Split-Path -Parent $repoRoot
    if ((Split-Path -Leaf $pluginsRoot) -eq 'Plugins') {
        return (Split-Path -Parent $pluginsRoot)
    }
    return $repoRoot
}

function Get-RelativePathText {
    param(
        [string]$FromDirectory,
        [string]$ToPath
    )

    try {
        return ([System.IO.Path]::GetRelativePath($FromDirectory, $ToPath)).Replace('\', '/')
    }
    catch {
        return $ToPath
    }
}

function New-TemporarySiblingPath {
    param([string]$Path)

    $directory = Split-Path -Parent $Path
    $leaf = Split-Path -Leaf $Path
    return (Join-Path $directory ".$leaf.monolith-$([System.Guid]::NewGuid().ToString('N')).tmp")
}

function Assert-AtomicFileTarget {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) {
        Write-Error "Atomic file target is a directory: $Path"
    }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Write-Error "Atomic file target is a reparse point and will not be replaced: $Path"
    }
}

function Write-FileBytesAtomically {
    param(
        [string]$Path,
        [byte[]]$Bytes,
        [string]$BackupPath = ''
    )

    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    Assert-AtomicFileTarget -Path $Path
    if (-not [string]::IsNullOrWhiteSpace($BackupPath)) {
        $targetDirectory = [System.IO.Path]::GetFullPath((Split-Path -Parent $Path)).TrimEnd('\')
        $backupDirectory = [System.IO.Path]::GetFullPath((Split-Path -Parent $BackupPath)).TrimEnd('\')
        if (-not $targetDirectory.Equals($backupDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Error 'Atomic replacement backups must be created in the target directory.'
        }
        if (Test-Path -LiteralPath $BackupPath) {
            Write-Error "Atomic replacement backup already exists: $BackupPath"
        }
    }
    $temporaryPath = New-TemporarySiblingPath -Path $Path
    $temporaryBackupPath = ''
    try {
        [System.IO.File]::WriteAllBytes($temporaryPath, $Bytes)
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            # Windows PowerShell's .NET Framework File.Replace overload rejects
            # a null backup path. Use a same-directory ephemeral backup when the
            # caller does not request a durable one, then remove it immediately.
            $replaceBackup = if ([string]::IsNullOrWhiteSpace($BackupPath)) {
                $temporaryBackupPath = New-TemporarySiblingPath -Path $Path
                $temporaryBackupPath
            }
            else {
                $BackupPath
            }
            [System.IO.File]::Replace($temporaryPath, $Path, $replaceBackup, $false)
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction Stop
        }
        if (-not [string]::IsNullOrWhiteSpace($temporaryBackupPath) -and
            (Test-Path -LiteralPath $temporaryBackupPath)) {
            Remove-Item -LiteralPath $temporaryBackupPath -Force -ErrorAction Stop
        }
    }
}

function Write-TextFileAtomically {
    param(
        [string]$Path,
        [string]$Text,
        [string]$BackupPath = ''
    )

    # Match Windows PowerShell's `-Encoding utf8` contract: UTF-8 with BOM.
    $encoding = New-Object System.Text.UTF8Encoding($true)
    $preamble = $encoding.GetPreamble()
    $content = $encoding.GetBytes($Text)
    $bytes = New-Object byte[] ($preamble.Length + $content.Length)
    [System.Array]::Copy($preamble, 0, $bytes, 0, $preamble.Length)
    [System.Array]::Copy($content, 0, $bytes, $preamble.Length, $content.Length)
    Write-FileBytesAtomically -Path $Path -Bytes $bytes -BackupPath $BackupPath
}

function Test-ByteArraysEqual {
    param(
        [byte[]]$Left,
        [byte[]]$Right
    )

    if ($null -eq $Left -or $null -eq $Right -or $Left.Length -ne $Right.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Left.Length; $index++) {
        if ($Left[$index] -ne $Right[$index]) {
            return $false
        }
    }
    return $true
}

function Set-FileLastWriteTimeOnHandle {
    param(
        [System.IO.FileStream]$Stream,
        [datetime]$LastWriteTimeUtc
    )

    if ($null -eq ('MonolithOnboardingNativeFile' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class MonolithOnboardingNativeFile
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetFileTime(
        SafeFileHandle file,
        IntPtr creationTime,
        IntPtr lastAccessTime,
        ref long lastWriteTime);
}
'@
    }

    [long]$fileTime = $LastWriteTimeUtc.ToUniversalTime().ToFileTimeUtc()
    if (-not [MonolithOnboardingNativeFile]::SetFileTime(
        $Stream.SafeFileHandle,
        [IntPtr]::Zero,
        [IntPtr]::Zero,
        [ref]$fileTime)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Failed to restore file timestamp through the pinned rollback handle (Win32 $errorCode)."
    }
}

function New-FileSnapshots {
    param([string[]]$Paths)

    $snapshots = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($path in $Paths) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        $fullPath = [System.IO.Path]::GetFullPath($path)
        $key = $fullPath.ToUpperInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true

        Assert-AtomicFileTarget -Path $fullPath
        $exists = Test-Path -LiteralPath $fullPath -PathType Leaf
        $item = if ($exists) { Get-Item -LiteralPath $fullPath -Force } else { $null }
        $bytes = if ($exists) { [System.IO.File]::ReadAllBytes($fullPath) } else { [byte[]]@() }
        $snapshots.Add([pscustomobject]@{
            Path = $fullPath
            Exists = $exists
            Bytes = $bytes
            ExpectedExists = $exists
            ExpectedBytes = $bytes
            Attributes = if ($exists) { $item.Attributes } else { $null }
            LastWriteTimeUtc = if ($exists) { $item.LastWriteTimeUtc } else { $null }
        }) | Out-Null
    }
    return $snapshots.ToArray()
}

function Update-FileSnapshotExpectedState {
    param([pscustomobject]$Snapshot)

    if ($null -eq $Snapshot) { return }
    Assert-AtomicFileTarget -Path $Snapshot.Path
    $Snapshot.ExpectedExists = Test-Path -LiteralPath $Snapshot.Path -PathType Leaf
    $Snapshot.ExpectedBytes = if ($Snapshot.ExpectedExists) {
        [System.IO.File]::ReadAllBytes($Snapshot.Path)
    }
    else {
        [byte[]]@()
    }
}

function Restore-FileSnapshotCompareAndSwap {
    param([pscustomobject]$Snapshot)

    $originalEqualsExpected = $Snapshot.Exists -eq $Snapshot.ExpectedExists -and
        (-not $Snapshot.Exists -or
            (Test-ByteArraysEqual -Left ([byte[]]$Snapshot.Bytes) -Right ([byte[]]$Snapshot.ExpectedBytes)))
    if ($originalEqualsExpected) {
        return
    }

    if ($Snapshot.ExpectedExists) {
        if (-not (Test-Path -LiteralPath $Snapshot.Path -PathType Leaf)) {
            throw "Rollback compare-and-swap rejected a concurrent modification: $($Snapshot.Path) no longer exists."
        }
        Assert-AtomicFileTarget -Path $Snapshot.Path

        if ($Snapshot.Exists) {
            # Hold an exclusive writer/delete lock from comparison through restore.
            # Readers may continue, but no external writer can change bytes between
            # the compare and the in-place restoration.
            $stream = [System.IO.File]::Open(
                $Snapshot.Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::Read)
            try {
                $currentBytes = New-Object byte[] $stream.Length
                $offset = 0
                while ($offset -lt $currentBytes.Length) {
                    $read = $stream.Read($currentBytes, $offset, $currentBytes.Length - $offset)
                    if ($read -le 0) { break }
                    $offset += $read
                }
                if ($offset -ne $currentBytes.Length -or
                    -not (Test-ByteArraysEqual -Left $currentBytes -Right ([byte[]]$Snapshot.ExpectedBytes))) {
                    throw "Rollback compare-and-swap rejected a concurrent modification: $($Snapshot.Path) differs from the transaction-written bytes."
                }

                $stream.Position = 0
                $stream.Write([byte[]]$Snapshot.Bytes, 0, $Snapshot.Bytes.Length)
                $stream.SetLength($Snapshot.Bytes.Length)
                $stream.Flush($true)
                [System.IO.File]::SetAttributes($Snapshot.Path, $Snapshot.Attributes)
                Set-FileLastWriteTimeOnHandle -Stream $stream -LastWriteTimeUtc $Snapshot.LastWriteTimeUtc
            }
            finally {
                $stream.Dispose()
            }
            return
        }

        # The transaction created this file. Share delete but not write while
        # comparing, then mark the same open file for deletion.
        $stream = [System.IO.File]::Open(
            $Snapshot.Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            ([System.IO.FileShare]::Read -bor [System.IO.FileShare]::Delete))
        try {
            $currentBytes = New-Object byte[] $stream.Length
            $offset = 0
            while ($offset -lt $currentBytes.Length) {
                $read = $stream.Read($currentBytes, $offset, $currentBytes.Length - $offset)
                if ($read -le 0) { break }
                $offset += $read
            }
            if ($offset -ne $currentBytes.Length -or
                -not (Test-ByteArraysEqual -Left $currentBytes -Right ([byte[]]$Snapshot.ExpectedBytes))) {
                throw "Rollback compare-and-swap rejected a concurrent modification: $($Snapshot.Path) differs from the transaction-written bytes."
            }
            [System.IO.File]::Delete($Snapshot.Path)
        }
        finally {
            $stream.Dispose()
        }
        return
    }

    if (Test-Path -LiteralPath $Snapshot.Path) {
        throw "Rollback compare-and-swap rejected a concurrent modification: $($Snapshot.Path) was created after the transaction state was recorded."
    }
    if ($Snapshot.Exists) {
        $directory = Split-Path -Parent $Snapshot.Path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        $stream = [System.IO.File]::Open(
            $Snapshot.Path,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        try {
            $stream.Write([byte[]]$Snapshot.Bytes, 0, $Snapshot.Bytes.Length)
            $stream.Flush($true)
            [System.IO.File]::SetAttributes($Snapshot.Path, $Snapshot.Attributes)
            Set-FileLastWriteTimeOnHandle -Stream $stream -LastWriteTimeUtc $Snapshot.LastWriteTimeUtc
        }
        finally {
            $stream.Dispose()
        }
    }
}

function Restore-FileSnapshots {
    param([object[]]$Snapshots)

    foreach ($snapshot in $Snapshots) {
        Restore-FileSnapshotCompareAndSwap -Snapshot $snapshot
    }
}

function Invoke-SkillValidation {
    $validator = Join-Path $repoRoot 'Scripts\validate_monolith_skills.ps1'
    Write-Host "VALIDATE repository skills"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $validator
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Skill validation failed with exit code $LASTEXITCODE."
    }
}

function Invoke-SkillInstall {
    param(
        [pscustomobject[]]$SelectedAdapters
    )

    $skillAdapters = @($SelectedAdapters | Where-Object { Get-BoolProperty -Object $_ -Name 'supportsSkills' })
    if ($skillAdapters.Count -eq 0) {
        Write-Host 'SKIP skill links: selected targets do not support skills.'
        return
    }

    $targetNames = New-Object System.Collections.Generic.List[string]
    $targetSpecs = New-Object System.Collections.Generic.List[string]
    foreach ($adapter in $skillAdapters) {
        $id = Get-StringProperty -Object $adapter -Name 'id'
        $root = Get-StringProperty -Object $adapter -Name 'skillRoot'
        if ([string]::IsNullOrWhiteSpace($root)) {
            Add-Issue "$id supports skills but has no skillRoot."
            continue
        }
        $expandedRoot = Resolve-MonolithPath -Path $root -BasePath $repoRoot
        $targetNames.Add($id) | Out-Null
        $targetSpecs.Add("$id=$expandedRoot") | Out-Null
    }

    if ($targetNames.Count -eq 0) { return }

    $installer = Join-Path $repoRoot 'Scripts\install_monolith_skills.ps1'
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $installer,
        '-Targets', ($targetNames -join ','),
        '-SourceRoot', (Join-Path $repoRoot 'Skills'),
        '-TargetRootSpecs'
    )
    $args += @($targetSpecs)
    if ($Execute) { $args += '-Execute' }
    if ($ReplaceCopies) { $args += '-ReplaceCopies' }
    if ($UseSymlink) { $args += '-UseSymlink' }

    Write-Host "SKILLS $($targetNames -join ',')"
    & powershell @args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Skill installation step failed with exit code $LASTEXITCODE."
    }
}

function Get-McpServerName {
    param([pscustomobject]$Adapter)

    $name = Get-StringProperty -Object $Adapter -Name 'mcpServerName'
    if ([string]::IsNullOrWhiteSpace($name)) { return 'monolith' }
    return $name
}

function Get-McpUrl {
    param([pscustomobject]$Adapter)

    $url = Get-StringProperty -Object $Adapter -Name 'mcpUrl'
    if ([string]::IsNullOrWhiteSpace($url)) { return 'http://localhost:9316/mcp' }
    return $url
}

function Get-McpScope {
    param([pscustomobject]$Adapter)

    $scope = Get-StringProperty -Object $Adapter -Name 'mcpScope'
    if ([string]::IsNullOrWhiteSpace($scope)) { return 'user' }
    return $scope
}

function Get-FileSha256 {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-McpProxyVersion {
    param([string]$Path)

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Path
    $startInfo.Arguments = '--version'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw 'process did not start'
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'process exceeded the 10 second validation timeout'
        }
        $stdout = $stdoutTask.Result
        $null = $stderrTask.Result
        if ($process.ExitCode -ne 0) {
            throw "process exited with code $($process.ExitCode)"
        }
        if ([string]::IsNullOrWhiteSpace($stdout) -or $stdout.Length -gt 65536) {
            throw 'process returned an empty or oversized version payload'
        }
        try {
            return ($stdout | ConvertFrom-Json)
        }
        catch {
            throw 'process returned invalid JSON'
        }
    }
    finally {
        $process.Dispose()
    }
}

function Resolve-McpProxyCommand {
    param([string]$PluginRoot = $repoRoot)

    $binariesRoot = [System.IO.Path]::GetFullPath((Join-Path $PluginRoot 'Binaries')).TrimEnd('\')
    if (-not (Test-Path -LiteralPath $binariesRoot -PathType Container)) {
        throw "Monolith proxy Binaries directory does not exist: $binariesRoot"
    }
    $binariesItem = Get-Item -LiteralPath $binariesRoot -Force
    if (($binariesItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Monolith proxy Binaries directory is a reparse point: $binariesRoot"
    }

    $manifestPath = Join-Path $binariesRoot 'monolith_proxy.current.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Monolith immutable proxy manifest does not exist: $manifestPath"
    }
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if (($manifestItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Monolith immutable proxy manifest is a reparse point: $manifestPath"
    }

    $manifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
    try {
        $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
        $manifest = $strictUtf8.GetString($manifestBytes) | ConvertFrom-Json
    }
    catch {
        throw "Monolith immutable proxy manifest is not strict UTF-8 JSON: $manifestPath"
    }

    $requiredProperties = @('schema_version', 'file', 'version', 'source_hash', 'sha256', 'tool', 'runtime')
    $actualProperties = @($manifest.PSObject.Properties.Name)
    $unexpectedProperties = @($actualProperties | Where-Object { $requiredProperties -cnotcontains $_ })
    $missingProperties = @($requiredProperties | Where-Object { $actualProperties -cnotcontains $_ })
    if ($unexpectedProperties.Count -gt 0 -or $missingProperties.Count -gt 0) {
        throw 'Monolith immutable proxy manifest must contain exactly schema_version, file, version, source_hash, sha256, tool, and runtime.'
    }

    $schemaProperty = $manifest.PSObject.Properties['schema_version']
    $fileProperty = $manifest.PSObject.Properties['file']
    $versionProperty = $manifest.PSObject.Properties['version']
    $sourceHashProperty = $manifest.PSObject.Properties['source_hash']
    $shaProperty = $manifest.PSObject.Properties['sha256']
    $toolProperty = $manifest.PSObject.Properties['tool']
    $runtimeProperty = $manifest.PSObject.Properties['runtime']
    $schemaIsInteger = $null -ne $schemaProperty -and
        ($schemaProperty.Value -is [int] -or $schemaProperty.Value -is [long])
    if (-not $schemaIsInteger -or $schemaProperty.Value -ne 1) {
        throw 'Monolith immutable proxy manifest schema_version must be integer 1.'
    }
    if ($null -eq $fileProperty -or $fileProperty.Value -isnot [string]) {
        throw 'Monolith immutable proxy manifest file must be a string.'
    }
    if ($null -eq $versionProperty -or $versionProperty.Value -isnot [string] -or
        [string]::IsNullOrWhiteSpace([string]$versionProperty.Value)) {
        throw 'Monolith immutable proxy manifest version must be a non-empty string.'
    }
    if ($null -eq $sourceHashProperty -or $sourceHashProperty.Value -isnot [string]) {
        throw 'Monolith immutable proxy manifest source_hash must be a string.'
    }
    if ($null -eq $shaProperty -or $shaProperty.Value -isnot [string]) {
        throw 'Monolith immutable proxy manifest sha256 must be a string.'
    }
    if ($null -eq $toolProperty -or $toolProperty.Value -isnot [string] -or
        [string]$toolProperty.Value -cne 'monolith-proxy') {
        throw 'Monolith immutable proxy manifest tool must be exactly monolith-proxy.'
    }
    if ($null -eq $runtimeProperty -or $runtimeProperty.Value -isnot [string] -or
        [string]$runtimeProperty.Value -cne 'native-cpp') {
        throw 'Monolith immutable proxy manifest runtime must be exactly native-cpp.'
    }

    $fileName = [string]$fileProperty.Value
    $fileMatch = [regex]::Match($fileName, '^monolith_proxy-([0-9a-f]{16})\.exe$')
    if (-not $fileMatch.Success -or (Split-Path -Leaf $fileName) -cne $fileName) {
        throw 'Monolith immutable proxy manifest file must be the exact leaf monolith_proxy-<16 lowercase hex>.exe.'
    }
    $sourceHash = [string]$sourceHashProperty.Value
    if ($sourceHash -cnotmatch '^[0-9a-f]{16}$' -or $sourceHash -cne $fileMatch.Groups[1].Value) {
        throw 'Monolith immutable proxy manifest source_hash does not match its versioned filename.'
    }
    $expectedSha256 = [string]$shaProperty.Value
    if ($expectedSha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Monolith immutable proxy manifest sha256 must be 64 lowercase hexadecimal characters.'
    }

    $proxyPath = [System.IO.Path]::GetFullPath((Join-Path $binariesRoot $fileName))
    $proxyParent = [System.IO.Path]::GetFullPath((Split-Path -Parent $proxyPath)).TrimEnd('\')
    if (-not $proxyParent.Equals($binariesRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Monolith immutable proxy target escapes the plugin Binaries directory.'
    }
    if (-not (Test-Path -LiteralPath $proxyPath -PathType Leaf)) {
        throw "Monolith immutable proxy target does not exist: $proxyPath"
    }
    $proxyItem = Get-Item -LiteralPath $proxyPath -Force
    if ($proxyItem.PSIsContainer -or
        ($proxyItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Monolith immutable proxy target must be a regular non-reparse file: $proxyPath"
    }

    $actualSha256 = Get-FileSha256 -Path $proxyPath
    if ($actualSha256 -cne $expectedSha256) {
        throw 'Monolith immutable proxy target SHA-256 does not match the manifest.'
    }

    try {
        $version = Get-McpProxyVersion -Path $proxyPath
    }
    catch {
        throw "Monolith immutable proxy target --version validation failed: $($_.Exception.Message)"
    }
    $versionSourceHashProperty = $version.PSObject.Properties['source_hash']
    $versionValueProperty = $version.PSObject.Properties['version']
    $versionToolProperty = $version.PSObject.Properties['tool']
    $versionRuntimeProperty = $version.PSObject.Properties['runtime']
    if ($null -eq $versionSourceHashProperty -or
        $versionSourceHashProperty.Value -isnot [string] -or
        [string]$versionSourceHashProperty.Value -cne $sourceHash) {
        throw 'Monolith immutable proxy target --version source_hash does not match the manifest.'
    }
    if ($null -eq $versionValueProperty -or $versionValueProperty.Value -isnot [string] -or
        [string]$versionValueProperty.Value -cne [string]$versionProperty.Value) {
        throw 'Monolith immutable proxy target --version version does not match the manifest.'
    }
    if ($null -eq $versionToolProperty -or $versionToolProperty.Value -isnot [string] -or
        [string]$versionToolProperty.Value -cne 'monolith-proxy') {
        throw 'Monolith immutable proxy target --version tool must be exactly monolith-proxy.'
    }
    if ($null -eq $versionRuntimeProperty -or $versionRuntimeProperty.Value -isnot [string] -or
        [string]$versionRuntimeProperty.Value -cne 'native-cpp') {
        throw 'Monolith immutable proxy target --version runtime must be exactly native-cpp.'
    }

    # Detect manifest or executable replacement during validation before
    # publishing the command path.
    $postVersionItem = Get-Item -LiteralPath $proxyPath -Force
    if ($postVersionItem.PSIsContainer -or
        ($postVersionItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        (Get-FileSha256 -Path $proxyPath) -cne $expectedSha256) {
        throw 'Monolith immutable proxy target changed while it was being validated.'
    }
    if (-not (Test-ByteArraysEqual -Left $manifestBytes -Right ([System.IO.File]::ReadAllBytes($manifestPath)))) {
        throw 'Monolith immutable proxy manifest changed while it was being validated.'
    }
    return $proxyPath
}

function Get-McpProxyCommand {
    if ([string]::IsNullOrWhiteSpace($script:resolvedMcpProxyCommand)) {
        $script:resolvedMcpProxyCommand = Resolve-McpProxyCommand
    }
    return $script:resolvedMcpProxyCommand
}

function Get-McpClientConfigPath {
    param([pscustomobject]$Adapter)

    $kind = Get-StringProperty -Object $Adapter -Name 'mcpConfigKind'
    switch ($kind) {
        'codex-cli' {
            $codexHome = if ([string]::IsNullOrWhiteSpace($env:CODEX_HOME)) {
                Join-Path $HOME '.codex'
            }
            else {
                Resolve-MonolithPath -Path $env:CODEX_HOME -BasePath $HOME
            }
            return (Join-Path $codexHome 'config.toml')
        }
        'claude-cli' {
            if (-not [string]::IsNullOrWhiteSpace($env:CLAUDE_CONFIG_DIR)) {
                if ($Execute) {
                    Write-Error 'Transactional Claude MCP onboarding cannot prove the user-config file location while CLAUDE_CONFIG_DIR is set. Use -ProjectMcpConfig or unset the override for this operation.'
                }
                Write-Host 'PREREQ Execute mode requires CLAUDE_CONFIG_DIR to be unset for transactional global Claude config; project config remains supported.'
                return ''
            }
            return (Join-Path $HOME '.claude.json')
        }
        default { return '' }
    }
}

function Confirm-McpProxyPrerequisite {
    param([pscustomobject[]]$SelectedAdapters)

    if ($SkipMcpConfig -or $McpMode -ne 'Proxy') {
        return
    }

    $mcpAdapters = @($SelectedAdapters | Where-Object {
        Get-BoolProperty -Object $_ -Name 'supportsMcpConfig'
    })
    if ($mcpAdapters.Count -eq 0) {
        return
    }

    $buildScript = Join-Path $repoRoot 'Tools\MonolithProxy\build.bat'
    $manifestPath = Join-Path $repoRoot 'Binaries\monolith_proxy.current.json'
    try {
        $script:resolvedMcpProxyCommand = Resolve-McpProxyCommand
        $script:mcpProxyPrerequisiteReady = $true
        Write-Host "PREREQ Immutable proxy manifest: $manifestPath"
        Write-Host "PREREQ Validated immutable proxy executable: $script:resolvedMcpProxyCommand"
        return
    }
    catch {
        $script:mcpProxyPrerequisiteReady = $false
        $failure = $_.Exception.Message
        if (-not $Execute) {
            Add-Issue "Immutable proxy validation failed: $failure"
            Write-Host "PREREQ Expected authoritative manifest: $manifestPath"
            Write-Host "PREREQ Build a source checkout with: cmd /c `"$buildScript`""
            Write-Host 'PREREQ Alternatively, install a packaged Monolith release containing a valid immutable proxy manifest and image.'
            return
        }

        Write-Error @"
Cannot configure Monolith MCP in Proxy mode because immutable proxy validation failed:
  $failure

No onboarding mutation was attempted. The authoritative manifest is:
  $manifestPath

Build and publish a source checkout proxy first:
  cmd /c "$buildScript"

Alternatively, install a packaged Monolith release containing a valid manifest and immutable proxy image, then rerun onboarding with -Execute.
"@
    }
}

function Test-McpOutputMatches {
    param(
        [string]$Output,
        [pscustomobject]$Adapter
    )

    if ($McpMode -eq 'Http') {
        return ($Output -match [regex]::Escape((Get-McpUrl -Adapter $Adapter)))
    }

    $commandMatches = [regex]::Matches(
        $Output,
        '(?im)^\s*command\s*:\s*(.*?)\s*$')
    if ($commandMatches.Count -ne 1) {
        return $false
    }

    $reportedCommand = $commandMatches[0].Groups[1].Value.Trim()
    if ($reportedCommand.Length -ge 2 -and
        (($reportedCommand[0] -eq '"' -and $reportedCommand[$reportedCommand.Length - 1] -eq '"') -or
         ($reportedCommand[0] -eq "'" -and $reportedCommand[$reportedCommand.Length - 1] -eq "'"))) {
        $reportedCommand = $reportedCommand.Substring(1, $reportedCommand.Length - 2)
    }
    if (-not [System.IO.Path]::IsPathRooted($reportedCommand)) {
        return $false
    }

    try {
        $reportedPath = [System.IO.Path]::GetFullPath($reportedCommand)
        $expectedPath = [System.IO.Path]::GetFullPath((Get-McpProxyCommand))
    }
    catch {
        return $false
    }
    return $reportedPath.Equals($expectedPath, [System.StringComparison]::OrdinalIgnoreCase)
}

function Format-CommandLine {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    $parts = @($Command) + @($Arguments | ForEach-Object {
        if ($_ -match '\s') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    })
    return ($parts -join ' ')
}

function Invoke-OnboardingCommand {
    param(
        [string]$Command,
        [string[]]$Arguments,
        [pscustomobject]$ConfigSnapshot = $null
    )

    $display = Format-CommandLine -Command $Command -Arguments $Arguments
    if (-not $Execute) {
        Write-Host "DRYRUN $display"
        return
    }

    $commandExitCode = 0
    try {
        & $Command @Arguments
        $commandExitCode = $LASTEXITCODE
    }
    finally {
        if ($null -ne $ConfigSnapshot) {
            Update-FileSnapshotExpectedState -Snapshot $ConfigSnapshot
        }
    }
    if ($commandExitCode -ne 0) {
        Write-Error "Command failed with exit code $commandExitCode`: $display"
    }
}

function Install-CodexMcpConfig {
    param(
        [pscustomobject]$Adapter,
        [pscustomobject]$ConfigSnapshot = $null
    )

    if ($null -eq (Get-Command codex -ErrorAction SilentlyContinue)) {
        Add-Issue 'Codex MCP config requested, but codex CLI is not available.'
        return
    }

    $serverName = Get-McpServerName -Adapter $Adapter
    $getOutput = & codex mcp get $serverName 2>&1
    $exists = ($LASTEXITCODE -eq 0)
    $outputText = ($getOutput | Out-String)

    if ($exists -and (Test-McpOutputMatches -Output $outputText -Adapter $Adapter)) {
        Write-Host "SKIP Codex MCP config already matches '$serverName'."
        return
    }

    $addArgs = if ($McpMode -eq 'Http') {
        @('mcp', 'add', $serverName, '--url', (Get-McpUrl -Adapter $Adapter))
    }
    else {
        @('mcp', 'add', $serverName, '--', (Get-McpProxyCommand))
    }

    if ($exists) {
        $message = "Codex MCP config '$serverName' exists and differs. Use -ReplaceMcpConfig to replace it through codex mcp remove/add."
        if (-not $ReplaceMcpConfig) {
            Add-Issue $message
            return
        }

        Invoke-OnboardingCommand -Command 'codex' -Arguments @('mcp', 'remove', $serverName) -ConfigSnapshot $ConfigSnapshot
    }

    Invoke-OnboardingCommand -Command 'codex' -Arguments $addArgs -ConfigSnapshot $ConfigSnapshot
    if ($Execute) {
        $verifyOutput = & codex mcp get $serverName 2>&1
        $verifyExitCode = $LASTEXITCODE
        $verifyText = ($verifyOutput | Out-String)
        if ($verifyExitCode -ne 0 -or -not (Test-McpOutputMatches -Output $verifyText -Adapter $Adapter)) {
            Write-Error "Codex MCP config verification failed after updating '$serverName'."
        }
    }
}

function Install-ClaudeMcpConfig {
    param(
        [pscustomobject]$Adapter,
        [pscustomobject]$ConfigSnapshot = $null
    )

    if ($null -eq (Get-Command claude -ErrorAction SilentlyContinue)) {
        Add-Issue 'Claude MCP config requested, but claude CLI is not available.'
        return
    }

    $serverName = Get-McpServerName -Adapter $Adapter
    $scope = Get-McpScope -Adapter $Adapter
    $getOutput = & claude mcp get $serverName 2>&1
    $exists = ($LASTEXITCODE -eq 0)
    $outputText = ($getOutput | Out-String)

    if ($exists -and (Test-McpOutputMatches -Output $outputText -Adapter $Adapter) -and $outputText -match "Scope:\s+User config") {
        Write-Host "SKIP Claude MCP config already matches '$serverName' in user scope."
        return
    }

    $addArgs = if ($McpMode -eq 'Http') {
        @('mcp', 'add', '--scope', $scope, '--transport', 'http', $serverName, (Get-McpUrl -Adapter $Adapter))
    }
    else {
        @('mcp', 'add', '--scope', $scope, $serverName, '--', (Get-McpProxyCommand))
    }

    if ($exists) {
        $message = "Claude MCP config '$serverName' exists and differs. Use -ReplaceMcpConfig to replace it through claude mcp remove/add."
        if (-not $ReplaceMcpConfig) {
            Add-Issue $message
            return
        }

        Invoke-OnboardingCommand -Command 'claude' -Arguments @('mcp', 'remove', $serverName, '-s', $scope) -ConfigSnapshot $ConfigSnapshot
    }

    Invoke-OnboardingCommand -Command 'claude' -Arguments $addArgs -ConfigSnapshot $ConfigSnapshot
    if ($Execute) {
        $verifyOutput = & claude mcp get $serverName 2>&1
        $verifyExitCode = $LASTEXITCODE
        $verifyText = ($verifyOutput | Out-String)
        if ($verifyExitCode -ne 0 -or
            -not (Test-McpOutputMatches -Output $verifyText -Adapter $Adapter) -or
            $verifyText -notmatch 'Scope:\s+User config') {
            Write-Error "Claude MCP config verification failed after updating '$serverName'."
        }
    }
}

function Install-ProjectMcpConfig {
    param(
        [string]$ResolvedProjectRoot
    )

    $templateName = if ($McpMode -eq 'Proxy') { '.mcp.json.proxy.example' } else { '.mcp.json.example' }
    $templatePath = Join-Path $templatesRoot $templateName
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
        Write-Error "MCP template does not exist: $templatePath"
    }

    $targetPath = if ([string]::IsNullOrWhiteSpace($McpConfigPath)) {
        Join-Path $ResolvedProjectRoot '.mcp.json'
    }
    else {
        Resolve-MonolithPath -Path $McpConfigPath -BasePath $ResolvedProjectRoot
    }

    $templateText = Get-Content -LiteralPath $templatePath -Raw
    if ($McpMode -eq 'Proxy') {
        try {
            $templateJson = $templateText | ConvertFrom-Json
            $mcpServersProperty = $templateJson.PSObject.Properties['mcpServers']
            $serverProperty = if ($null -ne $mcpServersProperty) {
                $mcpServersProperty.Value.PSObject.Properties['monolith']
            }
            else {
                $null
            }
            $commandProperty = if ($null -ne $serverProperty) {
                $serverProperty.Value.PSObject.Properties['command']
            }
            else {
                $null
            }
            if ($null -eq $commandProperty -or $commandProperty.Value -isnot [string]) {
                throw 'proxy template is missing mcpServers.monolith.command'
            }
            $commandProperty.Value = Get-McpProxyCommand
            $templateText = $templateJson | ConvertTo-Json -Depth 10
        }
        catch {
            Write-Error "Cannot render immutable proxy project MCP config from $templatePath`: $($_.Exception.Message)"
        }
    }
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        if ($Execute) {
            Write-TextFileAtomically -Path $targetPath -Text $templateText
            Write-Host "CREATE MCP config $targetPath from $templateName"
        }
        else {
            Write-Host "DRYRUN create MCP config $targetPath from $templateName"
        }
        return
    }

    $existingText = Get-Content -LiteralPath $targetPath -Raw
    if ($existingText -eq $templateText) {
        Write-Host "SKIP MCP config already matches $targetPath"
        return
    }

    if (-not $ReplaceMcpConfig) {
        $message = "MCP config exists and differs: $targetPath. Use -ReplaceMcpConfig to back it up and replace it."
        Add-Issue $message
        return
    }

    $backupPath = "$targetPath.backup-$(Get-Date -Format 'yyyyMMdd-HHmmssfff')"
    if ($Execute) {
        Write-TextFileAtomically -Path $targetPath -Text $templateText -BackupPath $backupPath
        Write-Host "REPLACE MCP config $targetPath, backup $backupPath"
    }
    else {
        Write-Host "DRYRUN replace MCP config $targetPath, backup $backupPath"
    }
}

function Install-McpConfig {
    param(
        [pscustomobject[]]$SelectedAdapters,
        [string]$ResolvedProjectRoot
    )

    $mcpAdapters = @($SelectedAdapters | Where-Object { Get-BoolProperty -Object $_ -Name 'supportsMcpConfig' })
    if ($mcpAdapters.Count -eq 0) {
        Write-Host 'SKIP MCP config: selected targets do not consume MCP config.'
        return
    }

    if ($McpMode -eq 'Proxy' -and -not $script:mcpProxyPrerequisiteReady) {
        Write-Host 'SKIP MCP config: immutable proxy manifest validation did not pass in Plan mode.'
        return
    }

    if ($ProjectMcpConfig -or -not [string]::IsNullOrWhiteSpace($McpConfigPath)) {
        Install-ProjectMcpConfig -ResolvedProjectRoot $ResolvedProjectRoot
        return
    }

    $configPaths = @($mcpAdapters | ForEach-Object { Get-McpClientConfigPath -Adapter $_ } | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    })
    $snapshots = if ($Execute) { @(New-FileSnapshots -Paths $configPaths) } else { @() }
    $snapshotsByPath = @{}
    foreach ($snapshot in $snapshots) {
        $snapshotsByPath[[System.IO.Path]::GetFullPath($snapshot.Path).ToUpperInvariant()] = $snapshot
    }
    $initialIssueCount = $issues.Count

    try {
        foreach ($adapter in $mcpAdapters) {
            $kind = Get-StringProperty -Object $adapter -Name 'mcpConfigKind'
            $id = Get-StringProperty -Object $adapter -Name 'id'
            $adapterConfigPath = Get-McpClientConfigPath -Adapter $adapter
            $configSnapshot = $null
            if ($Execute -and -not [string]::IsNullOrWhiteSpace($adapterConfigPath)) {
                $snapshotKey = [System.IO.Path]::GetFullPath($adapterConfigPath).ToUpperInvariant()
                if ($snapshotsByPath.ContainsKey($snapshotKey)) {
                    $configSnapshot = $snapshotsByPath[$snapshotKey]
                }
            }
            switch ($kind) {
                'codex-cli' { Install-CodexMcpConfig -Adapter $adapter -ConfigSnapshot $configSnapshot }
                'claude-cli' { Install-ClaudeMcpConfig -Adapter $adapter -ConfigSnapshot $configSnapshot }
                default {
                    Write-Host "SKIP MCP config for $id`: no global MCP config handler. Use -ProjectMcpConfig for .mcp.json clients."
                }
            }
        }

        if ($Execute -and $issues.Count -gt $initialIssueCount) {
            Write-Error 'Global MCP configuration transaction encountered a conflict after it began.'
        }
    }
    catch {
        $originalError = $_
        if ($Execute) {
            try {
                Restore-FileSnapshots -Snapshots $snapshots
                Write-Host 'ROLLBACK restored the selected global MCP client configuration files.'
            }
            catch {
                throw "Global MCP configuration failed and rollback also failed: $($_.Exception.Message). Original failure: $($originalError.Exception.Message)"
            }
        }
        throw $originalError
    }
}

function Get-InstructionBlock {
    param([string]$ResolvedInstructionRoot)

    $onboardingPath = Join-Path $templatesRoot 'Onboarding\Onboarding.md'
    $onboardingRef = Get-RelativePathText -FromDirectory $ResolvedInstructionRoot -ToPath $onboardingPath
    $pluginRef = Get-RelativePathText -FromDirectory $ResolvedInstructionRoot -ToPath $repoRoot

    return @"
<!-- BEGIN MONOLITH ONBOARDING -->
## Monolith MCP
This project uses Monolith MCP from $pluginRef.

- Use monolith_status() before editor-backed actions.
- Use monolith_find("<task>") for routing and focused monolith_discover({ "namespace": "<ns>", "action": "<action>", "mode": "schema" }) for exact parameters.
- Use Binaries\monolith_query.exe only as the read-only offline fallback when MCP/editor access is down.
- Do not copy Monolith skills into global skill roots; link them with Scripts\onboard_monolith.ps1 or Scripts\install_monolith_skills.ps1.
- See $onboardingRef for setup, verification, and recovery steps.
<!-- END MONOLITH ONBOARDING -->
"@
}

function Update-InstructionFile {
    param(
        [string]$Path,
        [string]$Block
    )

    $begin = '<!-- BEGIN MONOLITH ONBOARDING -->'
    $end = '<!-- END MONOLITH ONBOARDING -->'

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($Execute) {
            Write-TextFileAtomically -Path $Path -Text ($Block + [Environment]::NewLine)
            Write-Host "CREATE instruction file $Path"
        }
        else {
            Write-Host "DRYRUN create instruction file $Path"
        }
        return
    }

    $content = Get-Content -LiteralPath $Path -Raw
    if ($content.Contains($begin) -and $content.Contains($end)) {
        $escapedBegin = [regex]::Escape($begin)
        $escapedEnd = [regex]::Escape($end)
        $newContent = [regex]::Replace($content, "$escapedBegin.*?$escapedEnd", [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $Block }, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if ($newContent -eq $content) {
            Write-Host "SKIP instruction block unchanged $Path"
            return
        }
        if ($Execute) {
            Write-TextFileAtomically -Path $Path -Text $newContent
            Write-Host "UPDATE instruction block $Path"
        }
        else {
            Write-Host "DRYRUN update instruction block $Path"
        }
        return
    }

    if ($content -match '(?m)^##\s+\d*\.?\s*Monolith Onboarding and Skills' -or
        ($content -match 'Onboarding\.md' -and $content -match 'install_monolith_skills\.ps1|onboard_monolith\.ps1')) {
        Write-Host "SKIP instruction file already contains Monolith onboarding guidance $Path"
        return
    }

    if ($Execute) {
        $newContent = $content + [Environment]::NewLine + $Block + [Environment]::NewLine
        Write-TextFileAtomically -Path $Path -Text $newContent
        Write-Host "APPEND instruction block $Path"
    }
    else {
        Write-Host "DRYRUN append instruction block $Path"
    }
}

function Update-Instructions {
    param(
        [pscustomobject[]]$SelectedAdapters,
        [string]$ResolvedInstructionRoot
    )

    $files = New-Object System.Collections.Generic.List[string]
    foreach ($adapter in $SelectedAdapters) {
        if (-not (Get-BoolProperty -Object $adapter -Name 'supportsInstructions')) { continue }
        foreach ($file in (Get-StringArrayProperty -Object $adapter -Name 'instructionFiles')) {
            if ([string]::IsNullOrWhiteSpace($file)) { continue }
            if (-not $files.Contains($file)) {
                $files.Add($file) | Out-Null
            }
        }
    }

    if ($files.Count -eq 0) {
        Write-Host 'SKIP instructions: selected targets do not use project instruction files.'
        return
    }

    $block = Get-InstructionBlock -ResolvedInstructionRoot $ResolvedInstructionRoot
    foreach ($file in $files) {
        $path = Resolve-MonolithPath -Path $file -BasePath $ResolvedInstructionRoot
        Update-InstructionFile -Path $path -Block $block
    }
}

$resolvedProjectRoot = if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    Get-DefaultProjectRoot
}
else {
    Resolve-MonolithPath -Path $ProjectRoot -BasePath $repoRoot
}

$resolvedInstructionRoot = if ([string]::IsNullOrWhiteSpace($InstructionRoot)) {
    $repoRoot
}
else {
    Resolve-MonolithPath -Path $InstructionRoot -BasePath $repoRoot
}

$adapters = Read-OnboardingAdapters
$targetIds = Normalize-Targets -Adapters $adapters
$selectedAdapters = @($targetIds | ForEach-Object { $adapters[$_] })
$mode = if ($Execute) { 'EXECUTE' } else { 'PLAN' }

Write-Host "Monolith onboarding: mode=$mode targets=$($targetIds -join ',') mcp=$McpMode"
Write-Host "RepoRoot: $repoRoot"
Write-Host "ProjectRoot: $resolvedProjectRoot"
Write-Host "InstructionRoot: $resolvedInstructionRoot"

Confirm-McpProxyPrerequisite -SelectedAdapters $selectedAdapters

if ($SkipSkills) {
    Write-Host 'SKIP repository skill validation and skill links by request.'
}
else {
    Invoke-SkillValidation
    Invoke-SkillInstall -SelectedAdapters $selectedAdapters
}

if ($SkipMcpConfig) {
    Write-Host 'SKIP MCP config by request.'
}
else {
    Install-McpConfig -SelectedAdapters $selectedAdapters -ResolvedProjectRoot $resolvedProjectRoot
}

if ($SkipInstructions) {
    Write-Host 'SKIP instructions by request.'
}
else {
    Update-Instructions -SelectedAdapters $selectedAdapters -ResolvedInstructionRoot $resolvedInstructionRoot
}

if ($issues.Count -gt 0) {
    Write-Host "Monolith onboarding completed with $($issues.Count) conflict(s)."
    if ($Execute) {
        exit 1
    }
}
else {
    Write-Host 'Monolith onboarding completed without conflicts.'
}
