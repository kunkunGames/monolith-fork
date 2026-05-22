# Verification — Fuzzy Search Consolidation & `asset.find_assets`

**Date:** 2026-05-22
**Scope:** `MonolithCore` (`FMonolithFuzzyMatch` engine, `monolith.find` + `FindSimilarActions` refactor), `MonolithAsset` (`asset.find_assets`)
**Engine:** UE 5.7 (resolved from `GO.uproject`), Win64 Development editor
**Spec:** [../specs/SPEC_MonolithAssetFind.md](../specs/SPEC_MonolithAssetFind.md)

---

## 1. Build

| Gate | Command | Result |
|------|---------|--------|
| Phase 1 + Phase 2 | `UnrealBuildTool GoGameEditor Win64 Development -Project=GO.uproject -WaitMutex -NoHotReloadFromIDE` | **Succeeded** (exit 0) |

All changed/new translation units compiled with zero compile errors under `-WarningsAsErrors`: `MonolithFuzzyMatch.cpp`, refactored `MonolithCoreTools.cpp` / `MonolithToolRegistry.cpp`, `MonolithAssetFindActions.cpp`, `MonolithAssetModule.cpp`, and both test files. `UnrealEditor-MonolithCore.dll` and `UnrealEditor-MonolithAsset.dll` linked. The loaded `MonolithAsset` module reports **12 asset actions** (11 prior + `find_assets`). The 2026-05-22 rerun for `allow_transposition` also succeeded after opening the MonolithCore/MonolithAsset plugin DLLs for edit in Perforce.

## 2. Automation Results

Headless runner: `UnrealEditor-Cmd GO.uproject -ExecCmds="Automation RunTests <filter>" -TestExit="Automation Test Queue Empty" -nullrhi -unattended`.

### 2.1 Phase 1 — `Monolith.Core` (engine + parity)

| Metric | Value |
|--------|-------|
| Succeeded | 52 |
| Failed | 0 |
| Not run | 0 |

New engine tests (all pass): `Monolith.Core.FuzzyMatch.NormalizeText`, `.Tokenize`, `.EditDistanceBounded`, `.IsTypoMatch`, `.ScoreTokens`, `.ScoreCandidate`. Parity guard (pass): `Monolith.Core.ErrorHints.FindSimilarActions` (and the rest of `ErrorHints`), confirming the `FindSimilarActions` refactor onto `FMonolithFuzzyMatch::EditDistanceBounded` is behavior-preserving.

### 2.2 Phase 2 — `Monolith.Asset.FindAssets`

| Metric | Value |
|--------|-------|
| Succeeded | 2 |
| Failed | 0 |
| Not run | 0 |

| Test | Covers | Result |
|------|--------|--------|
| `ResolveClassNames` | `FindObject`/`FindFirstObject<UClass>` class resolution, full `/Script` paths, unknown-class rejection | Success |
| `Search` | Live `AssetRegistry` fixture search: name ranking, typo tolerance (`punchb0t`→`PunchBot`), adjacent transposition tolerance on/off (`crate`↔`carte`, default on, strict Levenshtein off), `class_names` filter, `threshold` cut, invalid-path rejection with structured error data | Success |

### 2.3 `allow_transposition` rerun

| Gate | Result |
|------|--------|
| `Automation RunTests Monolith.Asset.FindAssets` | **Succeeded** — 2 tests performed (`ResolveClassNames`, `Search`). |
| `Automation RunTests Monolith.Core.FuzzyMatch` | **Succeeded** — 7 tests performed, including `Transposition`. |
| Runtime MCP smoke | `asset.find_assets` on `/Game/MonolithTests/AssetFind` accepted `allow_transposition=false` and alias `bAllowTransposition=false`; both responses echoed `allow_transposition:false`. |

## 3. Outcome

- Three fragmented fuzzy/distance implementations consolidated to one shared `FMonolithFuzzyMatch` engine (one banded Levenshtein, one tokenizer/normalizer, one weighted token scorer). `monolith.find` and `FindSimilarActions` now consume it; behavior preserved.
- `asset.find_assets` added as a thin consumer; `allow_transposition` now controls Damerau adjacent-swap tolerance and defaults to true. `FMonolithAssetUtils::FindAssetCandidates` left exact-name (not merged). Phase 3 (`FMonolithDidYouMean`) intentionally deferred (YAGNI).
- Docs synced: `SPEC_MonolithCore.md`, `SPEC_MonolithAsset.md` (11→12), `API_REFERENCE.md`, `SPEC_MonolithAssetFind.md`.

## 4. Environment Notes

- The full `GoGameEditor` target build requires no editor process holding the Monolith plugin DLLs. During this session a headless editor was repeatedly auto-relaunched (MCP), intermittently locking `UnrealEditor-MonolithAsset.dll` and causing `LNK1104` link failures and one stale-DLL run (`find_assets` absent, 11 actions). Resolution: kill the editor immediately before a link-only UBT pass so the DLL write wins the race, then launch the test editor (which loads the fresh DLL from disk). A clean run is reproducible once no editor holds the DLL.
- The `allow_transposition` rerun also hit `LNK1104` while writing `UnrealEditor-MonolithAsset.dll` / `UnrealEditor-MonolithCore.dll` because those Perforce-tracked plugin DLLs were read-only. `p4 edit` on both DLLs fixed the write permission issue; the next UBT invocation linked successfully.
- An unrelated pre-existing game-code compile error (`Source/GoGame/Private/ViewModel/Node/GoNodeViewModel.cpp:115`, a `const` method passing `this` to `UGoNodeManager::Get(UObject*)`) briefly blocked the target build and was resolved outside this change set (caller switched to `Get(Node)`). No Monolith code was implicated.
