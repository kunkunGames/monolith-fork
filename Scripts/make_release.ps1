# Monolith Release Zip Builder
# Creates a release zip with "Installed": true for Blueprint-only compatibility.
# Automatically builds with optional dependencies disabled (MONOLITH_RELEASE_BUILD=1).
#
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.10.0"
#
# What it does:
#   1. Sets MONOLITH_RELEASE_BUILD=1 (forces BA/GBA optional deps OFF in Build.cs)
#   2. Runs UBT to produce clean release binaries
#   3. Packages tracked files + binaries into a zip with Installed=true
#   4. Strips accidentally re-merged non-redistributable modules from source,
#      binaries, and the uplugin module list
#   5. Unsets env var (your next dev build auto-detects deps normally)
#
# Source users (GitHub clones) are unaffected -- Build.cs auto-detects at compile time.
#
# Non-redistributable sibling modules live outside this repo. The strip phase below
# is defense-in-depth for accidental re-merges into Plugins/Monolith/.

param(
    [Parameter(Mandatory=$true)]
    [string]$Version,
    [switch]$SkipBuild,
    # Allow releasing with a dirty working tree. DANGEROUS: WIP modifications to tracked
    # files end up in the zip because this script copies the working-tree content, not the
    # committed HEAD. Only use if you know exactly what dirty files you're shipping.
    [switch]$AllowDirtyTree,
    # Allow releasing when the hard-link import smoke cannot run (dumpbin.exe not found).
    # DANGEROUS: skips the only check that proves shipped DLLs do not hard-link optional
    # plugins (issue #30 / #71 failure mode). Real releases NEVER set this -- it exists
    # only so a dev machine without VS Build Tools can produce a non-shippable test zip.
    [switch]$AllowUnverifiedImports
)

$ErrorActionPreference = "Stop"

# --- Step 0: Refuse to release a dirty working tree ---
# This script copies tracked files from the working tree (not from HEAD), so any
# uncommitted modification to a tracked file silently ends up in the release zip.
# Bitten by this shipping v0.13.1 with WIP CommonUI refs in MonolithUI. Never again.
$PluginDir = Split-Path -Parent $PSScriptRoot
Push-Location $PluginDir
try {
    # Porcelain is empty iff working tree and index both match HEAD (no modified, staged,
    # or untracked files). Ignores files under .gitignore. Perfect clean gate.
    $dirty = git status --porcelain
    if ($dirty -and -not $AllowDirtyTree) {
        Pop-Location
        Write-Host "`n  [FAIL] Working tree is dirty. Refusing to release." -ForegroundColor Red
        Write-Host "`n  Offending files:" -ForegroundColor Red
        $dirty | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        Write-Host "`n  Options:" -ForegroundColor Cyan
        Write-Host "    - Commit or stash the changes, then re-run" -ForegroundColor Cyan
        Write-Host "    - Re-run with -AllowDirtyTree if you REALLY know what you're shipping" -ForegroundColor Cyan
        exit 1
    }
    if ($dirty -and $AllowDirtyTree) {
        Write-Host "`n  [WARN] -AllowDirtyTree set. Working tree has uncommitted changes:" -ForegroundColor Yellow
        $dirty | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        Write-Host "  These WILL be in the release zip." -ForegroundColor Yellow
    }
}
finally {
    Pop-Location
}

$ProjectDir = Split-Path -Parent (Split-Path -Parent $PluginDir)

# --- $StrippedModules: defense-in-depth against accidental sibling-plugin re-merge ---
# Sibling plugins live outside Plugins/Monolith/ at the project's Plugins/ level.
# They are naturally excluded from the release zip by `git ls-files` scope (which
# only sees files inside Plugins/Monolith/). This array exists purely as
# defense-in-depth: if someone ever accidentally re-merges sibling source back into
# Plugins/Monolith/Source/ (refactor mistake, copy-paste, etc.), the strip filter
# catches it before it ships.
#
# Auto-discover all "Monolith*" sibling folders alongside Plugins/Monolith/ -- every new
# sibling gets protected automatically without script maintenance. Excludes Monolith
# itself.
$ProjectPluginsDir = Join-Path $ProjectDir "Plugins"
$StrippedModules = @(Get-ChildItem -Path $ProjectPluginsDir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "Monolith*" -and $_.Name -ne "Monolith" } |
    Select-Object -ExpandProperty Name)
if ($StrippedModules.Count -gt 0) {
    Write-Host "  [strip-list] Auto-discovered $($StrippedModules.Count) sibling plugin(s) to defend against: $($StrippedModules -join ', ')" -ForegroundColor DarkGray
}
if ($IsMacOS) {
    $OutputZip = Join-Path $ProjectDir "Monolith-v$Version-macOS.zip"
} elseif ($IsLinux) {
    $OutputZip = Join-Path $ProjectDir "Monolith-v$Version-Linux.zip"
} else {
    $OutputZip = Join-Path $ProjectDir "Monolith-v$Version.zip"
}
$TempDir = Join-Path $env:TEMP "Monolith_Release_$Version"

# Locate UBT dynamically
$UBT = $env:UE_57_UBT
if (-not $UBT) {
    if ($env:UE_57) {
        $UBT = Join-Path $env:UE_57 "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe"
    } elseif ($env:UE_ROOT) {
        $UBT = Join-Path $env:UE_ROOT "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe"
    }
}

$UProject = Join-Path $ProjectDir "Leviathan.uproject"

Write-Host "Building Monolith v$Version release zip..." -ForegroundColor Cyan

# --- Step 1: Build with optional deps disabled ---
if (-not $SkipBuild) {
    if (-not $UBT -or -not (Test-Path $UBT)) {
        Write-Host "    [FAIL] UnrealBuildTool not found. Set UE_57, UE_ROOT, or UE_57_UBT environment variable." -ForegroundColor Red
        exit 1
    }

    Write-Host "`n  [1/4] Building release binaries (optional deps OFF)..." -ForegroundColor Yellow

    # Set env var so Build.cs files skip optional dependency detection
    $env:MONOLITH_RELEASE_BUILD = "1"
    Write-Host "    MONOLITH_RELEASE_BUILD=1 (BA/GBA/ComboGraph forced off)" -ForegroundColor DarkGray

    try {
        # Non-unity build catches missing includes and unity-only symbol collisions
        # before they reach public releases (feedback_non_unity_build_releases.md).
        & $UBT MonolithEditor Win64 Development "-Project=$UProject" -waitmutex -DisableUnity
        if ($LASTEXITCODE -ne 0) {
            throw "UBT failed with exit code $LASTEXITCODE. Is the editor closed?"
        }
        Write-Host "    Build succeeded" -ForegroundColor Green
    }
    finally {
        # Always unset -- even if build fails, don't poison future dev builds
        Remove-Item Env:\MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
        Write-Host "    MONOLITH_RELEASE_BUILD unset" -ForegroundColor DarkGray
    }
} else {
    Write-Host "`n  [1/4] Skipping build (-SkipBuild flag)" -ForegroundColor DarkGray
    Write-Host "    WARNING: Ensure you built with MONOLITH_RELEASE_BUILD=1" -ForegroundColor Red
}

# --- Step 1a: Full-unity collision gate (issue #68 defense) ---
# The -DisableUnity build above compiles every .cpp as its own translation unit.
# That catches missing includes but is STRUCTURALLY BLIND to duplicate file-local
# symbols: anonymous-namespace functions or file-static functions/vars sharing a
# name across two+ .cpp files in one module. With no concatenation they never
# collide, so -DisableUnity cannot see them. That is exactly how issue #68 shipped.
#
# End users build with DEFAULT adaptive unity. On a fresh clone the adaptive
# working set is empty, so everything concatenates (effectively full unity) and
# they hit the collision on first compile. To catch this class against the EXACT
# release configuration, we run a SECOND pass under FORCED full unity.
#
# We force full unity by temporarily flipping bUseAdaptiveUnityBuild=false in the
# UBT BuildConfiguration.xml, then ALWAYS restore the original (back it up first,
# restore in finally). We do NOT pass -DisableUnity here -- we WANT concatenation.
# We do NOT pass -Clean: flipping the adaptive flag already invalidates the
# makefile so UBT rebuilds the affected unity blobs, and a clean would wipe the
# precompiled Plugins/RLPlugin/Binaries/Win64/UnrealEditor-RLPlugin.dll (requiring
# a dv restore afterward).
#
# A whole-project full-unity build also surfaces the host project's own latent
# collisions. Those are NOT Monolith's concern -- we filter the captured log to
# Plugins\Monolith\ paths only. Only Monolith-path collision errors ship-block.
if (-not $SkipBuild) {
    Write-Host "`n  [1a] Full-unity collision gate (issue #68 defense)..." -ForegroundColor Yellow

    $BuildConfigDir = Join-Path $env:APPDATA "Unreal Engine\UnrealBuildTool"
    $BuildConfigXml = Join-Path $BuildConfigDir "BuildConfiguration.xml"
    $BuildConfigBackup = "$BuildConfigXml.monolith-release-bak"
    $HadBuildConfig = Test-Path $BuildConfigXml
    $UnityLog = Join-Path $env:TEMP "Monolith_FullUnity_$Version.log"

    # Collision error codes that full-unity concatenation surfaces but -DisableUnity
    # cannot: redefinition / multiple-definition / ambiguous-overload classes.
    #   C2084 - function already has a body
    #   C2011 - type redefinition
    #   C2086 - identifier redefinition
    #   C2027 - use of undefined type (often a downstream of conflicting decls)
    #   C2374 - redefinition / multiple initialization
    #   C2668 - ambiguous call to overloaded function
    $CollisionCodes = @("C2084", "C2011", "C2086", "C2027", "C2374", "C2668")

    $env:MONOLITH_RELEASE_BUILD = "1"
    Write-Host "    MONOLITH_RELEASE_BUILD=1 (release config)" -ForegroundColor DarkGray

    try {
        # Back up the existing BuildConfiguration.xml (if any) so we can restore it
        # verbatim in finally. Then write one that forces full unity.
        if (-not (Test-Path $BuildConfigDir)) {
            New-Item -ItemType Directory -Path $BuildConfigDir -Force | Out-Null
        }
        if ($HadBuildConfig) {
            Copy-Item $BuildConfigXml $BuildConfigBackup -Force
            Write-Host "    Backed up existing BuildConfiguration.xml" -ForegroundColor DarkGray
        }

        $ForceUnityXml = @"
<?xml version="1.0" encoding="utf-8" ?>
<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">
  <BuildConfiguration>
    <bUseAdaptiveUnityBuild>false</bUseAdaptiveUnityBuild>
  </BuildConfiguration>
</Configuration>
"@
        Set-Content -Path $BuildConfigXml -Value $ForceUnityXml -Encoding utf8
        Write-Host "    Forced bUseAdaptiveUnityBuild=false (full unity)" -ForegroundColor DarkGray
        Write-Host "    Building (no -DisableUnity, no -Clean -- RLPlugin hazard)..." -ForegroundColor DarkGray

        # Capture the full UBT output so we can scan it for Monolith-path collisions.
        # We do NOT throw on a non-zero UBT exit here directly -- a collision is
        # itself a non-zero exit, and we want the filtered diagnostic, not a bare
        # "exit code" message. The collision scan below is the real ship-gate.
        & $UBT MonolithEditor Win64 Development "-Project=$UProject" -waitmutex 2>&1 |
            Tee-Object -FilePath $UnityLog | Out-Null
        $unityExit = $LASTEXITCODE

        # Scan the captured log for collision error codes on Plugins\Monolith\ paths.
        # MSVC emits errors like:
        #   D:\...\Plugins\Monolith\Source\...\Foo.cpp(42): error C2084: ...
        $logLines = if (Test-Path $UnityLog) { Get-Content $UnityLog } else { @() }
        $codeAlt = ($CollisionCodes -join "|")
        $monolithCollisions = @($logLines | Where-Object {
            $_ -match "Plugins\\Monolith\\" -and $_ -match "error\s+($codeAlt)\b"
        })

        if ($monolithCollisions.Count -gt 0) {
            Write-Host "`n  [FAIL] Full-unity gate found $($monolithCollisions.Count) Monolith-path collision error(s):" -ForegroundColor Red
            $monolithCollisions | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            Write-Host "`n  This is the issue #68 failure mode: duplicate file-local symbols" -ForegroundColor Red
            Write-Host "  (anonymous-namespace or file-static names shared across .cpp files in" -ForegroundColor Red
            Write-Host "  one module) that -DisableUnity cannot see. End users build with adaptive" -ForegroundColor Red
            Write-Host "  unity and hit this on a fresh clone. Rename or hoist the colliding symbols" -ForegroundColor Red
            Write-Host "  before shipping. Refusing to publish v$Version." -ForegroundColor Red
            throw "Full-unity collision gate failed: $($monolithCollisions.Count) Monolith-path collision(s)."
        }

        # No Monolith-path collisions. If UBT still failed, surface that -- a release
        # build that does not compile under full unity is not shippable either.
        if ($unityExit -ne 0) {
            Write-Host "`n  [FAIL] Full-unity build exited $unityExit (no Monolith-path collisions, but build failed)." -ForegroundColor Red
            Write-Host "    See full log: $UnityLog" -ForegroundColor Yellow
            throw "Full-unity build failed with exit code $unityExit. Is the editor closed? See $UnityLog."
        }

        Write-Host "    Full-unity gate passed (no Monolith-path collisions)" -ForegroundColor Green
    }
    finally {
        # ALWAYS restore the BuildConfiguration.xml to its original state, even on
        # failure -- otherwise the dev's next build silently runs under full unity.
        if ($HadBuildConfig) {
            if (Test-Path $BuildConfigBackup) {
                Copy-Item $BuildConfigBackup $BuildConfigXml -Force
                Remove-Item $BuildConfigBackup -Force -ErrorAction SilentlyContinue
                Write-Host "    Restored original BuildConfiguration.xml" -ForegroundColor DarkGray
            }
        } else {
            # There was no BuildConfiguration.xml before -- remove the one we wrote.
            Remove-Item $BuildConfigXml -Force -ErrorAction SilentlyContinue
            Write-Host "    Removed temporary BuildConfiguration.xml (none existed before)" -ForegroundColor DarkGray
        }
        Remove-Item Env:\MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
        Remove-Item $UnityLog -Force -ErrorAction SilentlyContinue
        Write-Host "    MONOLITH_RELEASE_BUILD unset" -ForegroundColor DarkGray
    }
} else {
    Write-Host "`n  [1a] Skipping full-unity collision gate (-SkipBuild flag)" -ForegroundColor DarkGray
}

# --- Step 1b: Build the offline CLI fresh + hard-gate offline parity ---
# The offline tool Binaries/monolith_query.exe is built from tracked source
# Tools/MonolithQuery/monolith_query.cpp via a standalone cl.exe build (NOT UBT).
# Binaries/ is gitignored, so without this step the release would ship whatever
# stale exe happened to sit on disk. Rebuild it here so the shipped exe matches
# the shipped source, then hard-gate the exe-vs-py parity guard. A drifted exe
# must never ship -- both the build failure and a parity FAIL abort the release.
Write-Host "`n  [1b] Building offline CLI fresh + parity gate..." -ForegroundColor Yellow

$ToolDir = Join-Path $PluginDir "Tools\MonolithQuery"
$ToolBuildBat = Join-Path $ToolDir "build.bat"
if (-not (Test-Path $ToolBuildBat)) {
    throw "Offline CLI build script not found at $ToolBuildBat"
}

# Locate vcvars64.bat via vswhere so cl.exe is on PATH for build.bat.
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    throw "vswhere.exe not found at $VsWhere. Visual Studio is required to build the offline CLI."
}
$VsInstallPath = & $VsWhere -latest -property installationPath
if (-not $VsInstallPath) {
    throw "vswhere could not locate a Visual Studio installation."
}
$VcVars = Join-Path $VsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) {
    throw "vcvars64.bat not found at $VcVars."
}

# Run vcvars64.bat then build.bat in a single cmd session so cl.exe is in PATH.
# build.bat copies the freshly built exe to Plugins/Monolith/Binaries/ (the same
# Binaries dir the copy step below picks up), so the exe is staged before [3/4].
Write-Host "    Using VS at $VsInstallPath" -ForegroundColor DarkGray
# Three robustness measures, learned the hard way during the v0.18.0 release:
#   1. Prepend the VS *Installer* dir to PATH. vcvars64.bat calls 'vswhere' by bare
#      name (expecting it on PATH); where the Installer dir is NOT on PATH it prints
#      "'vswhere.exe' is not recognized" to STDERR. vcvars still recovers and inits
#      x64 fine -- but that stray STDERR line is what bites under measure #3.
#   2. Redirect the cmd subprocess STDERR into STDOUT (2>&1) and capture to a log so any
#      real build failure is diagnosable from the captured tail.
#   3. THE ACTUAL FIX: relax $ErrorActionPreference to 'Continue' around the native call.
#      This script runs under EAP='Stop'. Under Stop, PS 5.1 promotes ANY native-command
#      STDERR to a TERMINATING error -- EVEN through a 2>&1 merge (verified: Stop throws on
#      the vswhere line, Continue exits 0) -- aborting the release BEFORE the $LASTEXITCODE
#      gate, even though build.bat itself exits 0. Foreground/interactive hosts default to
#      Continue, which masked this for every prior release. With EAP relaxed, the exit code
#      is the sole arbiter; restore the prior preference immediately after.
$VsInstallerDir = Split-Path $VsWhere -Parent
$CliBuildLog = Join-Path $env:TEMP "Monolith_OfflineCLIBuild_$Version.log"
$PrevEAP = $ErrorActionPreference
Push-Location $ToolDir
try {
    $env:PATH = "$VsInstallerDir;$env:PATH"
    $ErrorActionPreference = 'Continue'
    & cmd.exe /c "call `"$VcVars`" && call `"$ToolBuildBat`"" 2>&1 |
        Tee-Object -FilePath $CliBuildLog | Out-Null
    $CliExit = $LASTEXITCODE
    $ErrorActionPreference = $PrevEAP
    if ($CliExit -ne 0) {
        if (Test-Path $CliBuildLog) {
            Write-Host "    --- offline CLI build log (tail) ---" -ForegroundColor Yellow
            Get-Content $CliBuildLog -Tail 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        }
        throw "Offline CLI build failed with exit code $CliExit. See $CliBuildLog."
    }
}
finally {
    $ErrorActionPreference = $PrevEAP
    Pop-Location
    Remove-Item $CliBuildLog -Force -ErrorAction SilentlyContinue
}
Write-Host "    Offline CLI built (fresh exe staged in Binaries/)" -ForegroundColor Green

# Hard-gate: the freshly built exe must deep-equal its Python sibling across all
# RI actions. A non-zero exit means the two offline tools drifted -- abort.
Write-Host "    Running offline parity guard (verify_offline_parity.py)..." -ForegroundColor DarkGray
$ParityScript = Join-Path $PluginDir "Scripts\verify_offline_parity.py"
& python $ParityScript
if ($LASTEXITCODE -ne 0) {
    throw "Offline parity guard FAILED (exit $LASTEXITCODE). The exe drifted from its Python sibling. Refusing to ship a drifted offline CLI."
}
Write-Host "    Offline parity guard PASSED" -ForegroundColor Green

# --- Step 2: Copy tracked files ---
Write-Host "`n  [2/4] Copying tracked files..." -ForegroundColor Yellow

if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -ItemType Directory -Path $TempDir | Out-Null

Push-Location $PluginDir
$allTrackedFiles = git ls-files
# Strip non-redistributable module sources (Source/<Module>/ and Intermediate/<Module>/)
$trackedFiles = $allTrackedFiles | Where-Object {
    $path = $_
    $keep = $true
    foreach ($mod in $StrippedModules) {
        if ($path -like "Source/$mod/*" -or $path -like "Intermediate/*$mod*") {
            $keep = $false
            break
        }
    }
    # Strip internal testing-execution records (Docs/testing/) -- per-feature test-pass
    # diaries with no downstream consumer value. Public-facing test artefacts live
    # elsewhere (SPEC sections, automation tests under Source/).
    if ($keep -and $path -like "Docs/testing/*") {
        $keep = $false
    }
    # Strip internal planning and spec folders (Docs/plans/, CRG/, PRD/, Plans/)
    if ($keep -and ($path -like "Docs/plans/*" -or $path -like "CRG/*" -or $path -like "PRD/*" -or $path -like "Plans/*")) {
        $keep = $false
    }
    # Enforce release ZIP hygiene: explicitly exclude build/local folders even if accidentally tracked
    if ($keep -and ($path -like "Intermediate/*" -or $path -like "Saved/*" -or $path -like ".git/*" -or $path -eq ".git" -or $path -like ".github/*" -or $path -eq ".github" -or $path -like ".claude/*" -or $path -eq ".claude" -or $path -like ".jules/*" -or $path -eq ".jules" -or $path -like ".vscode/*" -or $path -like ".vs/*" -or $path -like ".idea/*" -or $path -like ".clangd/*" -or $path -eq ".clangd" -or $path -like ".pytest_cache/*" -or $path -eq ".pytest_cache" -or $path -like ".ruff_cache/*" -or $path -eq ".ruff_cache" -or $path -like ".mypy_cache/*" -or $path -eq ".mypy_cache" -or $path -like ".venv/*" -or $path -eq ".venv" -or $path -like ".code-review-graph/*" -or $path -eq ".code-review-graph" -or $path -like ".cache/*" -or $path -eq ".cache" -or $path -like ".antigravitycli/*" -or $path -eq ".antigravitycli" -or $path -like "PRD/*" -or $path -eq "PRD" -or $path -like "CRG/*" -or $path -eq "CRG" -or $path -like "Plans/*" -or $path -eq "Plans" -or $path -like "Docs/plans/*" -or $path -eq "Docs/plans" -or $path -like "Docs/research/*" -or $path -eq "Docs/research" -or $path -like "TrainingMemory/*" -or $path -eq "TrainingMemory" -or $path -like "Logs/*" -or $path -eq "Logs" -or $path -like "logs/*" -or $path -eq "logs")) {
        $keep = $false
    }
    # Strip internal-only Docs that the project tracks in git but should not ship.
    # MISSING_FEATURES.md is the empirical gap log fed from real project work;
    # downstream consumers do not need it.
    if ($keep -and $path -eq "Docs/MISSING_FEATURES.md") {
        $keep = $false
    }
    $keep
}
$strippedSourceCount = $allTrackedFiles.Count - $trackedFiles.Count
foreach ($file in $trackedFiles) {
    $destPath = Join-Path $TempDir $file
    $destDir = Split-Path -Parent $destPath
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    Copy-Item $file -Destination $destPath -Force
}
Pop-Location
Write-Host "    $($trackedFiles.Count) files copied ($strippedSourceCount stripped: $($StrippedModules -join ', '))" -ForegroundColor Green

# --- Step 3: Copy binaries (gitignored but needed for Blueprint-only users) ---
Write-Host "`n  [3/4] Copying binaries..." -ForegroundColor Yellow

$binDir = Join-Path $PluginDir "Binaries"
if (Test-Path $binDir) {
    $destBin = Join-Path $TempDir "Binaries"
    New-Item -ItemType Directory -Path $destBin -Force | Out-Null
    $binCount = 0
    $binStripCount = 0
    # Build a regex that matches any stripped module's binary.
    $stripModuleRegex = "(" + (($StrippedModules | ForEach-Object { [regex]::Escape($_) }) -join "|") + ")"
    # Exclude gitignored dot-directories (e.g. .claude, .jules) that may exist physically under
    # Binaries/. git ls-files never tracks them, but the Binaries copy below walks the
    # physical directory -- without this guard such a directory and its contents could ship
    # in the public zip. No legitimate Binaries content lives under a dot-directory.
    Get-ChildItem $binDir -Recurse -File |
        Where-Object { $_.Extension -ne '.pdb' -and $_.Name -notmatch '\.patch_' -and $_.FullName -notmatch '[\\/]\.[^\\/]+[\\/]' } |
        ForEach-Object {
            if ($_.Name -match "UnrealEditor-$stripModuleRegex\.") {
                $binStripCount++
                return
            }
            $rel = $_.FullName.Substring($binDir.Length)
            $dest = Join-Path $destBin $rel
            $destParent = Split-Path -Parent $dest
            if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
            Copy-Item $_.FullName -Destination $dest -Force
            $binCount++
        }
    Write-Host "    $binCount binary files included (no .pdb, no .patch_*, $binStripCount stripped: $($StrippedModules -join ', '))" -ForegroundColor Green
} else {
    Write-Host "    WARNING: No Binaries/ found - Blueprint-only users will need to compile" -ForegroundColor Red
}

# --- Step 4: Patch and package ---
Write-Host "`n  [4/4] Packaging..." -ForegroundColor Yellow

# Set Installed=true for Blueprint-only users
$upluginPath = Join-Path $TempDir "Monolith.uplugin"
$content = Get-Content $upluginPath -Raw
$content = $content -replace '"Installed":\s*false', '"Installed": true'

# Strip non-redistributable module entries from the "Modules" array.
# Matches an optional preceding comma, the module object block, and its trailing comma (if present).
$upluginStrips = 0
foreach ($mod in $StrippedModules) {
    # Match the module object + its trailing comma (if present). Do NOT consume the leading
    # comma -- that belongs to the previous entry and must stay. If the stripped module was
    # the LAST array entry (no trailing comma), the previous entry's trailing comma is
    # orphaned; the ",]" cleanup below catches that.
    $escMod = [regex]::Escape($mod)
    $pattern = "(?s)\{\s*""Name"":\s*""$escMod"".*?\}\s*,?\s*"
    $before = $content.Length
    $content = [regex]::Replace($content, $pattern, "")
    if ($content.Length -ne $before) { $upluginStrips++ }
}

# Strip any trailing comma immediately before a closing array bracket.
$content = $content -replace ',(\s*\])', '$1'

Set-Content $upluginPath $content -NoNewline
Write-Host "    Installed=true set in .uplugin, $upluginStrips module entries stripped" -ForegroundColor Green

# Create zip
if (Test-Path $OutputZip) { Remove-Item $OutputZip -Force }
Compress-Archive -Path "$TempDir\*" -DestinationPath $OutputZip -Force

# Clean temp
Remove-Item $TempDir -Recurse -Force

$fileSize = [math]::Round((Get-Item $OutputZip).Length / 1MB, 1)
Write-Host "`nRelease complete: $OutputZip" -ForegroundColor Green
Write-Host "Size: ${fileSize}MB" -ForegroundColor Green
Write-Host "`nVerify: optional deps should be OFF in the binaries." -ForegroundColor Cyan
Write-Host "  WITH_BLUEPRINT_ASSIST=0, WITH_GBA=0, WITH_COMMONUI=0" -ForegroundColor Cyan
Write-Host "  WITH_COMBOGRAPH=0, WITH_LOGICDRIVER=0, WITH_METASOUND=0" -ForegroundColor Cyan
Write-Host "  WITH_GAMEPLAYABILITIES=0" -ForegroundColor Cyan
Write-Host "  WITH_MASSENTITY=0, WITH_ZONEGRAPH=0" -ForegroundColor Cyan
Write-Host "  WITH_STATETREE=0, WITH_SMARTOBJECTS=0   (after F22 lands)" -ForegroundColor Cyan
Write-Host "  Your next editor build will auto-detect deps normally." -ForegroundColor DarkGray

# --- Step 5: Post-build hard-link smoke (defense against issue #30) ---
# Issue #30 (v0.14.0): MonolithMesh.dll hard-linked GeometryScriptingCore.dll because
# MonolithMesh.Build.cs missed the MONOLITH_RELEASE_BUILD=1 bypass on its GeometryScripting
# probe. End-user editors failed to load Monolith with "missing import" errors.
#
# This step dumps the import table of every UnrealEditor-Monolith*.dll in the release zip
# and refuses if any imports a sentinel module -- a module from a non-default-enabled UE
# plugin that should NEVER appear in a release-built Monolith binary.
Write-Host "`n  [5/5] Post-build hard-link smoke (issue #30 defense)..." -ForegroundColor Yellow

# Sentinel modules: their presence in a Monolith DLL's imports = build-time gate failure.
# Add new sentinels when adding new optional plugin integrations.
#
# GameplayAbilities removed from sentinels in v0.14.7: it's declared as a hard
# dep in Monolith.uplugin (no Optional flag), so the engine auto-enables it on
# Monolith install and guarantees load order. MonolithGAS + MonolithIndex
# hard-link GameplayAbilities and that's functionally safe under this contract.
# Migration to optional + WITH_GAMEPLAYABILITIES gate planned for v0.14.8
# alongside the StructUtils-cleanup follow-up.
#
# External optional widget runtime sentinel: MonolithUI must stay decoupled from
# provider plugins. Exact public dependency sentinels below catch accidental hard
# DLL imports before the public zip ships; downstream providers should extend
# this list in their own release wrappers.
# Each entry is the EXACT module-name string a Build.cs adds under an optional gate.
# The smoke matches "UnrealEditor-<entry>.dll" in a shipped DLL's import table, so a
# casing/name mismatch here means a real leak slips past (issue #71: MassSpawner was
# missing entirely; MetaSoundEngine/MetaSoundFrontend had wrong casing -- the engine
# DLLs are UnrealEditor-MetasoundEngine.dll, lowercase 's'; ComboGraphRuntime and
# LogicDriver named no real module DLL). The $OptionalModuleUnion drift assertion below
# is the source of truth -- if it FAILs, add the missing name HERE (and keep it sorted
# by source module for review sanity).
$LeakSentinels = @(
    # MonolithMesh / MonolithWorldGen -- GeometryScripting (delay-loaded but still gated)
    "GeometryScriptingCore", "GeometryFramework", "GeometryCore",
    # MonolithUI -- CommonUI
    "CommonUI", "CommonInput",
    # MonolithBABridge -- BlueprintAssist
    "BlueprintAssist",
    # MonolithAI -- GameplayBehaviors / MassEntity / ZoneGraph / StateTree / SmartObjects
    "GameplayBehaviorsModule", "MassEntity", "MassSpawner", "MassGameplayEditor", "ZoneGraph",
    "StateTreeModule", "StateTreeEditorModule", "GameplayStateTreeModule", "PropertyBindingUtils",
    "SmartObjectsModule", "SmartObjectsEditorModule",
    # MonolithGAS -- GBA (Blueprint Attributes). NOTE: GameplayAbilities is deliberately NOT a
    # sentinel -- it is a hard dep in Monolith.uplugin (Enabled, no Optional), so the engine
    # auto-enables it and guarantees its DLL is present; MonolithGAS/MonolithIndex hard-link it
    # safely (removed from sentinels in v0.14.7, see the rationale block above).
    "BlueprintAttributes",
    # MonolithIndex / MonolithAudio -- MetaSound (engine DLLs are 'Metasound', lowercase s)
    "MetasoundEngine", "MetasoundFrontend", "MetasoundEditor",
    # MonolithAnimation -- Chooser
    "Chooser",
    # MonolithComboGraph -- ComboGraph (was stale 'ComboGraphRuntime')
    "ComboGraph", "ComboGraphEditor",
    # MonolithLogicDriver -- Logic Driver Pro / SMSystem (was stale 'LogicDriver')
    "SMSystem", "SMSystemEditor"
)

# --- Drift assertion: $LeakSentinels must be a superset of every optional-gated module ---
# Single source of truth so the next optional dep cannot silently slip past the smoke
# (issue #71 root cause: MassSpawner was added to a Build.cs optional block but never to
# the sentinel list, so the leak shipped). We use an explicit expected-union array rather
# than parsing the .Build.cs files at release time: block-scoped PowerShell parsing of C#
# is brittle (multi-line AddRange, nested if, comments), and a false-negative parse would
# REINTRODUCE the exact silent-gap failure this guard exists to kill. The union below is
# the auditor-verified list of module strings each Source/*/*.Build.cs adds inside an
# optional/release-gated block (if (bHas...) / if (!bReleaseBuild...)). When you add a new
# optional dep to ANY Build.cs, add its module string(s) HERE and to $LeakSentinels above.
$OptionalModuleUnion = @(
    "GeometryScriptingCore", "GeometryFramework", "GeometryCore",   # MonolithMesh / MonolithWorldGen
    "CommonUI", "CommonInput",                                       # MonolithUI
    "BlueprintAssist",                                               # MonolithBABridge
    "GameplayBehaviorsModule", "MassEntity", "MassSpawner",          # MonolithAI
    "MassGameplayEditor", "ZoneGraph",                              # MonolithAI
    "StateTreeModule", "StateTreeEditorModule",                     # MonolithAI
    "GameplayStateTreeModule", "PropertyBindingUtils",             # MonolithAI
    "SmartObjectsModule", "SmartObjectsEditorModule",              # MonolithAI
    # GameplayAbilities is optional-gated in MonolithAI but a HARD dep in Monolith.uplugin and
    # hard-linked unconditionally (and safely) in MonolithGAS/MonolithIndex -- so it is NOT a
    # sentinel and is excluded from this drift union (it is not an unsafe optional dep).
    "BlueprintAttributes",                                          # MonolithGAS
    "MetasoundEngine", "MetasoundFrontend", "MetasoundEditor",      # MonolithIndex / MonolithAudio
    "Chooser",                                                      # MonolithAnimation
    "ComboGraph", "ComboGraphEditor",                              # MonolithComboGraph
    "SMSystem", "SMSystemEditor"                                    # MonolithLogicDriver
)

$MissingSentinels = $OptionalModuleUnion | Where-Object { $LeakSentinels -notcontains $_ }
if ($MissingSentinels.Count -gt 0) {
    Write-Host "`n  [FAIL] Sentinel drift: optional-gated module(s) missing from `$LeakSentinels:" -ForegroundColor Red
    $MissingSentinels | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Write-Host "`n  Every module added under an optional/release gate in a Source/*/*.Build.cs MUST" -ForegroundColor Red
    Write-Host "  be in `$LeakSentinels, or a hard-link leak can ship unflagged (issue #71)." -ForegroundColor Red
    Write-Host "  Add the missing name(s) to `$LeakSentinels above, then re-run." -ForegroundColor Red
    exit 1
}

# Locate dumpbin.exe -- ships with Visual Studio Build Tools. Try common locations,
# then a vswhere.exe lookup, before giving up. This smoke is MANDATORY: without it a
# release can ship with ZERO import verification (issue #71 root cause #1 -- the old
# code printed [SKIP] and continued, so a leak shipped). If dumpbin still can't be
# found we HARD FAIL; the only bypass is -AllowUnverifiedImports (never set for a real
# release -- it produces a NON-shippable test zip on a machine without VS Build Tools).
$Dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $Dumpbin) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $vsBase = Join-Path $installPath "VC\Tools\MSVC"
            if (Test-Path $vsBase) {
                $candidate = Get-ChildItem -Path $vsBase -Directory -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending |
                    ForEach-Object { Join-Path $_.FullName "bin\HostX64\x64\dumpbin.exe" } |
                    Where-Object { Test-Path $_ } |
                    Select-Object -First 1
                if ($candidate) { $Dumpbin = $candidate }
            }
        }
    }
}

# Fallback: ask vswhere.exe (ships with every VS 2017+ installer) to locate dumpbin.exe
# anywhere a VS instance is installed -- covers non-2022 / non-default-edition layouts.
if (-not $Dumpbin) {
    $VSWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VSWhere) {
        $vswhereHit = & $VSWhere -latest -find "**\dumpbin.exe" 2>$null | Select-Object -First 1
        if ($vswhereHit -and (Test-Path $vswhereHit)) { $Dumpbin = $vswhereHit }
    }
}

if (-not $Dumpbin) {
    if ($AllowUnverifiedImports) {
        Write-Host "    [WARN] dumpbin.exe not found AND -AllowUnverifiedImports set." -ForegroundColor Yellow
        Write-Host "    [WARN] Hard-link import smoke SKIPPED -- this zip is NOT shippable." -ForegroundColor Yellow
        Write-Host "    [WARN] Imports are UNVERIFIED. Do NOT publish this build (issue #71 risk)." -ForegroundColor Yellow
    } else {
        Write-Host "`n  [FAIL] dumpbin.exe not found -- cannot run the mandatory hard-link import smoke." -ForegroundColor Red
        Write-Host "  Install Visual Studio Build Tools (provides dumpbin.exe) and re-run." -ForegroundColor Red
        Write-Host "  Shipping without this check is how issue #71 (MassSpawner/ZoneGraph hard-link) escaped." -ForegroundColor Red
        Write-Host "  Bypass ONLY for a non-shippable local test zip: re-run with -AllowUnverifiedImports." -ForegroundColor Red
        exit 1
    }
} else {
    # Re-extract the just-built zip into a scratch dir to inspect the actual shipped DLLs
    # (not the dev binaries we may have overwritten before zipping).
    $SmokeDir = Join-Path $env:TEMP "Monolith_Release_${Version}_Smoke"
    if (Test-Path $SmokeDir) { Remove-Item $SmokeDir -Recurse -Force }
    New-Item -ItemType Directory -Path $SmokeDir | Out-Null
    Expand-Archive -Path $OutputZip -DestinationPath $SmokeDir -Force

    $MonolithDlls = @(Get-ChildItem -Path $SmokeDir -Recurse -Filter "UnrealEditor-Monolith*.dll")
    $LeakingDlls = @()
    foreach ($dllItem in $MonolithDlls) {
        $imports = & $Dumpbin /imports $dllItem.FullName 2>$null | Out-String
        foreach ($sentinel in $LeakSentinels) {
            # Match "UnrealEditor-<Sentinel>.dll" in the import table
            if ($imports -match "UnrealEditor-$([regex]::Escape($sentinel))\.dll") {
                $LeakingDlls += [PSCustomObject]@{
                    Dll      = $dllItem.Name
                    Sentinel = $sentinel
                }
            }
        }
    }

    # Cleanup smoke dir regardless of outcome
    Remove-Item $SmokeDir -Recurse -Force -ErrorAction SilentlyContinue

    if ($LeakingDlls.Count -gt 0) {
        Write-Host "`n  [FAIL] Hard-link smoke found $($LeakingDlls.Count) sentinel import(s) in shipped DLLs:" -ForegroundColor Red
        $LeakingDlls | ForEach-Object {
            Write-Host "    $($_.Dll) imports UnrealEditor-$($_.Sentinel).dll" -ForegroundColor Red
        }
        Write-Host "`n  This is the issue #30 failure mode. The Build.cs for the affected module" -ForegroundColor Red
        Write-Host "  is not honouring MONOLITH_RELEASE_BUILD=1. Fix the Build.cs probe before shipping." -ForegroundColor Red
        Write-Host "`n  Refusing to publish v$Version. Delete $OutputZip after fixing Build.cs and re-run." -ForegroundColor Red
        exit 1
    }
    Write-Host "    No sentinel imports found in $($MonolithDlls.Count) Monolith DLLs (clean)" -ForegroundColor Green

    # Record the exact zip the smoke just inspected, pinned by hash, so the SHA256
    # printed for the release notes can be asserted to come from THIS smoked artifact
    # and not a post-smoke manual re-zip (issue #71 risk: SHA of an unverified rebuild).
    $SmokedZipPath = $OutputZip
    $SmokedZipHash = (Get-FileHash -Algorithm SHA256 -Path $OutputZip).Hash.ToLower()
}

# --- SHA256 hash for release notes (Issue #38) ---
# Marker token is `Monolith-SHA256:` (not bare `SHA256:`) so the auto-updater's
# regex never collides with prose mentions of the word SHA256 elsewhere in the
# release body. The parser anchors on this exact sentinel.
if (Test-Path $OutputZip) {
    $Hash = (Get-FileHash -Algorithm SHA256 -Path $OutputZip).Hash.ToLower()

    # Pin the published hash to the smoked bytes. The hard-link smoke above inspected
    # the DLLs inside $OutputZip; if the zip was replaced/re-built after the smoke (e.g.
    # a manual re-zip), this hash would describe UNVERIFIED bytes -- exactly the gap that
    # let issue #71 ship. Assert the file the smoke checked is the file we are hashing.
    # ($SmokedZipHash is unset only when the smoke was bypassed via -AllowUnverifiedImports,
    # in which case we already warned loudly that the zip is not shippable.)
    if ($SmokedZipHash) {
        if ($Hash -ne $SmokedZipHash -or $OutputZip -ne $SmokedZipPath) {
            Write-Host "`n  [FAIL] Zip changed after the hard-link smoke ran." -ForegroundColor Red
            Write-Host "  Smoked: $SmokedZipPath ($SmokedZipHash)" -ForegroundColor Red
            Write-Host "  Hashing now: $OutputZip ($Hash)" -ForegroundColor Red
            Write-Host "  The release-notes SHA must describe the SMOKED bytes. Re-run the full" -ForegroundColor Red
            Write-Host "  release so the smoke and the published hash see the same artifact." -ForegroundColor Red
            exit 1
        }
        Write-Host "    SHA256 pinned to the smoked zip (imports verified)" -ForegroundColor Green
    }
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "SHA256: $Hash" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Paste this exact line into the GitHub Release notes body (include all platform markers if multi-platform):" -ForegroundColor Yellow
    Write-Host ""
    $MarkerPrefix = if ($IsMacOS) { "Monolith-macOS-SHA256" } elseif ($IsLinux) { "Monolith-Linux-SHA256" } else { "Monolith-SHA256" }
    Write-Host "  ${MarkerPrefix}: $Hash" -ForegroundColor White
    Write-Host ""
    Write-Host "The auto-updater parses this exact marker and fails safe (refusing" -ForegroundColor Yellow
    Write-Host "to install) if the downloaded zip's hash does not match. If the marker"        -ForegroundColor Yellow
    Write-Host "is missing entirely, it logs a warning and proceeds without checking."         -ForegroundColor Yellow
    Write-Host "Do not rename or"                                                              -ForegroundColor Yellow
    Write-Host "reformat the marker -- the prefix and a single space before the"   -ForegroundColor Yellow
    Write-Host "hex string are required."                                          -ForegroundColor Yellow
    Write-Host "================================================================" -ForegroundColor Cyan
}
