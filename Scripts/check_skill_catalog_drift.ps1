<#
.SYNOPSIS
Deterministic skill <-> live-catalog drift guard. Compares the action and
parameter documentation in each Monolith Agent Skill (SKILL.md plus any linked
references/*.md tables) against the LIVE MCP catalog so the per-skill action
tables cannot silently rot as the runtime catalog changes.

.DESCRIPTION
The Agent Skills enrichment effort documents, per namespace, an action table
(`## Action Reference`) and full parameter signatures. Those tables are a
hand-maintained snapshot of `monolith_discover`; the live catalog stays the
only source of truth. This script makes the snapshot self-checking: it fetches
the live catalog for each skill's mapped namespace(s) and reports

  (a) an action documented in the skill but ABSENT from the live catalog,
  (b) a documented param name not present in that action's live schema,
  (c) a param the skill marks required (`name*`) that the schema reports
      optional, or marked optional (`name?` / `name=default`) that the schema
      reports required,
  (d) live actions the skill does not document (informational only).

(a), (b), and (c) are HARD drift and fail the run (nonzero exit) so the check
can gate CI. (d) is informational and never fails. `-ReportOnly` prints the
same report but always exits 0.

A namespace that is genuinely not loaded in the running editor
(`status:"not_installed"` or `Unknown namespace`, e.g. combograph /
logicdriver) is reported as "skipped: plugin not loaded" and is NOT drift — a
skill that documents an optional-plugin namespace must not fail CI just because
the plugin is absent from this machine.

Two source-of-truth modes:

  LIVE (default)  Query monolith_discover over JSON-RPC at -McpUrl.
  -Offline        Read pre-fetched namespace dumps (one JSON file per
                  namespace) from -DumpDir. Namespaces with no dump file are
                  skipped (reported as no_dump), never treated as drift.

Each dump file under -DumpDir is named `<namespace>.json` and holds either the
raw `monolith_discover` catalog object ({ namespace, actions:[...] }) or the
not_installed sentinel ({ namespace, actions:0, status:"not_installed" }).

.PARAMETER SkillsRoot
Skills root containing the per-skill directories (default: <plugin>/Skills).

.PARAMETER McpUrl
MCP JSON-RPC endpoint for LIVE mode (default: MONOLITH_URL env var, else
http://localhost:9316/mcp).

.PARAMETER Offline
Use pre-fetched namespace dumps from -DumpDir instead of querying the MCP.

.PARAMETER DumpDir
Directory of `<namespace>.json` catalog dumps for -Offline mode.

.PARAMETER Skill
Restrict the run to one or more skill names (default: every mapped skill).

.PARAMETER ShowUndocumented
Include the (d) informational list of live-but-undocumented actions in the
report. Off by default to keep the report focused on hard drift.

.PARAMETER ReportOnly
Print the full report but always exit 0 (do not fail on hard drift).

.OUTPUTS
Line-oriented status: INFO / SKILL= / SKIP / DRIFT / INFO-UNDOC / RESULT=
tokens, one RESULT= summary at the end.

Exit codes:
  0  no hard drift (or -ReportOnly)
  2  hard drift found (a/b/c) in at least one skill
  3  blocked: skills root missing, or LIVE mode and the MCP endpoint is down,
     or -Offline with a missing -DumpDir
#>
[CmdletBinding()]
param(
    [string]$SkillsRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Skills'),
    [string]$McpUrl = $(if ($env:MONOLITH_URL) { $env:MONOLITH_URL } else { 'http://localhost:9316/mcp' }),
    [switch]$Offline,
    [string]$DumpDir,
    [string[]]$Skill,
    [switch]$ShowUndocumented,
    [switch]$ReportOnly,
    [string]$GatedAllowlist = (Join-Path $PSScriptRoot 'skill_drift_gated_actions.json')
)

$ErrorActionPreference = 'Stop'

# --- Feature-gated action allowlist -------------------------------------------
# Actions that exist in source but only register in the live catalog when a
# feature flag is on (e.g. bEnableProceduralTownGen). Loaded from a sidecar
# JSON (skill -> [action names]); reported as GATED (informational), NOT hard
# drift, so the tool can gate CI. See SPEC_MonolithSkillCatalogDrift.md.
$script:GatedMap = @{}
if ($GatedAllowlist -and (Test-Path -LiteralPath $GatedAllowlist)) {
    try {
        $gatedJson = Get-Content -LiteralPath $GatedAllowlist -Raw | ConvertFrom-Json
        foreach ($prop in $gatedJson.PSObject.Properties) {
            if ($prop.Name -like '_*') { continue }
            $script:GatedMap[$prop.Name] = @($prop.Value)
        }
    }
    catch {
        Write-Output ("INFO gated_allowlist_unreadable path={0} detail={1}" -f $GatedAllowlist, $_.Exception.Message)
    }
}

# --- Skill -> namespace(s) map -------------------------------------------------
# Authoritative routing map (mirrors CLAUDE.md section 17 + the per-namespace
# skill ownership). Skills with no backing namespace (pure reference knowledge)
# map to an empty array and are reported as no_namespace. logicdriver/combograph
# are optional-plugin namespaces and surface as "skipped: plugin not loaded"
# whenever the plugin is not loaded.
$SkillNamespaceMap = [ordered]@{
    'monolith-mcp'           = @('monolith')
    'monolith-schema'        = @('describe', 'bulk_fill')
    'unreal-ai'              = @('ai')
    'unreal-animation'       = @('animation')
    'unreal-asset'           = @('asset')
    'unreal-audio'           = @('audio')
    'unreal-blueprints'      = @('blueprint')
    'unreal-bridge'          = @('bridge')
    'unreal-build'           = @('editor')
    'unreal-chaos-fracture'  = @('chaos_fracture')
    'unreal-chooser'         = @('chooser')
    'unreal-cloth'           = @('cloth')
    'unreal-collection'      = @('collection')
    'unreal-combograph'      = @('combograph')
    'unreal-config'          = @('config')
    'unreal-cpp'             = @('source', 'config')
    'unreal-dataflow'        = @('dataflow')
    'unreal-debugging'       = @('editor')
    'unreal-gamefeatures'    = @('gamefeatures')
    'unreal-gas'             = @('gas')
    'unreal-hlod'            = @('hlod')
    'unreal-imagegen'        = @('imagegen')
    'unreal-input'           = @('input')
    'unreal-interchange'     = @('interchange')
    'unreal-level-instance'  = @('level_instance')
    'unreal-level-sequences' = @('level_sequence')
    'unreal-leveldesign'     = @('leveldesign')
    'unreal-localization'    = @('localization')
    # logicdriver is an optional-plugin namespace; when Logic Driver Pro is not
    # loaded the live catalog answers "Unknown namespace: logicdriver", which
    # this tool reports as "skipped: plugin not loaded" (not drift). Mapping it
    # here (rather than to an empty array) keeps that skipped semantics explicit
    # instead of mislabeling it a knowledge-only skill.
    'unreal-logicdriver'     = @('logicdriver')
    'unreal-materials'       = @('material', 'asset')
    'unreal-mesh'            = @('mesh')
    'unreal-metahuman'       = @('metahuman')
    'unreal-modelgen'        = @('modelgen')
    'unreal-ndisplay'        = @('ndisplay')
    'unreal-niagara'         = @('niagara')
    'unreal-paper2d'         = @('paper2d')
    'unreal-pcg'             = @('pcg')
    'unreal-performance'     = @('config', 'material', 'niagara')
    'unreal-project-search'  = @('project')
    'unreal-reflection-intel' = @('cppreflect', 'network', 'decision', 'risk', 'reflect')
    'unreal-scene'           = @('scene')
    'unreal-slate'           = @('slate')
    'unreal-source-control'  = @('source_control')
    'unreal-sprite'          = @('sprite')
    'unreal-ui'              = @('ui')
    'unreal-water'           = @('water')
    'unreal-world-conditions' = @('world_conditions')
    'unreal-worldgen'        = @('worldgen')
    # KNOWLEDGE-only skills drive no namespace.
    'material-reference'     = @()
    'niagara-reference'      = @()
}

# --- Live catalog access (LIVE mode) ------------------------------------------
function Test-McpUp {
    $healthUrl = $McpUrl -replace '/mcp/?$', '/health'
    try {
        $resp = Invoke-WebRequest -Uri $healthUrl -Method Get -TimeoutSec 3 -UseBasicParsing
        return ($resp.StatusCode -eq 200)
    }
    catch { return $false }
}

# Returns a normalized namespace catalog:
#   @{ Status = 'ok'|'not_installed'|'unknown'|'error'; Actions = @{ name -> @{ params = @{ name -> @{ required = $bool } } } }; Raw = <text> }
# 'not_installed'/'unknown' both mean the plugin is not loaded (skipped, not drift).
function ConvertTo-Catalog {
    param($Json)
    $catalog = @{ Status = 'ok'; Actions = @{} }
    if ($null -eq $Json) { $catalog.Status = 'error'; return $catalog }
    if ($Json.PSObject.Properties['status'] -and $Json.status -eq 'not_installed') {
        $catalog.Status = 'not_installed'
        return $catalog
    }
    # 'actions' is an array on a loaded namespace, or the integer 0 on not_installed.
    if (-not ($Json.PSObject.Properties['actions'])) { $catalog.Status = 'error'; return $catalog }
    if ($Json.actions -isnot [System.Array]) {
        # integer 0 (not_installed sentinel without an explicit status field)
        $catalog.Status = 'not_installed'
        return $catalog
    }
    foreach ($action in $Json.actions) {
        $name = [string]$action.action
        if (-not $name) { continue }
        $params = @{}
        if ($action.PSObject.Properties['params'] -and $action.params) {
            foreach ($p in $action.params.PSObject.Properties) {
                $required = $false
                if ($p.Value -and $p.Value.PSObject.Properties['required']) {
                    $required = [bool]$p.Value.required
                }
                $params[$p.Name] = @{ Required = $required }
            }
        }
        $catalog.Actions[$name] = @{ Params = $params }
    }
    return $catalog
}

function Get-LiveCatalog {
    param([string]$Namespace)
    $argsJson = @{ namespace = $Namespace } | ConvertTo-Json -Compress
    $body = '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"monolith_discover","arguments":' + $argsJson + '}}'
    try {
        $resp = Invoke-RestMethod -Uri $McpUrl -Method Post -ContentType 'application/json' `
            -Headers @{ Accept = 'application/json, text/event-stream' } -Body $body -TimeoutSec 30
    }
    catch {
        return @{ Status = 'error'; Actions = @{} }
    }
    $text = $null
    if ($resp.result -and $resp.result.content -and $resp.result.content.Count -gt 0) {
        $text = [string]$resp.result.content[0].text
    }
    if ($resp.result -and $resp.result.isError) {
        # "Unknown namespace: <ns>" => plugin not loaded / not registered.
        if ($text -match '(?i)unknown namespace') { return @{ Status = 'unknown'; Actions = @{} } }
        return @{ Status = 'error'; Actions = @{} }
    }
    if (-not $text) { return @{ Status = 'error'; Actions = @{} } }
    $json = $null
    try { $json = $text | ConvertFrom-Json } catch { return @{ Status = 'error'; Actions = @{} } }
    return ConvertTo-Catalog -Json $json
}

function Get-OfflineCatalog {
    param([string]$Namespace)
    $path = Join-Path $DumpDir ("{0}.json" -f $Namespace)
    if (-not (Test-Path -LiteralPath $path)) { return @{ Status = 'no_dump'; Actions = @{} } }
    $json = $null
    try { $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
    catch { return @{ Status = 'error'; Actions = @{} } }
    return ConvertTo-Catalog -Json $json
}

# Catalog cache keyed by namespace (a namespace is shared by several skills).
$script:CatalogCache = @{}
function Resolve-Catalog {
    param([string]$Namespace)
    if ($script:CatalogCache.ContainsKey($Namespace)) { return $script:CatalogCache[$Namespace] }
    $catalog = if ($Offline) { Get-OfflineCatalog -Namespace $Namespace } else { Get-LiveCatalog -Namespace $Namespace }
    $script:CatalogCache[$Namespace] = $catalog
    return $catalog
}

# --- SKILL.md / references parsing --------------------------------------------
# Collect the SKILL.md plus any same-skill references/*.md it links via markdown
# links, so skills that hoisted their action tables out (unreal-mesh,
# unreal-niagara) are still covered.
function Get-SkillDocFiles {
    param([System.IO.DirectoryInfo]$SkillDir)
    $files = New-Object System.Collections.Generic.List[string]
    $skillMd = Join-Path $SkillDir.FullName 'SKILL.md'
    if (-not (Test-Path -LiteralPath $skillMd)) { return $files }
    $files.Add($skillMd) | Out-Null
    $content = Get-Content -LiteralPath $skillMd -Raw
    $linkRegex = [regex]'\]\(([^)]+)\)'
    foreach ($m in $linkRegex.Matches($content)) {
        $target = $m.Groups[1].Value.Trim()
        $hash = $target.IndexOf('#'); if ($hash -ge 0) { $target = $target.Substring(0, $hash) }
        if (-not $target) { continue }
        if ($target -notmatch '\.md$') { continue }
        if ($target -match '^[A-Za-z][A-Za-z0-9+.-]*://') { continue }
        if ($target -match '^[\\/]' -or $target -match '^[A-Za-z]:[\\/]') { continue }
        $resolved = Join-Path $SkillDir.FullName ($target -replace '/', [System.IO.Path]::DirectorySeparatorChar)
        if ((Test-Path -LiteralPath $resolved) -and -not ($files -contains (Resolve-Path -LiteralPath $resolved).Path)) {
            $files.Add((Resolve-Path -LiteralPath $resolved).Path) | Out-Null
        }
    }
    return $files
}

# Split a markdown table row into trimmed cells (honoring escaped pipes).
function Split-TableCells {
    param([string]$Line)
    return ($Line.Trim() -replace '^\|', '' -replace '\|$', '') -split '(?<!\\)\|' | ForEach-Object { $_.Trim() }
}

# True for the separator row `|---|---|`.
function Test-SeparatorRow {
    param([string]$Line)
    return ($Line -match '^\s*\|[\s:|-]+\|?\s*$')
}

# A documented param token carries an explicit notation marker so prose
# backticks are never mistaken for a param. The token must be a STANDALONE
# single-token backtick (no spaces, no `{`/`[`/`(`) shaped as an identifier with
# a marker and/or a default. Returns @{ Name; Required } or $null.
#   name*            -> required
#   name?            -> optional
#   name=default     -> optional (has a default)
#   name?=default    -> optional (marker + default; the dominant form, 400+ uses)
# The `*` required marker never combines with `=` (a required param has no
# default); `name*=...` is treated as malformed and ignored.
function ConvertTo-DocumentedParam {
    param([string]$Token)
    $t = $Token.Trim()
    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*)\*$') { return @{ Name = $Matches[1]; Required = $true } }
    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*)\?(=\S*)?$') { return @{ Name = $Matches[1]; Required = $false } }
    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*)=\S*$') { return @{ Name = $Matches[1]; Required = $false } }
    return $null
}

# Extract the LEADING run of param-shaped standalone backticks from a params
# cell. Skill param cells list params as a comma/space-separated run of
# backticks at the start of the cell; the first intervening prose fragment
# (`(`, `(alias`, `DSL:`, `e.g.`, `if ...`) or non-param backtick ends the run.
# Stopping at the first prose fragment is what keeps documented-but-explanatory
# backticks (`WITH_GBA`, `inventory_supported=false`, nested `{...sort_priority?}`)
# out of the param set, eliminating false (b)/(c) drift. Missing a trailing real
# param only drops a check; it never invents drift.
function Get-LeadingParams {
    param([string]$Cell)
    $params = New-Object System.Collections.Generic.List[hashtable]
    # Tokenize into backtick groups and the literal text between them.
    $matches = [regex]::Matches($Cell, '`([^`]*)`')
    $cursor = 0
    foreach ($m in $matches) {
        $between = $Cell.Substring($cursor, $m.Index - $cursor)
        $cursor = $m.Index + $m.Length
        # Allowed separators between consecutive params: whitespace and commas only.
        if ($between.Trim(@(' ', "`t", ',')) -ne '') { break }
        # A single backtick may pack several comma-separated param tokens.
        $tokens = $m.Groups[1].Value -split ','
        $tokenOk = $true
        $rowParams = New-Object System.Collections.Generic.List[hashtable]
        foreach ($tok in $tokens) {
            $p = ConvertTo-DocumentedParam -Token $tok.Trim()
            if (-not $p) { $tokenOk = $false; break }
            $rowParams.Add($p) | Out-Null
        }
        if (-not $tokenOk) { break }   # first non-param backtick ends the leading run
        foreach ($p in $rowParams) { $params.Add($p) | Out-Null }
    }
    return $params
}

# Normalize a documented action token to a bare live-catalog action name.
# Strips an inline `[w]` marker (leading or trailing) and a `<ns>_query("action")`
# wrapper. Returns '' if the token is not a bare action identifier.
function ConvertTo-ActionName {
    param([string]$Token)
    $t = $Token.Trim()
    $t = $t -replace '^\[w\]\s*', ''
    $t = $t -replace '\s*\[w\]$', ''
    if ($t -match '_query\(\s*"([^"]+)"') { return $Matches[1] }
    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*)$') { return $t }
    return ''
}

# Extract documented actions (with their leading params) from one skill's doc
# files. A table is treated as an ACTION table only when its header row's first
# column is exactly `Action` or `Tool` — this is the precise discriminator that
# excludes node-type tables (`node_type`), property tables, and example/workflow
# tables. The PARAMS column is the column whose header contains "Param" or
# "Signature"; if there is no such column, only action presence is recorded.
function Get-DocumentedActions {
    param([string[]]$DocFiles)
    $documented = @{}   # actionName -> @{ Params = @(@{Name;Required}) }
    foreach ($file in $DocFiles) {
        $lines = @(Get-Content -LiteralPath $file)
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            if ($line -notmatch '^\s*\|') { continue }
            if (Test-SeparatorRow -Line $line) { continue }
            # Header row candidate: next non-empty line is a separator row.
            if ($i + 1 -ge $lines.Count -or -not (Test-SeparatorRow -Line $lines[$i + 1])) { continue }
            $header = Split-TableCells -Line $line
            if ($header.Count -lt 1) { continue }
            $firstHeader = ($header[0] -replace '`', '').Trim()
            if ($firstHeader -notmatch '^(?i)(action|tool)$') { continue }   # not an action table
            # Locate the params column (header contains Param or Signature).
            $paramsCol = -1
            for ($c = 1; $c -lt $header.Count; $c++) {
                if ($header[$c] -match '(?i)param|signature') { $paramsCol = $c; break }
            }
            # Consume the table body until a non-row line.
            $r = $i + 2
            while ($r -lt $lines.Count -and $lines[$r] -match '^\s*\|') {
                if (-not (Test-SeparatorRow -Line $lines[$r])) {
                    $cells = Split-TableCells -Line $lines[$r]
                    if ($cells.Count -ge 1) {
                        # Action is the first backticked identifier in column 0.
                        $action = ''
                        foreach ($bt in [regex]::Matches($cells[0], '`([^`]+)`')) {
                            $candidate = ConvertTo-ActionName -Token $bt.Groups[1].Value
                            if ($candidate) { $action = $candidate; break }
                        }
                        if ($action) {
                            if (-not $documented.ContainsKey($action)) {
                                $documented[$action] = @{ Params = (New-Object System.Collections.Generic.List[hashtable]) }
                            }
                            if ($paramsCol -ge 0 -and $paramsCol -lt $cells.Count) {
                                foreach ($p in (Get-LeadingParams -Cell $cells[$paramsCol])) {
                                    $documented[$action].Params.Add($p) | Out-Null
                                }
                            }
                        }
                    }
                }
                $r++
            }
            $i = $r - 1
        }
    }
    return $documented
}

# --- Drift evaluation per skill -----------------------------------------------
function Test-SkillDrift {
    param(
        [string]$SkillName,
        [string[]]$Namespaces,
        [System.IO.DirectoryInfo]$SkillDir,
        [hashtable]$GlobalActions
    )

    $result = [PSCustomObject]@{
        Skill          = $SkillName
        Status         = 'checked'   # checked | no_namespace | skipped | blocked
        Detail         = ''
        HardDrift      = New-Object System.Collections.Generic.List[string]
        Gated          = New-Object System.Collections.Generic.List[string]
        XRef           = New-Object System.Collections.Generic.List[string]
        Undocumented   = New-Object System.Collections.Generic.List[string]
        CheckedActions = 0
    }

    if ($Namespaces.Count -eq 0) {
        $result.Status = 'no_namespace'
        $result.Detail = 'knowledge-only skill; no backing namespace'
        return $result
    }

    # Build the union live catalog across the skill's namespaces, and track which
    # namespaces were skipped (not loaded) vs blocked (transport/dump error).
    $liveActions = @{}            # actionName -> @{ params... } (first namespace wins; names are unique per registry)
    $skippedNs = @()
    $blockedNs = @()
    $okNs = @()
    foreach ($ns in $Namespaces) {
        $catalog = Resolve-Catalog -Namespace $ns
        switch ($catalog.Status) {
            'ok' {
                $okNs += $ns
                foreach ($k in $catalog.Actions.Keys) {
                    if (-not $liveActions.ContainsKey($k)) { $liveActions[$k] = $catalog.Actions[$k] }
                }
            }
            'not_installed' { $skippedNs += $ns }
            'unknown'       { $skippedNs += $ns }
            'no_dump'       { $skippedNs += $ns }   # offline: no dump => skip, not drift
            default         { $blockedNs += $ns }
        }
    }

    # If EVERY namespace is skipped (optional plugin not loaded / no dump), the
    # skill is "skipped: plugin not loaded" — never drift.
    if ($okNs.Count -eq 0 -and $blockedNs.Count -eq 0) {
        $result.Status = 'skipped'
        $result.Detail = ('plugin not loaded (' + ($skippedNs -join ',') + ')')
        return $result
    }
    if ($okNs.Count -eq 0 -and $blockedNs.Count -gt 0) {
        $result.Status = 'blocked'
        $result.Detail = ('catalog unavailable for ' + ($blockedNs -join ','))
        return $result
    }

    $documented = Get-DocumentedActions -DocFiles (Get-SkillDocFiles -SkillDir $SkillDir)
    $result.CheckedActions = $documented.Count

    foreach ($actionName in ($documented.Keys | Sort-Object)) {
        # Resolve to the live-catalog key. The monolith-mcp skill documents both
        # the standalone MCP tools (`monolith_find`/`monolith_discover`/...) and
        # the `monolith` admin-namespace actions (`find`/`discover`/...); the
        # `monolith_`-prefixed form resolves to the bare namespace action.
        $resolved = $actionName
        if (-not $liveActions.ContainsKey($resolved) -and $resolved -match '^monolith_(.+)$' -and $liveActions.ContainsKey($Matches[1])) {
            $resolved = $Matches[1]
        }
        if (-not $liveActions.ContainsKey($resolved)) {
            # Absent from the skill's OWN namespaces. If it exists ANYWHERE in the
            # live catalog it is a deliberate cross-namespace reference (e.g.
            # unreal-cpp pointing at `cppreflect` actions), reported as XREF —
            # informational, never failure. Only true whole-catalog absence is
            # (a) hard drift.
            if ($GlobalActions -and ($GlobalActions.ContainsKey($resolved) -or ($resolved -match '^monolith_(.+)$' -and $GlobalActions.ContainsKey($Matches[1])))) {
                $result.XRef.Add(("'{0}' documented here but owned by another namespace (cross-reference)" -f $actionName)) | Out-Null
            }
            elseif ($script:GatedMap.ContainsKey($SkillName) -and ($script:GatedMap[$SkillName] -contains $actionName)) {
                # Feature-gated: exists in source, registers only when its flag is
                # on. Informational, not hard drift (CI-gateable).
                $result.Gated.Add(("(gated) action '{0}' absent from default catalog; feature-flagged off, enable to verify [{1}]" -f $actionName, ($okNs -join ','))) | Out-Null
            }
            else {
                # (a) documented but absent from the entire live catalog. Only
                # reached when at least one mapped namespace is loaded (okNs > 0).
                $result.HardDrift.Add(("(a) action '{0}' documented but ABSENT from live catalog [{1}]" -f $actionName, ($okNs -join ','))) | Out-Null
            }
            continue
        }
        $liveParams = $liveActions[$resolved].Params
        foreach ($docParam in $documented[$actionName].Params) {
            if (-not $liveParams.ContainsKey($docParam.Name)) {
                # (b) param name not in schema
                $result.HardDrift.Add(("(b) action '{0}' documents param '{1}' not in live schema" -f $actionName, $docParam.Name)) | Out-Null
                continue
            }
            $schemaRequired = [bool]$liveParams[$docParam.Name].Required
            if ($docParam.Required -ne $schemaRequired) {
                # (c) required/optional mismatch
                $docMark = if ($docParam.Required) { 'required(*)' } else { 'optional(?/=)' }
                $schemaMark = if ($schemaRequired) { 'required' } else { 'optional' }
                $result.HardDrift.Add(("(c) action '{0}' param '{1}' documented {2} but schema is {3}" -f $actionName, $docParam.Name, $docMark, $schemaMark)) | Out-Null
            }
        }
    }

    # (d) live-but-undocumented actions (informational).
    foreach ($liveName in ($liveActions.Keys | Sort-Object)) {
        if (-not $documented.ContainsKey($liveName)) {
            $result.Undocumented.Add($liveName) | Out-Null
        }
    }

    if ($skippedNs.Count -gt 0) {
        $result.Detail = ('checked ' + ($okNs -join ',') + '; skipped not-loaded ' + ($skippedNs -join ','))
    }
    else {
        $result.Detail = ('checked ' + ($okNs -join ','))
    }
    return $result
}

# --- Main ----------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $SkillsRoot -PathType Container)) {
    Write-Output ("RESULT=BLOCKED reason=skills_root_missing path={0}" -f $SkillsRoot)
    exit 3
}

if ($Offline) {
    if (-not $DumpDir) {
        Write-Output 'RESULT=BLOCKED reason=offline_requires_dumpdir detail=pass -DumpDir <dir of <namespace>.json catalog dumps>'
        exit 3
    }
    if (-not (Test-Path -LiteralPath $DumpDir -PathType Container)) {
        Write-Output ("RESULT=BLOCKED reason=dumpdir_missing path={0}" -f $DumpDir)
        exit 3
    }
    Write-Output ("INFO mode=offline dump_dir={0}" -f (Resolve-Path -LiteralPath $DumpDir).Path)
}
else {
    if (-not (Test-McpUp)) {
        Write-Output ("RESULT=BLOCKED reason=mcp_endpoint_down url={0} detail=start the headless editor (Scripts/recover_mcp.ps1) or run with -Offline -DumpDir" -f $McpUrl)
        exit 3
    }
    Write-Output ("INFO mode=live url={0}" -f $McpUrl)
}

$selected = if ($Skill) {
    $Skill | ForEach-Object { $_ } | Where-Object { $SkillNamespaceMap.Contains($_) }
}
else {
    @($SkillNamespaceMap.Keys)
}
if ($Skill) {
    foreach ($s in $Skill) {
        if (-not $SkillNamespaceMap.Contains($s)) { Write-Output ("INFO unknown_skill={0} (not in the skill->namespace map; ignored)" -f $s) }
    }
}

# Global live-action index across every distinct namespace in the map. A
# documented action that is absent from a skill's own namespaces but present
# here is a cross-namespace reference (XREF), not whole-catalog drift.
$globalActions = @{}
$distinctNamespaces = $SkillNamespaceMap.Values | ForEach-Object { $_ } | Where-Object { $_ } | Select-Object -Unique
foreach ($ns in $distinctNamespaces) {
    $catalog = Resolve-Catalog -Namespace $ns
    if ($catalog.Status -eq 'ok') {
        foreach ($k in $catalog.Actions.Keys) { $globalActions[$k] = $true }
    }
}

$results = @()
foreach ($skillName in $selected) {
    $skillDir = Get-Item -LiteralPath (Join-Path $SkillsRoot $skillName) -ErrorAction SilentlyContinue
    if (-not $skillDir) {
        Write-Output ("SKILL={0} STATUS=blocked detail=skill directory not found under {1}" -f $skillName, $SkillsRoot)
        $results += [PSCustomObject]@{ Skill = $skillName; Status = 'blocked'; HardDrift = @('skill directory missing') }
        continue
    }
    $namespaces = @($SkillNamespaceMap[$skillName])
    $r = Test-SkillDrift -SkillName $skillName -Namespaces $namespaces -SkillDir $skillDir -GlobalActions $globalActions
    $results += $r

    switch ($r.Status) {
        'no_namespace' { Write-Output ("SKILL={0} STATUS=no_namespace detail={1}" -f $r.Skill, $r.Detail) }
        'skipped'      { Write-Output ("SKILL={0} STATUS=skipped detail=plugin not loaded ({1})" -f $r.Skill, $r.Detail) }
        'blocked'      { Write-Output ("SKILL={0} STATUS=blocked detail={1}" -f $r.Skill, $r.Detail) }
        default {
            $drift = if ($r.HardDrift.Count -gt 0) { 'DRIFT' } else { 'ok' }
            Write-Output ("SKILL={0} STATUS={1} documented_actions={2} hard_drift={3} gated={4} xref={5} undocumented={6} ({7})" -f `
                    $r.Skill, $drift, $r.CheckedActions, $r.HardDrift.Count, $r.Gated.Count, $r.XRef.Count, $r.Undocumented.Count, $r.Detail)
            foreach ($d in $r.HardDrift) { Write-Output ("  DRIFT {0}" -f $d) }
            foreach ($g in $r.Gated) { Write-Output ("  GATED {0}" -f $g) }
            foreach ($x in $r.XRef) { Write-Output ("  XREF {0}" -f $x) }
            if ($ShowUndocumented -and $r.Undocumented.Count -gt 0) {
                Write-Output ("  INFO-UNDOC {0}" -f ($r.Undocumented -join ', '))
            }
        }
    }
}

$driftedSkills = @($results | Where-Object { $_.HardDrift -and $_.HardDrift.Count -gt 0 })
$blockedSkills = @($results | Where-Object { $_.Status -eq 'blocked' })
$totalDrift = ($driftedSkills | ForEach-Object { $_.HardDrift.Count } | Measure-Object -Sum).Sum
if (-not $totalDrift) { $totalDrift = 0 }

if ($blockedSkills.Count -gt 0) {
    Write-Output ("RESULT=BLOCKED skills={0} detail=catalog unavailable; resolve before trusting the drift result" -f (($blockedSkills.Skill) -join ','))
    exit 3
}

$totalGated = ($results | ForEach-Object { if ($_.PSObject.Properties['Gated']) { $_.Gated.Count } else { 0 } } | Measure-Object -Sum).Sum
if (-not $totalGated) { $totalGated = 0 }

if ($driftedSkills.Count -eq 0) {
    Write-Output ("RESULT=OK skills_checked={0} hard_drift=0 gated={1}" -f (@($results | Where-Object { $_.Status -eq 'checked' }).Count), $totalGated)
    exit 0
}

Write-Output ("RESULT=DRIFT skills={0} hard_drift_findings={1} gated={2}" -f (($driftedSkills.Skill) -join ','), $totalDrift, $totalGated)
if ($ReportOnly) { exit 0 }
exit 2
