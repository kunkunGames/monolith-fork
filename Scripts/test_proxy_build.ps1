[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $repositoryRoot 'Tools\MonolithProxy\build_proxy.bat'
$compatibilityBuildScript = Join-Path $repositoryRoot 'Tools\MonolithProxy\build.bat'
$proxySource = Join-Path $repositoryRoot 'Tools\MonolithProxy\monolith_proxy.cpp'
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ("MonolithProxyBuildTest-{0}" -f [Guid]::NewGuid().ToString('N'))

function Invoke-ProxyBuild {
    param(
        [Parameter(Mandatory)] [string] $SourceFile,
        [Parameter(Mandatory)] [string] $OutputDirectory,
        [Parameter(Mandatory)] [string] $EntryPoint,
        [string] $StagingDirectory
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $env:ComSpec
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.WorkingDirectory = Split-Path -Parent $EntryPoint
    $startInfo.Arguments = '/d /s /c ""{0}""' -f $EntryPoint.Replace('"', '""')
    $startInfo.Environment['MONOLITH_PROXY_SOURCE_FILE'] = $SourceFile
    $startInfo.Environment['MONOLITH_PROXY_OUTPUT_DIR'] = $OutputDirectory
    if ($StagingDirectory) {
        $startInfo.Environment['MONOLITH_PROXY_STAGING_DIR'] = $StagingDirectory
    }
    else {
        [void] $startInfo.Environment.Remove('MONOLITH_PROXY_STAGING_DIR')
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'Failed to start build_proxy.bat'
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()

    [pscustomobject]@{
        ExitCode = $process.ExitCode
        StdOut = $stdoutTask.GetAwaiter().GetResult()
        StdErr = $stderrTask.GetAwaiter().GetResult()
    }
}

function Assert-Condition {
    param(
        [Parameter(Mandatory)] [bool] $Condition,
        [Parameter(Mandatory)] [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $successOutput = Join-Path $testRoot 'success-output'
    $successResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $successOutput -EntryPoint $buildScript
    if ($successResult.ExitCode -ne 0) {
        throw "Expected a successful native build.`n$($successResult.StdOut)`n$($successResult.StdErr)"
    }
    $successBinary = Join-Path $successOutput 'monolith_proxy.exe'
    Assert-Condition -Condition (Test-Path -LiteralPath $successBinary -PathType Leaf) `
        -Message 'Successful build did not publish monolith_proxy.exe'
    Assert-Condition -Condition ((Get-Item -LiteralPath $successBinary).Length -gt 0) `
        -Message 'Successful build published an empty executable'

    $compatibilityOutput = Join-Path $testRoot 'compatibility-output'
    $compatibilityResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $compatibilityOutput `
        -EntryPoint $compatibilityBuildScript
    if ($compatibilityResult.ExitCode -ne 0) {
        throw "Expected build.bat to delegate successfully.`n$($compatibilityResult.StdOut)`n$($compatibilityResult.StdErr)"
    }
    $compatibilityBinary = Join-Path $compatibilityOutput 'monolith_proxy.exe'
    Assert-Condition -Condition (Test-Path -LiteralPath $compatibilityBinary -PathType Leaf) `
        -Message 'build.bat did not publish monolith_proxy.exe'
    Assert-Condition -Condition ((Get-Item -LiteralPath $compatibilityBinary).Length -eq (Get-Item -LiteralPath $successBinary).Length) `
        -Message 'build.bat did not delegate to the authoritative build output'

    $failureOutput = Join-Path $testRoot 'failure-output'
    New-Item -ItemType Directory -Path $failureOutput | Out-Null
    $protectedBinary = Join-Path $failureOutput 'monolith_proxy.exe'
    $sentinelBytes = [byte[]](0x4D, 0x4F, 0x4E, 0x4F, 0x4C, 0x49, 0x54, 0x48)
    [IO.File]::WriteAllBytes($protectedBinary, $sentinelBytes)
    $invalidSource = Join-Path $testRoot 'invalid_proxy.cpp'
    [IO.File]::WriteAllText($invalidSource, "#error Intentional proxy build regression fixture`r`n")

    $failureResult = Invoke-ProxyBuild -SourceFile $invalidSource -OutputDirectory $failureOutput -EntryPoint $buildScript
    Assert-Condition -Condition ($failureResult.ExitCode -ne 0) `
        -Message 'Compile failure incorrectly returned exit code 0'
    $protectedBytesAfterFailure = [IO.File]::ReadAllBytes($protectedBinary)
    Assert-Condition -Condition ([Convert]::ToBase64String($protectedBytesAfterFailure) -eq [Convert]::ToBase64String($sentinelBytes)) `
        -Message 'Compile failure changed the pre-existing proxy binary'
    Assert-Condition -Condition ($failureResult.StdOut -notmatch 'SUCCESS:') `
        -Message 'Compile failure printed a success message'

    $blockedOutput = Join-Path $testRoot 'blocked-output'
    [IO.File]::WriteAllText($blockedOutput, 'not a directory')
    $blockedResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $blockedOutput -EntryPoint $buildScript
    Assert-Condition -Condition ($blockedResult.ExitCode -ne 0) `
        -Message 'Invalid output directory incorrectly returned exit code 0'
    Assert-Condition -Condition ($blockedResult.StdOut -notmatch 'SUCCESS:') `
        -Message 'Invalid output directory printed a success message'

    $collisionStage = Join-Path $testRoot 'pre-existing-stage'
    New-Item -ItemType Directory -Path $collisionStage | Out-Null
    $collisionSentinel = Join-Path $collisionStage 'owned-by-another-process.txt'
    [IO.File]::WriteAllText($collisionSentinel, 'preserve me')
    $collisionResult = Invoke-ProxyBuild -SourceFile $proxySource -OutputDirectory $successOutput `
        -EntryPoint $buildScript -StagingDirectory $collisionStage
    Assert-Condition -Condition ($collisionResult.ExitCode -ne 0) `
        -Message 'Pre-existing staging directory incorrectly returned exit code 0'
    Assert-Condition -Condition (Test-Path -LiteralPath $collisionSentinel -PathType Leaf) `
        -Message 'Staging collision deleted a file the build did not own'
    Assert-Condition -Condition ($collisionResult.StdOut -notmatch 'SUCCESS:') `
        -Message 'Staging collision printed a success message'

    Write-Host 'PASS: both entry points build, failures preserve prior outputs, and cleanup removes only owned staging files.'
}
finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if ($resolvedTestRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTestRoot).StartsWith('MonolithProxyBuildTest-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
