# Chooser Bounded Readback and Validation Verification

**Date:** 2026-08-04
**Scope:** Six read-only `chooser` actions in `MonolithAnimation`
**Engines:** Unreal Engine 5.7 and 5.8
**Status:** Pass

---

## 1. Goal

Add a bounded, non-mutating preflight surface for `UChooserTable` assets without duplicating the existing authoring implementation. One reflection-first serializer, reference walker, and structural validator serve all six endpoints.

| Action | Contract |
|---|---|
| `list_chooser_tables` | Stable AssetRegistry discovery with exact package-prefix filtering and bounded pagination |
| `get_chooser_table` | Counts, bounded columns/references, fallback data, and opt-in row readback |
| `list_chooser_columns` | Bounded reflected column metadata and row-value alignment |
| `list_chooser_rows` | Bounded row pages with explicit cell truncation metadata |
| `list_chooser_references` | Stable hard/soft reference pages with exact object-existence evidence and scan-completeness metadata |
| `validate_chooser_table` | Read-only structural/reference validation; errors invalidate, warnings do not |

The existing `validate_chooser` action still performs a compile-oriented validation pass. `validate_chooser_table` is deliberately separate and never compiles, saves, transacts, mutates, or dirties the package.

---

## 2. Bounds and failure semantics

| Surface | Limit / behavior |
|---|---|
| Tables | 1,000 per response |
| Rows | 500 per response |
| Columns | 512 |
| Collected references | 4,096 |
| Reference depth | 12 |
| Global reference traversal | 65,536 property/element visits |
| Reflected value depth | 3 |
| Reflected struct fields | 128 full / 16 compact |
| Reflected container entries | 256 full / 8 compact |
| Exported text | 4,096 characters |

Canonical mounted package or top-level object paths are required. Filesystem paths, relative paths, subobjects, redirectors, whitespace aliases, case-only aliases, and package/object leaf mismatches return invalid-parameter errors instead of being normalized or substituted. Any depth, visit, result, row, column, field, or container cutoff is surfaced as explicit truncation/completeness metadata.

---

## 3. Build verification

Both engine roots were resolved from each isolated host project's `EngineAssociation`; no engine path is encoded in source or scripts.

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\04-chooser\Logs\UE57\Log.txt` |
| UE 5.8 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\04-chooser\Logs\UE58\Log.txt` |
| UE 5.7 | Chooser-enabled `UnrealEditor Win64 Development` target | Pass after the enabled lane exposed and the source fixed one missing explicit test-helper qualification; final incremental compile/link 4/4, result succeeded | `D:\P4\MonolithValidation20260804\04-chooser\Logs\ChooserEnabledBuild-UE57-rerun.log` |
| UE 5.8 | Chooser-enabled `UnrealEditor Win64 Development` target | Pass, 436/436 actions, result succeeded | `D:\P4\MonolithValidation20260804\04-chooser\Logs\ChooserEnabledBuild-UE58-rerun.log` |
| UE 5.7 | Namespace-policy follow-up `UnrealEditor Win64 Development` target | Pass; implementation and focused tests compiled independently, module linked, result succeeded | `D:\P4\MonolithValidation20260804\04-chooser\Logs\NamespacePolicyBuild-UE57.log` |
| UE 5.8 | Namespace-policy follow-up `UnrealEditor Win64 Development` target | Pass; implementation and focused tests compiled independently, module linked, result succeeded | `D:\P4\MonolithValidation20260804\04-chooser\Logs\NamespacePolicyBuild-UE58.log` |

The packaged-plugin gate proves the optional-dependency-off build remains valid. The Chooser-enabled target gate separately proves the direct `UChooserTable::GetContextData()` path, engine Chooser types, asset fixtures, and enabled-only test code compile on both supported engines.

---

## 4. Automation verification

Each Chooser-enabled host ran:

```text
Automation RunTests Monolith.Chooser.Read
```

| Engine | Tests found | Success | Failed / skipped | Final marker | Evidence |
|---|---:|---:|---:|---|---|
| UE 5.7 | 6 | 6 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\04-chooser\Logs\NamespacePolicyTests-UE57.log` |
| UE 5.8 | 6 | 6 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\04-chooser\Logs\NamespacePolicyTests-UE58.log` |

The six tests cover registration and schemas, strict parameter guards, an empty table, an authored-table round trip, an unresolved soft reference whose package shell remains loaded, and root-context/result-payload validation. Fixture packages remain clean after every readback and validation call.

---

## 5. Static and visual gates

| Gate | Result | Reason |
|---|---|---|
| Named C++ scopes | Pass | New implementation/test helpers use file-stem-derived named namespaces; handlers and tests fully qualify every namespaced symbol, with no anonymous namespace, `using namespace`, or individual `using` declaration |
| Patch hygiene | Pass | `git diff --check` completed without whitespace errors |
| PC 1920x1080 screenshot | N/A | Headless action handlers, schemas, tests, docs, and routing guidance have no visual surface |
| Discord screenshot upload | N/A | No relevant visual artifact; `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked |

---

## 6. Result

Pass. The `chooser` namespace gains six cohesive read-only actions backed by one bounded implementation. Both supported engines compile the optional-dependency-off and Chooser-enabled forms, and both enabled hosts execute all six focused tests without failure or skip.
