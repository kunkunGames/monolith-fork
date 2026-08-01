# ==============================================================================
# verify_release_body.ps1 -- ship-blocking gate for a DRAFT GitHub release
# ==============================================================================
#
# Run this AFTER 'gh release create --draft' has uploaded its assets and BEFORE
# 'gh release edit --draft=false'. It exits non-zero on any failure, so the
# publish flow must stop rather than flip a bad release live.
#
# WHY DRAFT-THEN-FLIP AT ALL
#   'gh release create <tag> <zips>' creates the release PUBLISHED and only then
#   uploads the assets. During that upload window GET /releases/latest returns
#   the new tag with zero or partial assets. Monolith clients from v0.14.7
#   through v0.21.2 fall back to GitHub's generated source zipball when a
#   release carries no usable binary zip, and that archive has no compiled
#   DLLs -- so a client polling inside the window installs a broken plugin over
#   a working one. Those clients are already deployed and no change to HEAD can
#   reach them. Publishing as a draft closes the window for ALL of them,
#   because GET /releases/latest is documented to return "the most recent
#   non-prerelease, non-draft release". (Prereleases are excluded too, so never
#   use --prerelease for a staged rollout expecting clients to see it.)
#
# NOTE ON ENCODING: this file must stay ASCII-only. Windows PowerShell 5.1
# falls back to Windows-1252 when a .ps1 has no UTF-8 BOM, which mis-tokenises
# non-ASCII punctuation and produces parse errors at unrelated line numbers.
#
# Usage:
#   .\verify_release_body.ps1 -Version "0.21.3"
#   .\verify_release_body.ps1 -Version "0.21.3" -ArtifactDir <dir>   # if the zips are elsewhere
# ==============================================================================

[CmdletBinding()]
param(
    # Not declared Mandatory: -SelfTest runs without a release, and a Mandatory
    # parameter would prompt (and hang) in a non-interactive shell. Validated below.
    [Parameter(Mandatory = $false)] [string] $Version,
    # Runs the pre-v2 detector fixtures and exits. Touches no network and no release.
    [Parameter(Mandatory = $false)] [switch] $SelfTest,
    [Parameter(Mandatory = $false)] [string] $Repo        = "tumourlove/monolith",
    # Defaults to the host project root, which is where make_release.ps1 writes the
    # zips. The default is resolved after parameter binding because Windows
    # PowerShell 5.1 can expose an empty $PSScriptRoot while evaluating param-block
    # default expressions, preventing even -SelfTest from reaching its fixtures.
    [Parameter(Mandatory = $false)] [string] $ArtifactDir
)

$ErrorActionPreference = "Stop"
$script:Failures = @()

function Fail([string] $Message) {
    $script:Failures += $Message
    Write-Host "  [FAIL] $Message" -ForegroundColor Red
}
function Pass([string] $Message) {
    Write-Host "  [ OK ] $Message" -ForegroundColor Green
}

# Single source of truth for check 3. See the long comment at that check for why
# this is deliberately UNANCHORED. -SelfTest proves it both rejects and accepts.
$script:PreV2MarkerPattern = 'Monolith-SHA256(-UE5\.[0-9]+)?:'

# --- Self-test ----------------------------------------------------------------
# Guards the one property that matters: the detector must be at least as
# permissive as the deployed client's unanchored FindNext() matcher, while still
# accepting a body that carries only the three correct v2 markers. Without this,
# a future tidy-up re-anchors the regex and silently re-opens the crash path.
if ($SelfTest) {
    $H = 'a' * 64
    $mustReject = @(
        @{ Name = 'bare marker';        Body = "Monolith-SHA256: $H" },
        @{ Name = 'markdown bullet';    Body = "- Monolith-SHA256: $H" },
        @{ Name = 'table cell';         Body = "| Monolith-SHA256: | $H |" },
        @{ Name = 'prose prefix';       Body = "Legacy zip -- Monolith-SHA256: $H" },
        @{ Name = 'hash on next line';  Body = "Monolith-SHA256:`n$H" },
        @{ Name = 'engine-tagged';      Body = "- Monolith-SHA256-UE5.7: $H" },
        @{ Name = 'indented tagged';    Body = "    Monolith-SHA256-UE5.8: $H" }
    )
    $mustAccept = @(
        @{ Name = 'the three v2 markers'
           Body = "Monolith-SHA256-v2-UE5.7: $H`nMonolith-SHA256-v2-UE5.8: $H`nMonolith-SHA256-v2: $H" },
        @{ Name = 'v2 markers in prose/bullets'
           Body = "- Monolith-SHA256-v2: $H`n| Monolith-SHA256-v2-UE5.7: | $H |" }
    )

    Write-Host ""
    Write-Host "Self-test: pre-v2 marker detector" -ForegroundColor Cyan
    Write-Host ""
    $bad = 0
    foreach ($c in $mustReject) {
        if ([regex]::Matches($c.Body, $script:PreV2MarkerPattern).Count -gt 0) {
            Pass "rejects $($c.Name)"
        } else {
            Fail "MISSED pre-v2 marker: $($c.Name) -- this shape would crash deployed clients"; $bad++
        }
    }
    foreach ($c in $mustAccept) {
        if ([regex]::Matches($c.Body, $script:PreV2MarkerPattern).Count -eq 0) {
            Pass "accepts $($c.Name)"
        } else {
            Fail "FALSE POSITIVE on $($c.Name) -- this would block every valid release"; $bad++
        }
    }
    Write-Host ""
    if ($bad -gt 0) { Write-Host "SELF-TEST FAILED ($bad)" -ForegroundColor Red; exit 1 }
    Write-Host "SELF-TEST PASSED ($($mustReject.Count) reject, $($mustAccept.Count) accept)" -ForegroundColor Green
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    throw "-Version is required (omit it only with -SelfTest)."
}

if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    # This script lives at <ProjectRoot>\Plugins\Monolith\Scripts\. Derive the
    # project root here, after $PSScriptRoot is guaranteed to be initialized.
    $ArtifactDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
}

$Tag = "v$Version"
Write-Host ""
Write-Host "Verifying draft release $Tag in $Repo" -ForegroundColor Cyan
Write-Host ""

# --- Fetch the release --------------------------------------------------------
# --jq is deliberately avoided; ConvertFrom-Json keeps this readable on PS 5.1.
$raw = & gh release view $Tag --repo $Repo --json isDraft,body,assets 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "  [FAIL] Could not read release $Tag. gh said:" -ForegroundColor Red
    Write-Host "    $raw" -ForegroundColor Yellow
    exit 1
}
$rel = $raw | ConvertFrom-Json

# --- 1. It must still be a draft ---------------------------------------------
# If this is already published the gate is pointless -- the window is open.
if ($rel.isDraft -ne $true) {
    Fail "Release $Tag is ALREADY PUBLISHED. This gate must run while it is still a draft."
} else {
    Pass "Release is a draft"
}

# --- 2. Exactly three assets, all fully uploaded ------------------------------
$expected = @(
    "Monolith-v$Version-UE5.7.zip",
    "Monolith-v$Version-UE5.8.zip",
    "Monolith-v$Version.zip"
)
$actual = @($rel.assets | ForEach-Object { $_.name })

if ($actual.Count -ne 3) {
    Fail "Expected 3 assets, found $($actual.Count): $($actual -join ', ')"
} else {
    Pass "Asset count is 3"
}
foreach ($name in $expected) {
    if ($actual -notcontains $name) {
        Fail "Missing expected asset: $name"
    }
}
foreach ($name in $actual) {
    if ($expected -notcontains $name) {
        Fail "Unexpected asset present: $name (would be offered to updaters)"
    }
}
foreach ($a in $rel.assets) {
    if ($a.state -ne "uploaded") {
        Fail "Asset $($a.name) is in state '$($a.state)', not 'uploaded'"
    }
    if ([int64]$a.size -le 0) {
        Fail "Asset $($a.name) has size $($a.size)"
    }
}
if ($script:Failures.Count -eq 0) { Pass "All 3 assets present, uploaded, non-empty" }

# --- 3. Pre-v2 SHA markers are a PERMANENT ship-blocker -----------------------
# Updaters shipped in v0.14.7 through v0.21.0 call FPlatformMisc::GetSHA256Signature
# when they RECOGNISE a marker. That function has no Windows implementation and its
# generic fallback is a fatal checkf, so a recognised marker in a release body
# hard-crashes every un-upgraded Windows client that clicks Install (issues #90/#94).
# The v2 names are invisible to those parsers, so old clients fail safe.
#
# THE DETECTOR MUST BE UNANCHORED -- do not "tidy" this back to '(?m)^\s*...'.
# The deployed client parser (v0.21.0 MonolithUpdateSubsystem.cpp:338) is
#     FRegexPattern("Monolith-SHA256:\s*([0-9a-fA-F]{64})(?![0-9a-fA-F])")
# driven by FindNext(), i.e. unanchored: it matches at ANY column and its \s*
# spans newlines. A line-anchored gate that only tolerates leading whitespace is
# strictly weaker than the thing it defends, so all of these used to PASS the
# gate and still crash the client:
#     - Monolith-SHA256: <64hex>              (markdown bullet; "-" is not \s)
#     | Monolith-SHA256: | <64hex> |          (table cell)
#     Legacy zip -- Monolith-SHA256: <64hex>  (any prose prefix)
#     Monolith-SHA256:\n<64hex>               (hash on the next line)
# The first two are ordinary release-note formatting. Mirroring the client's own
# matching semantics is the only correct gate. Consequence, and it is intended:
# any PROSE MENTION of a pre-v2 name now fails the gate, which is exactly the
# documented contract -- the pre-v2 names must never appear in a body again.
# This cannot false-positive on the v2 names: after the "Monolith-SHA256"
# literal the optional group cannot consume "-v2", and the required ":" sees "-".
# Run -SelfTest to prove both halves.
$body = [string]$rel.body
$preV2 = [regex]::Matches($body, $script:PreV2MarkerPattern)
if ($preV2.Count -gt 0) {
    Fail "Body contains $($preV2.Count) PRE-V2 SHA marker(s). These hard-crash every deployed v0.14.7-v0.21.0 Windows client that clicks Install. Remove them."
} else {
    Pass "No pre-v2 SHA markers"
}

# --- 4. Exactly the three v2 markers -----------------------------------------
$markerMap = @{
    "Monolith-SHA256-v2-UE5.7:" = "Monolith-v$Version-UE5.7.zip"
    "Monolith-SHA256-v2-UE5.8:" = "Monolith-v$Version-UE5.8.zip"
    "Monolith-SHA256-v2:"       = "Monolith-v$Version.zip"
}
$markerValues = @{}
foreach ($prefix in $markerMap.Keys) {
    # The updater anchors on the literal prefix followed by a single space.
    $pattern = '(?m)^' + [regex]::Escape($prefix) + ' ([0-9a-fA-F]{64})(?![0-9a-fA-F])'
    $m = [regex]::Matches($body, $pattern)
    if ($m.Count -ne 1) {
        Fail "Expected exactly 1 '$prefix' marker, found $($m.Count)"
    } else {
        $markerValues[$prefix] = $m[0].Groups[1].Value.ToLower()
    }
}
if ($markerValues.Count -eq 3) { Pass "All 3 v2 markers present and well-formed" }

# --- 5. Each marker must match the actual artifact ----------------------------
# This is the check that would have caught a stale marker copied from a prior run.
foreach ($prefix in $markerMap.Keys) {
    if (-not $markerValues.ContainsKey($prefix)) { continue }
    $zip = Join-Path $ArtifactDir $markerMap[$prefix]
    if (-not (Test-Path $zip)) {
        Fail "Cannot verify '$prefix': artifact not found at $zip"
        continue
    }
    $actualSha = (Get-FileHash -Path $zip -Algorithm SHA256).Hash.ToLower()
    if ($actualSha -ne $markerValues[$prefix]) {
        Fail "SHA mismatch for $($markerMap[$prefix]): body says $($markerValues[$prefix]), file is $actualSha"
    } else {
        Pass "SHA matches for $($markerMap[$prefix])"
    }
}

# The legacy bridge zip is a byte copy of the UE5.7 zip, so their hashes must agree.
if ($markerValues.ContainsKey("Monolith-SHA256-v2:") -and $markerValues.ContainsKey("Monolith-SHA256-v2-UE5.7:")) {
    if ($markerValues["Monolith-SHA256-v2:"] -ne $markerValues["Monolith-SHA256-v2-UE5.7:"]) {
        Fail "Legacy marker does not equal the UE5.7 marker. The bridge zip is supposed to be a copy of the UE5.7 zip."
    } else {
        Pass "Legacy bridge marker equals UE5.7 marker"
    }
}

# --- 6. No AI attribution in a public release body ---------------------------
# The single-character classes below ([l], [w], [n]) are deliberate. They match
# exactly as the plain letters would, but they stop this file from containing the
# literal phrases it searches for -- this script ships inside the release zip, and
# the zip-contents scan asserts that no shipped text payload contains them. Without
# the classes, the detector would trip on itself.
$attribution = [regex]::Matches($body, '(?i)co-authored.{0,20}c[l]aude|generated [w]ith claude|[n]oreply@anthropic')
if ($attribution.Count -gt 0) {
    Fail "Release body contains AI-attribution text. All public content is attributed solely to the maintainer."
} else {
    Pass "No AI-attribution text in body"
}

# --- Verdict ------------------------------------------------------------------
Write-Host ""
if ($script:Failures.Count -gt 0) {
    Write-Host "GATE FAILED -- $($script:Failures.Count) problem(s). Do NOT publish." -ForegroundColor Red
    Write-Host "The release is still a draft, so nothing is visible to clients yet." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

Write-Host "GATE PASSED. Safe to publish:" -ForegroundColor Green
Write-Host "  gh release edit $Tag --repo $Repo --draft=false" -ForegroundColor Cyan
Write-Host ""
exit 0
