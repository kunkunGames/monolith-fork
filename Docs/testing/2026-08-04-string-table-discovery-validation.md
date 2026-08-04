# StringTable Discovery and Validation Verification

**Date:** 2026-08-04
**Scope:** Four read-only `localization` actions in `MonolithConfig`
**Engines:** Unreal Engine 5.7 and 5.8
**Status:** Pass

---

## 1. Goal

Provide reusable culture and StringTable preflight without importing the fork's mutation, transaction, import/export, dirty, or save surface.

| Action | Verified contract |
|---|---|
| `list_cultures` | Stable bounded culture pagination, explicit-name resolution, derived-culture option, and unresolved-name reporting |
| `list_string_tables` | Canonical Asset Registry discovery; details load only returned assets |
| `get_string_table` | Exclusive stable key cursor plus independent entry, metadata, and text bounds/completeness |
| `validate_string_table` | Deterministic bounded key/source issues, issue pagination, and a fail-closed completeness verdict |

The namespace advertises `readOnlyHint=true` and `idempotentHint=true`. It registers before the config settings gate and remains available when config authoring is disabled.

---

## 2. Bounds and failure semantics

| Surface | Limit / behavior |
|---|---|
| Explicit culture names | Maximum 256 |
| Culture page | 1–500; default 100 |
| StringTable asset page | 1–1,000; default 200 |
| Entry page | 1–1,000; default 200; exclusive `after_key` capped at 4,096 characters |
| Aggregate metadata rows | 0–4,096 per entry page; default 512 |
| Source/metadata value text | 1–65,536 characters per value; default 4,096, with original length and truncation flag |
| Validation scan | 1–10,000 entries; default 4,096 |
| Validation issue page | 1–1,000; default 200 |

Canonical mounted package or matching top-level object paths are required. Filesystem paths, subobjects, leading/trailing whitespace, malformed package paths, and mismatched object leaves return invalid-parameter errors. An asset that is not exactly a `UStringTable` is rejected; no asset or path fallback is substituted.

Every list response exposes `total`, `offset`, `limit`, `count`, and `has_more`. Entry readback separates `has_more_entries`, `all_entries_covered`, `metadata_complete`, and global `complete`. Validation treats a scan cutoff as an error, requires `complete=true` before `valid=true`, and paginates issues without changing the whole-run totals.

---

## 3. Build verification

Both engine roots were resolved from isolated host `.uproject` `EngineAssociation` values through launcher installation metadata; no engine path is encoded in source or scripts.

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 build actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\06-localization\Logs\UE57\Log.txt` |
| UE 5.8 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 build actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\06-localization\Logs\UE58\Log.txt` |
| UE 5.7 | Final `UnrealEditor Win64 Development` rebuild | Pass, 436/436 build actions, UBT exit 0 | `D:\P4\MonolithValidation20260804\06-localization\Logs\FinalBuild-UE57.log` |
| UE 5.8 | Final `UnrealEditor Win64 Development` rebuild | Pass, 436/436 build actions, UBT exit 0 | `D:\P4\MonolithValidation20260804\06-localization\Logs\FinalBuild-UE58.log` |

Both final builds compiled `MonolithLocalizationActions.cpp` and `MonolithLocalizationActionsTests.cpp` as individual actions, then linked `MonolithConfig` successfully.

The first UE 5.7 packaging compile correctly caught the test fixture attempting to mutate through the const `GetStringTable()` accessor. The fixture was corrected to Unreal's versioned mutable accessor, `GetMutableStringTable()`, while the production handlers retained const read access. Final packaged builds above are the post-correction gates.

---

## 4. Focused automation

Each packaged-plugin host ran:

```text
Automation RunTests Monolith.Localization.Read
```

| Engine | Tests found | Success | Failed / skipped | Final marker | Evidence |
|---|---:|---:|---:|---|---|
| UE 5.7 | 3 | 3 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\06-localization\Logs\Localization-UE57-final.log` |
| UE 5.8 | 3 | 3 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\06-localization\Logs\Localization-UE58-final.log` |

The tests cover the exact four-action registry/schema surface and dispatcher hints; malformed types, paths, cursors, offsets, and hard caps; transient StringTable discovery/readback; entry cursor continuation; metadata and text truncation; validation issue pagination and completeness; and package cleanliness.

The first focused run in each engine deliberately exposed an invalid assumption in the proposed case-insensitive duplicate check: setting `Case` and `case` does not preserve two simultaneous rows under Unreal's `FTextKey` identity. The unreachable check and its misleading test were removed instead of fabricating a warning from data the engine does not retain. Those failed discovery runs remain at `Localization-UE57.log` and `Localization-UE58.log`; the final evidence above is 3/3 in both engines.

---

## 5. Static and visual gates

| Gate | Result | Reason |
|---|---|---|
| Named C++ scopes | Pass | New production/test helpers use file-stem-derived named namespaces; no anonymous namespace, `using namespace`, or individual `using` declaration was added |
| Read-only production surface | Pass | Production localization source contains no transaction, `Modify`, dirty, create/delete, mutable StringTable, or save call; exactly four actions register |
| Patch hygiene | Pass | `git diff --check` completed without whitespace errors |
| PC 1920x1080 screenshot | N/A | Headless culture/StringTable read handlers, schemas, tests, docs, and routing guidance have no visual surface |
| Discord screenshot upload | N/A | No relevant visual artifact; `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked |

---

## 6. Result

Pass. The `localization` namespace adds four cohesive read-only actions with deterministic bounds, explicit completeness, no mutation path, and verified UE 5.7/5.8 behavior.
