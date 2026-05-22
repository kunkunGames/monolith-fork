# Monolith — Fuzzy Search/Suggest Consolidation & `asset.find_assets`

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithCore` (shared `FMonolithFuzzyMatch` engine), `MonolithAsset` (`asset.find_assets` action)
**Namespace:** `asset` (new action), `monolith` (refactored)
**MCP tool:** `asset_query`, `monolith_find`
**Status:** Implemented (2026-05-21) — Phase 1 (`FMonolithFuzzyMatch` engine + `monolith.find`/`FindSimilarActions` refactor) built and verified (all `Monolith.Core` automation tests pass, incl. 6 new `FuzzyMatch` tests + `FindSimilarActions` parity); Phase 2 (`asset.find_assets`) built and registered. Phase 3 (`FMonolithDidYouMean`) intentionally not done (YAGNI).

---

## 1. Scope & Goal

Consolidate the plugin's duplicated fuzzy/distance **primitives** into one engine, then add `asset.find_assets` (fuzzy, scored, typo-tolerant search over the **live `AssetRegistry`**) as a thin consumer of it.

Guiding rule: **unify only the genuinely identical primitives (text normalization, tokenization, edit distance, weighted token scoring); keep different policies and different algorithms separate.** Two of the three current "CC-05" sites (`monolith_find`, `FindSimilarActions`) plus the new `asset.find_assets` share one fuzzy/distance core but keep their own field sets, weights, and thresholds. The third site, `FindAssetCandidates`, is an **exact-`AssetName` disambiguator** — a different algorithm that uses no edit distance or tokenization — so it is intentionally left out of the merge rather than forced through a shared shell. Copying the scoring math into `MonolithAsset` is rejected; so is "false reuse" that hides divergent behavior behind one over-parameterized entry point.

## 2. Current Fragmentation (verified)

| Corpus ＼ Intent | Search (rich, agent-initiated) | Suggest (bare, error-path) |
|------------------|--------------------------------|----------------------------|
| **Action registry** | `monolith_find` — `MonolithCoreTools.cpp` `ScoreFindTokens` (L263) + `FindTokenEditDistanceBounded` (L197, **bounded, case-sensitive**) | `FMonolithToolRegistry::FindSimilarActions` — `MonolithToolRegistry.cpp` (L1154) + `LevenshteinDistance` (L36, **unbounded, case-insensitive — separate impl**) |
| **AssetRegistry** | `asset.find_assets` (this spec — not yet implemented) | `FMonolithAssetUtils::FindAssetCandidates` — `MonolithAssetUtils.cpp` (L280): **exact `AssetName` equality + path-hint substring** (no edit distance; a distinct algorithm) |

Symptoms:

- **Two Levenshtein implementations** with different semantics: `FindTokenEditDistanceBounded` (bounded, case-sensitive on pre-normalized lowercase) vs `LevenshteinDistance` (unbounded, case-insensitive per-char).
- **Two re-derived fuzzy scorers** over the action corpus: token-weighted (find) and prefix+distance (similar actions).
- Normalization/tokenization re-derived per site.
- `FindAssetCandidates` is a **separate exact-match algorithm** (no edit distance/tokenization), not a duplicate of the fuzzy primitives — included here for context, not as a merge target.
- The consumer decoration pattern (`1 candidate → WithRetryWith`, `N → WithDidYouMean`) is hand-coded in module `Load` helpers, e.g. `MonolithGASAbilityActions.cpp:162-172`.

## 3. Target Architecture — 1 Shared Engine, Thin Per-Consumer Policies

```
Layer 0  FMonolithFuzzyMatch  (MonolithCore Public) — the ONLY shared code
         NormalizeText · Tokenize(text, alias?)
         EditDistanceBounded(a, b, maxDistance, bCaseInsensitive, bAllowTransposition=false)
         · optional Damerau adjacent-transposition support
         IsTypoMatch · ScoreTokens
         ScoreCandidate(query, fields[]) -> { score, reasons, matchedTokens, bestDistance }   (per-field only)

Consumers  Each owns its corpus, field set, weights, thresholds, and output shape.
           They depend on Layer 0; they do NOT share a corpus adapter with each other.
         · monolith_find       (MonolithCore/MonolithCoreTools)    — action corpus, Search
         · FindSimilarActions  (MonolithCore/MonolithToolRegistry) — action corpus, Suggest (drops its private Levenshtein)
         · asset.find_assets   (MonolithAsset)                     — asset corpus, Search (NEW)

Untouched  FindAssetCandidates (MonolithCore/MonolithAssetUtils) — exact-AssetName disambiguator.
           Distinct algorithm (no edit distance / no tokenization); not part of the merge.
```

**What is shared vs not:** only Layer 0 is shared. There is **no** cross-corpus or cross-intent adapter — that would be "false reuse" (a shared shell branching into divergent innards via mode flags). The action corpus is used by both `monolith_find` and `FindSimilarActions`, but they keep separate field/weight/threshold policy; sharing the *engine* is enough.

**Dependency direction:** Layer 0 lives in `MonolithCore`, so the two in-core consumers (`monolith_find`, `FindSimilarActions`) reach it directly, and `asset.find_assets` reaches it downstream from `MonolithAsset` (which already depends on `MonolithCore`). No circular dependency, and `MonolithCore` does not grow an asset-search surface.

### 3.1 `FMonolithFuzzyMatch` primitives (extracted, generic)

| From (current static) | Becomes | Notes |
|-----------------------|---------|-------|
| `NormalizeFindText` (Tools) | `NormalizeText` | unchanged behavior |
| `TokenizeFindText` (Tools) | `Tokenize(text, aliasTable?)` | alias table becomes **caller-supplied**; no expansion when omitted |
| `FindTokenEditDistanceBounded` (Tools) + `LevenshteinDistance` (Registry) | `EditDistanceBounded(a, b, maxDistance, bCaseInsensitive, bAllowTransposition=false)` | one banded Levenshtein/optional restricted Damerau-OSA implementation; `maxDistance=MAX_int32` reproduces the unbounded caller |
| `IsFindTypoMatch` (Tools) | `IsTypoMatch` | unchanged gate (≥4 chars, same first char, dist ≤ 1/2) |
| `ScoreFindTokens` (Tools) | `ScoreTokens(query, fieldTokens, fieldText, FMonolithFuzzyWeights, reasonTag, outReasons, outMatched)` | unchanged scoring |
| (per-field phrase-bonus + `ScoreTokens` loop inside `HandleFind`) | `ScoreCandidate(query, TArrayView<FMonolithFuzzyField>)` | **per-field composition only** (see caveat) |

```cpp
struct FMonolithFuzzyWeights { int32 Exact=0, Prefix=0, Contains=0, Fuzzy=0; }; // Fuzzy=0 disables typo tolerance
struct FMonolithFuzzyField   { FString Text; TArray<FString> Tokens; FMonolithFuzzyWeights Weights;
                               int32 ExactPhraseBonus=0, PrefixPhraseBonus=0, ContainsPhraseBonus=0;
                               const TCHAR* ReasonTag=nullptr; };
struct FMonolithFuzzyScore   { int32 Score=0; TArray<FString> Reasons; TArray<FString> MatchedTokens; int32 BestDistance=INT32_MAX; };
```

**`ScoreCandidate` scope (per-field only):** it sums, per field, the phrase bonus + `ScoreTokens`, and returns `{score, reasons, matchedTokens, bestDistance}`. **Cross-field bonuses are caller policy and stay in the caller** — e.g. `monolith_find`'s "all/partial query tokens matched across the combined search text" bonus stays in `HandleFind`, not in `ScoreCandidate`. This keeps the shared composition free of consumer-specific scoring and prevents it from becoming an over-parameterized god-function.

### 3.2 Refactor of the two existing action call sites

- `FMonolithCoreTools::HandleFind` keeps every find-specific policy (phrase bonuses `action_id_exact=220` …, per-field weights, derived metadata/schema text, `all_query_tokens` bonus, `scoring_version="weighted_tokens_v3"`, MCP alias table) but sources normalization/tokenization/`ScoreTokens`/`ScoreCandidate` from `FMonolithFuzzyMatch`.
- `FMonolithToolRegistry::FindSimilarActions` keeps its Suggest policy (prefix/substring score 0/1, then `EditDistanceBounded` with `threshold=max(2, len/2)`, `bCaseInsensitive=true`) but drops its private `LevenshteinDistance`.

Both keep their public signatures and outputs; existing tests are the parity guard.

## 4. `asset.find_assets` Action (MonolithAsset)

### 4.1 Parameters

| Param | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `query` | string | yes | — | Asset name or task text. Empty/whitespace → `-32602`. |
| `path` | string | no | `/Game` | Content path scope. |
| `recursive` | boolean | no | `true` | Recurse subfolders. |
| `class_names` | array | no | (all) | Filter by class names. Alias `class`. Handler must validate every array entry as a string before building `FARFilter.ClassPaths`. |
| `limit` | integer | no | `20` | Range 1–100. |
| `threshold` | integer | no | (none) | Raw minimum score for `scoring_version="asset_fuzzy_v1"`. Not normalized and not portable across future scoring versions. |
| `include_tags` | boolean | no | `false` | Also score selected registry tag values (extra cost); only the whitelist below is eligible. |
| `include_score_breakdown` | boolean | no | `false` | Include `reason`, `matched_tokens`, `distance` per row. |
| `allow_transposition` | boolean | no | `true` | Count adjacent swaps as one typo edit for fuzzy token matching (`crate` ↔ `carte`). Set `false` for strict Levenshtein. Internal request field: `bAllowTransposition`. |

### 4.1.1 Parameter normalization and validation

- `path` must normalize to a mounted Unreal long package path such as `/Game` or another mounted content root. Invalid paths return `-32602` instead of silently falling back to `/Game`.
- `class_names` accepts class object paths (`/Script/Engine.Texture2D`), asset class paths (`/Script/Engine.Blueprint`), and short names (`Texture2D`, `Blueprint`). Short names are resolved before adding `FTopLevelAssetPath` values to `FARFilter.ClassPaths`.
- Unknown `class_names` entries are invalid params (`-32602`) with an `unknown_class_names` array in the error details. Do not ignore misspelled filters.
- `Blueprint` means the `UBlueprint` asset class. It does not mean "any generated class whose parent is a Blueprint class." `WidgetBlueprint`, `AnimBlueprint`, and similar specialized asset classes must be requested explicitly or reached through `bRecursiveClasses` when Unreal's class hierarchy supports it.
- `class` is a schema alias for `class_names`; the existing `FParamSchemaBuilder` alias flow rewrites it before handler validation.
- Schema type strings must stay within the current validator vocabulary (`array`, `integer`, `boolean`, etc.). Element-level checks for `class_names` are handler-owned unless the shared validator is extended in the same change.

Validation details:

| Param | Accepted | Invalid when | Error data |
|-------|----------|--------------|------------|
| `query` | Non-empty string after trim. | Missing, non-string, or whitespace-only. | `param="query"`, `reason="missing_or_empty"` or `reason="wrong_type"`. |
| `path` | Mounted long package path. Normalize trailing `/` away, including `/Game/` -> `/Game`. | Non-string, empty, not a valid long package path, or not mounted. | `param="path"`, `reason="invalid_package_path"`, `path="<input>"`. |
| `recursive` | Boolean. | Present but not boolean. | `param="recursive"`, `reason="wrong_type"`. |
| `class_names` | Array of non-empty strings; alias `class` is accepted before validation. | Non-array, empty string element, non-string element, or unknown class name. | `param="class_names"`, `reason`, plus `unknown_class_names` when applicable. |
| `limit` | Integer 1..100. | Non-integer, below 1, above 100. | `param="limit"`, `reason="out_of_range"`, `min=1`, `max=100`. |
| `threshold` | Optional integer >= 0. | Non-integer or negative. | `param="threshold"`, `reason="out_of_range"`, `min=0`. |
| `include_tags` | Boolean. | Present but not boolean. | `param="include_tags"`, `reason="wrong_type"`. |
| `include_score_breakdown` | Boolean. | Present but not boolean. | `param="include_score_breakdown"`, `reason="wrong_type"`. |
| `allow_transposition` | Boolean. | Present but not boolean. | `param="allow_transposition"`, `reason="wrong_type"`. |

Invalid params use `FMonolithActionResult::Error(Message, FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data)`. In JSON-RPC this becomes `error.code=-32602` with `error.data`; in structured tool content it appears as `error_code=-32602` with `error_data`.

Recommended message format: `Invalid parameter '<param>': <short reason>.`

Example unknown class error data:

```json
{
  "param": "class_names",
  "reason": "unknown_class_names",
  "unknown_class_names": ["Texture22"],
  "hint": "Pass a UClass name (e.g. Texture2D, Blueprint, StaticMesh) or a full /Script/Module.ClassName path."
}
```

### 4.1.2 Class filter resolution

`class_names` resolution is deterministic, **reuses the project's existing class-resolution convention**, and does not load assets.

For each trimmed entry:

1. If the entry is a full class path (`/Script/Module.ClassName` or `/Game/Path/Asset.ClassName`), resolve it directly with `FindObject<UClass>(nullptr, *Entry)`.
2. Otherwise resolve it as a class name with `FindFirstObject<UClass>(*Entry, EFindFirstObjectOptions::NativeFirst)` — the same pattern already used across `MonolithAnimation` / `MonolithBlueprint`. Asset classes are native `UClass` objects that are always loaded in an editor session, so this resolves without loading any asset. `UClass::GetName()` carries no `U`/`A` prefix, so `Texture2D`, `Blueprint`, `StaticMesh` resolve as written.
3. From the resolved `UClass`, take `GetClassPathName()` (an `FTopLevelAssetPath`) and add it to `FARFilter.ClassPaths`; set `bRecursiveClasses=true`.
4. If resolution returns null, reject the entry as `unknown_class_names` (`-32602`).

This intentionally avoids a hand-maintained short-name→path table: the engine's `UClass` registry is the single source of truth, so new/plugin asset classes work without spec edits, and the resolver stays consistent with the rest of the codebase. `NativeFirst` disambiguates the rare case of two classes sharing a short name. The previous static table is removed because it duplicated engine-owned knowledge, would rot on every new asset class, and contradicted the consolidation goal.

`Blueprint` resolves to `UBlueprint` and filters assets whose **asset class** is `UBlueprint`; specialized Blueprint asset classes (`WidgetBlueprint`, `AnimBlueprint`) must be passed by their own name and are reached as subclasses only when `bRecursiveClasses` and the class hierarchy support it.

### 4.2 Scoring policy (asset corpus, Search intent)

| Field | Source | Phrase bonus (exact/prefix/contains) | Token weights (Exact/Prefix/Contains/Fuzzy) |
|-------|--------|--------------------------------------|---------------------------------------------|
| Asset name | `FAssetData.AssetName` | 200 / 130 / 90 | 45 / 30 / 16 / 8 |
| Path segments | `PackageName` split `/` | — | 22 / 14 / 8 / 4 |
| Class | `AssetClassPath` short name | 60 / — / — | 25 / 16 / 8 / 0 |
| Tags (opt-in) | selected `TagsAndValues` | — | 10 / 6 / 4 / 0 |

Asset alias table: `tex→texture`, `mat→material`, `bp→blueprint`, `sk→skeletal`, `sm→static mesh`, `mi→material instance`, `abp→animation blueprint`. `scoring_version="asset_fuzzy_v1"` (independent of find's `weighted_tokens_v3`). Transposition tolerance is enabled by default and flows into `FMonolithFuzzyMatch::ScoreCandidate`, so adjacent swaps count as distance 1; `false` preserves plain Levenshtein behavior for callers that want stricter typo matching.

Tag scoring whitelist for `include_tags=true`:

- Text/display metadata: `DisplayName`, `Description`, `Tooltip`.
- Class relationship metadata: `ParentClass`, `NativeParentClass`, `GeneratedClass`, `BlueprintParentClass`.
- Common asset references that are short enough to be useful: `Skeleton`, `PreviewMesh`.

Do not score large source/import payloads, serialized object dumps, file-system source paths, or arbitrary tag keys. Tag values are search hints only; they must never cause asset loading.

Threshold semantics:

- `threshold` is applied after score computation and before `limit`.
- A candidate is dropped only when `score < threshold`; `score == threshold` is kept.
- `threshold` uses raw `asset_fuzzy_v1` points. Clients should not persist thresholds across scoring versions.

### 4.3 Candidate pre-filter & performance

1. `FARFilter.PackagePaths={path}`, `bRecursivePaths=recursive`.
2. `class_names` -> resolved `FTopLevelAssetPath` values in `FARFilter.ClassPaths` (+ `bRecursiveClasses=true`).
3. `IAssetRegistry::GetAssets(Filter, OutData)`.
4. Bound scored set by a scan budget (default ~20000); set `truncated=true` + `scanned_count` when exceeded.

Game-thread, `FAssetData`-only (no asset loading). The cost is **not** comparable to `FindAssetCandidates`' "<10 ms" scan: that does a cheap `FName` equality per asset, whereas `find_assets` tokenizes and weight-scores up to four fields per candidate with optional edit-distance typo matching — materially more work, especially on a broad unfiltered `/Game` query. Mitigations: (a) rely on `FARFilter` (`path`+`class_names`) to shrink the candidate set; (b) gate the expensive `EditDistanceBounded` typo step behind a cheap exact/prefix/contains check so it runs only on candidates lacking a stronger match; (c) measure against the scan budget rather than assuming. Sort order must be deterministic: higher score first, then lower edit distance when present, then shorter object path, then lexicographic object path.

Result counters:

- `filtered_count`: number of `FAssetData` rows returned by `AssetRegistry` after `path` and `class_names` filtering.
- `scanned_count`: number of rows actually scored. This is `min(filtered_count, scan_budget)`.
- `matched_count`: number of scored rows that survived `threshold` before `limit`.
- `count`: number of rows returned in `matches`.
- `truncated`: `true` only when `filtered_count > scan_budget`, meaning scores are partial.
- `limited`: `true` when `matched_count > count`, meaning the caller should raise `limit` for more rows.

### 4.4 Result shape (mirrors `monolith_find`)

```jsonc
{
  "status": "ok", "query": "punchbot", "path": "/Game", "recursive": true,
  "scoring_version": "asset_fuzzy_v1",
  "count": 2, "matched_count": 7, "filtered_count": 1843, "scanned_count": 1843,
  "truncated": false, "limited": true, "scan_budget": 20000,
  "matches": [{
    "object_path": "/Game/AI/PunchBot/BB_PunchBot.BB_PunchBot",
    "package_name": "/Game/AI/PunchBot/BB_PunchBot", "asset_name": "BB_PunchBot",
    "class": "BlackboardData", "class_path": "/Script/AIModule.BlackboardData",
    "score": 245
  }],
  "next_actions": ["asset.inspect_asset"]
}
```

Row fields:

| Field | Always | Notes |
|-------|--------|-------|
| `object_path` | yes | `FAssetData.GetSoftObjectPath().ToString()`. |
| `package_name` | yes | `FAssetData.PackageName.ToString()`. |
| `asset_name` | yes | `FAssetData.AssetName.ToString()`. |
| `class` | yes | `AssetClassPath.GetAssetName().ToString()`. |
| `class_path` | yes | Full `AssetClassPath.ToString()`. |
| `score` | yes | Raw `asset_fuzzy_v1` score. |
| `reason` | only when `include_score_breakdown=true` | Comma-joined reason tags. |
| `matched_tokens` | only when `include_score_breakdown=true` | Deduplicated query tokens that contributed. |
| `distance` | only when `include_score_breakdown=true` | Best edit distance; omit or set `null` if no fuzzy comparison contributed. |
| `score_breakdown` | only when `include_score_breakdown=true` | Object with field-level scores: `asset_name`, `path`, `class`, `tags`. |

No-match behavior is success, not an error: return `status="ok"`, `count=0`, `matched_count=0`, `matches=[]`, and keep the counter fields.

### 4.5 `FindAssetCandidates` is intentionally NOT merged

`FMonolithAssetUtils::FindAssetCandidates` stays **exactly as-is** (signature and implementation). It is an exact-`AssetName` disambiguator — it filters to assets whose `FAssetData.AssetName` equals the parsed short name and ranks them by path hints. It uses no edit distance and no tokenization, so it shares none of the Layer 0 primitives; merging it onto a shared asset-search adapter would be "false reuse" — a common shell wrapping a different filter — and would risk loosening its exact-name guarantee, which is load-bearing for safe auto-retry (`1 candidate → WithRetryWith`). It has a single caller today (`MonolithGASAbilityActions.cpp`), so there is no reuse pressure to justify churn.

`asset.find_assets` (fuzzy) and `FindAssetCandidates` (exact) are deliberately separate algorithms for separate jobs.

### 4.6 Implementation contracts

`asset.find_assets` is self-contained in `MonolithAsset`. It uses Layer 0 (`FMonolithFuzzyMatch`) for scoring and `IAssetRegistry` for enumeration; it does **not** introduce a shared asset-search class in `MonolithCore`. Expose small request/result structs (in `MonolithAsset`) so the handler and tests share one code path:

```cpp
// MonolithAsset-local (e.g. MonolithAssetFindActions.h)
struct FAssetFindRequest
{
    FString Query;
    FString Path = TEXT("/Game");
    bool bRecursive = true;
    TArray<FString> ClassNames;
    int32 Limit = 20;
    TOptional<int32> Threshold;
    bool bIncludeTags = false;
    bool bIncludeScoreBreakdown = false;
    bool bAllowTransposition = true;
    int32 ScanBudget = 20000;
};

struct FAssetFindRow
{
    FString ObjectPath, PackageName, AssetName, ClassName, ClassPath;
    int32 Score = 0;
    FMonolithFuzzyScore Breakdown;       // from Layer 0; surfaced only when bIncludeScoreBreakdown
    TMap<FString, int32> FieldScores;    // asset_name / path / class / tags
};

struct FAssetFindResult
{
    TArray<FAssetFindRow> Matches;
    int32 FilteredCount = 0, ScannedCount = 0, MatchedCount = 0;
    bool bTruncated = false, bLimited = false;
};
```

Internal seams (all in `MonolithAsset`, none load assets):

| Function | Contract |
|----------|----------|
| `RunAssetFind(const FAssetFindRequest&, FAssetFindResult&, FString& OutError, TSharedPtr<FJsonObject>& OutErrorData)` | Validates request, queries `AssetRegistry`, scores via `FMonolithFuzzyMatch`, returns ranked rows. |
| `ResolveClassNames(const TArray<FString>&, TArray<FTopLevelAssetPath>& Out, TArray<FString>& OutUnknown)` | §4.1.2 resolution via `FindObject` / `FindFirstObject<UClass>`; collects unknowns for `-32602`. |
| `BuildFieldsForAsset(const FAssetData&, bool bIncludeTags)` | Converts one asset into `FMonolithFuzzyField[]`; unit-testable, no asset load. |

`asset.find_assets` owns JSON parameter parsing/serialization and all asset-search validation (`path`, class resolution, scan budget, scoring). `FMonolithFuzzyMatch` owns scoring math only.

## 5. Files to Add / Change

| File | Change |
|------|--------|
| `MonolithCore/Public/MonolithFuzzyMatch.h` · `Private/MonolithFuzzyMatch.cpp` | **New.** The shared engine (Layer 0). Needs only `Core`. |
| `MonolithCore/Private/MonolithCoreTools.cpp` | **Edit.** `HandleFind` consumes engine; delete migrated statics. |
| `MonolithCore/Private/MonolithToolRegistry.cpp` | **Edit.** `FindSimilarActions` uses `EditDistanceBounded`; delete private `LevenshteinDistance`. |
| `MonolithAsset/Public/MonolithAssetFindActions.h` · `Private/MonolithAssetFindActions.cpp` | **New.** `asset.find_assets` handler, request/result structs, class resolution, registration. |
| `MonolithAsset/Private/MonolithAssetModule.cpp` | **Edit.** Register find action. |
| `MonolithCore/Private/Tests/MonolithFuzzyMatchTests.cpp` | **New.** Engine unit tests. |
| `MonolithAsset/Private/Tests/FindAssetsTests.cpp` | **New.** Action tests. |

`MonolithCore/Private/MonolithAssetUtils.cpp` (`FindAssetCandidates`) is intentionally **not** in the change set (§4.5). No `Build.cs` changes are expected: `FMonolithFuzzyMatch` is pure text (`Core` only); `MonolithAsset` already depends on `MonolithCore` + `AssetRegistry`. Keep new public headers conservative: the `MonolithCore` engine exposes only string/token/field/score types — it must not pull `AssetRegistry` into its public surface.

## 6. Phased Rollout (low-risk)

| Phase | Content | Guard |
|-------|---------|-------|
| **1. Engine extract + distance unify** | Add `FMonolithFuzzyMatch`; route `HandleFind` through it; replace both Levenshteins with `EditDistanceBounded`. No behavior change. | Existing `monolith.find` tests + `MonolithErrorHintTests` (FindSimilarActions) stay green; golden tests must preserve top action, score version, matched-token/reason categories, and prefix/substring priority. |
| **2. `asset.find_assets`** | Add the action in `MonolithAsset` using Layer 0 for scoring and `IAssetRegistry` for enumeration. `FindAssetCandidates` is **not** touched. | New asset tests; `FindAssetCandidates`/GAS error-path behavior unchanged because it is not modified. |

A consumer-side `FMonolithDidYouMean` helper to centralize the `1→WithRetryWith` / `N→WithDidYouMean` decoration was considered and **dropped as YAGNI**: `FindAssetCandidates` has one caller and `FindSimilarActions` has its own registry wiring. Revisit only if a third call site appears.

## 7. Parity Risks (must preserve)

- `FindSimilarActions`: case-insensitive distance + `threshold=max(2,len/2)` + prefix/substring priority; **not** an exact-name gate. Preserve as Suggest policy.
- `FindAssetCandidates`: **left untouched** by this work; its exact-`AssetName` hard filter remains load-bearing for safe auto-retry.
- `EditDistanceBounded`: expose `bCaseInsensitive` so both prior behaviors are reproducible (find passes pre-normalized lowercase; FindSimilarActions passes `true`).
- `monolith_find`: keep `scoring_version="weighted_tokens_v3"` and preserve the existing alias expansion boundary: query text may expand the MCP alias table, field/schema tokens do not.
- `asset.find_assets`: `threshold` is a Search-only raw score cut; it has no bearing on `FindAssetCandidates` (separate, untouched).

## 8. Verification Gates

| Gate | Requirement |
|------|-------------|
| Build | Primary `GoGameEditor` UBT command succeeds. |
| Engine units | `MonolithFuzzyMatchTests`: `EditDistanceBounded` band/early-out + case modes, `IsTypoMatch`, `ScoreTokens`, `ScoreCandidate`, normalization, alias expansion. |
| `monolith_find` parity | Existing find tests green after refactor. |
| Suggest parity | `MonolithErrorHintTests` (FindSimilarActions) green; GAS asset error-path retry/did-you-mean unchanged. |
| Action tests | `FindAssetsTests`: exact-name top rank, typo (`punchb0t`→`BB_PunchBot`), adjacent transposition on/off (`crate`↔`carte` with `allow_transposition`), short-name and path-form `class_names` filters, unknown class rejection, invalid param error data, no-match success, result row shape with/without score breakdown, `path` scope, `threshold` cut, `include_tags` whitelist, deterministic tie-break, `limit`/`truncated`. |
| Runtime discovery | `monolith_discover({namespace:"asset"})` includes `find_assets` and preserves all pre-existing asset actions. In this checkout the expected count is 12 actions after adding `find_assets` (baseline 11 + 1). |

### 8.1 Test fixture strategy

`FindAssetsTests` must not depend on project content such as `/Game/AI/PunchBot`. Create test assets at runtime under a unique path:

```text
/Game/MonolithTests/AssetFind/<Guid>/
```

Recommended fixture:

- Create at least three transient editor assets with `CreatePackage`, `NewObject<UTexture2D>`, and `FAssetRegistryModule::AssetCreated`.
- Use duplicate asset short names in different folders to verify path scoring and deterministic tie-breaks.
- Use a uniquely named `UTexture2D` for typo and class filter tests; this avoids needing Blueprint factory setup.
- Do not save fixture packages. Delete created objects with `ObjectTools::DeleteObjects` in teardown and run garbage collection.
- If `include_tags` cannot be covered reliably through live fixture assets, unit-test `BuildFieldsForAsset`/tag filtering through a synthetic `FAssetData` or a small adapter seam rather than depending on arbitrary project tags.
- Runtime discovery can use `FMonolithTestSupport::RunRegistryContractCases` for registration-level validation and a live `monolith_discover({namespace:"asset"})` smoke when the editor server is available.

## 9. Out of Scope / Future

- Offline parity for `asset.find_assets` in `monolith_query.exe` (requires live editor `AssetRegistry`; offline asset search stays on `project` FTS5).
- Semantic/embedding search.
- Async/multi-thread scan; first slice is synchronous game-thread.
- Migrating `FindAssetCandidates` onto the shared engine, or a shared `FMonolithDidYouMean` helper — deferred until a concrete second/third consumer justifies it.

## 10. Assumptions

- Action name `find_assets` (plural), consistent with `delete_assets`/`inspect_assets_batch`; no singular alias.
- Only generic primitives are shared; phrase/field/alias/threshold policy stays per-caller, so `monolith_find` and `FindSimilarActions` behavior is byte-for-byte preserved.
- `FindAssetCandidates` is out of scope and unchanged (exact-name disambiguator; not migrated onto the shared engine).
- Class filter resolution reuses `FindFirstObject<UClass>` / `FindObject<UClass>` (project convention), not a hand-maintained short-name table.
- `asset.find_assets` is a live editor action over `AssetRegistry`; it is intentionally separate from offline `project` FTS search.
