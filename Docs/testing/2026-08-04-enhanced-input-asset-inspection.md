# Enhanced Input Asset Inspection Verification

**Date:** 2026-08-04
**Scope:** Five read-only `input` actions in `MonolithGAS`
**Engines:** Unreal Engine 5.7 and 5.8
**Status:** Pass

---

## 1. Goal

Provide reusable Enhanced Input asset preflight without bringing the fork's mutation, transaction, or save surface into this PR.

| Action | Verified contract |
|---|---|
| `list_input_actions` | Stable bounded AssetRegistry page; details load only returned assets |
| `get_input_action` | Exact type-checked action metadata with bounded trigger/modifier arrays |
| `list_input_mapping_contexts` | Stable bounded context page and per-context mapping cap |
| `get_input_mapping_context` | Independent stable mapping pagination |
| `validate_input_mappings` | Missing-action/invalid-key errors, duplicate-key warnings, and explicit completeness |

The namespace advertises `readOnlyHint=true` and `idempotentHint=true`. It registers before the GAS settings gate and remains available when GAS authoring is disabled.

---

## 2. Bounds and failure semantics

| Surface | Limit / behavior |
|---|---|
| Asset page | 1–1,000; default 200 |
| Mapping read page | 1–500; default 100 |
| Input Action trigger/modifier array | 256 per array |
| Mapping trigger/modifier array | 64 per array |
| Validation context page | 1–1,000; default 200 |
| Explicit context paths | 1–1,000, unique |
| Validation mapping scan | 1–10,000 per context; default 4,096 |

Canonical mounted package or matching top-level object paths are required. Filesystem paths, subobjects, leading/trailing whitespace, and mismatched package/object leaves return invalid-parameter errors. `context_paths` and `path` are mutually exclusive. No path, asset, or result fallback is substituted.

Every list result reports `total`, `offset`, `limit`, `count`, and `has_more`. Mapping pages and instanced-object arrays expose their own counts and truncation. Validation separates mapping-scan `page_complete` from pagination-level `all_contexts_covered`; a cutoff on either axis makes global `complete=false` and prevents `valid=true`.

---

## 3. Build verification

Both engine roots were resolved from isolated host `.uproject` `EngineAssociation` values through launcher installation metadata; no engine path is encoded in source or scripts.

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\05-input\Logs\UE57\Log.txt` |
| UE 5.8 | `RunUAT BuildPlugin -NoTargetPlatforms -Rocket` | Pass, 436/436 actions, UAT exit 0 | `D:\P4\MonolithValidation20260804\05-input\Logs\UE58\Log.txt` |
| UE 5.7 | Final `UnrealEditor Win64 Development` rebuild after completeness hardening | Pass, 436/436 actions, UBT exit 0 | `D:\P4\MonolithValidation20260804\05-input\Logs\FinalBuild-UE57.log` |
| UE 5.8 | Final `UnrealEditor Win64 Development` rebuild after completeness hardening | Pass, 436/436 actions, UBT exit 0 | `D:\P4\MonolithValidation20260804\05-input\Logs\FinalBuild-UE58.log` |
| UE 5.7 | Namespace-policy follow-up `UnrealEditor Win64 Development` target | Pass; implementation and focused tests compiled independently, module linked, result succeeded | `D:\P4\MonolithValidation20260804\05-input\Logs\NamespacePolicyBuild-UE57.log` |
| UE 5.8 | Namespace-policy follow-up `UnrealEditor Win64 Development` target | Pass; implementation and focused tests compiled independently, module linked, result succeeded | `D:\P4\MonolithValidation20260804\05-input\Logs\NamespacePolicyBuild-UE58.log` |

Both gates compiled `MonolithGASInputAssetActions.cpp` and `MonolithGASInputAssetActionsTests.cpp` as individual adaptive-build actions, then linked `MonolithGAS` successfully.

---

## 4. Focused automation

Each packaged-plugin host ran:

```text
Automation RunTests Monolith.Input.Assets
```

| Engine | Tests found | Success | Failed / skipped | Final marker | Evidence |
|---|---:|---:|---:|---|---|
| UE 5.7 | 3 | 3 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\05-input\Logs\NamespacePolicyTests-UE57.log` |
| UE 5.8 | 3 | 3 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithValidation20260804\05-input\Logs\NamespacePolicyTests-UE58.log` |

The tests cover the exact five-action registry/schema surface and dispatcher hints; malformed types, paths, selector combinations, and hard caps; and transient IA/IMC readback, stable asset/mapping/validation pagination, duplicate-key warning semantics, mapping-scan and context-coverage completeness, and package cleanliness.

The first UE 5.7 host launch did not reach tests because the packaged `MonolithBABridge` binary had been linked against the locally installed Blueprint Assist plugin while the host had not enabled it. Enabling that installed plugin reproduced the already-established UE 5.7 host contract; the final run above passed 3/3. MCP auto-start was disabled and first-time indexing was deferred in both final isolated hosts. Because UE 5.7 reused the host from the setup attempt, it performed a no-change incremental index check before the tests; this did not change the 3/3 focused result.

---

## 5. Static and visual gates

| Gate | Result | Reason |
|---|---|---|
| Named C++ scopes | Pass | New source/test helpers use file-stem-derived named namespaces; handlers and tests fully qualify every namespaced symbol, with no anonymous namespace, `using namespace`, or individual `using` declaration |
| Patch hygiene | Pass | `git diff --check` completed without whitespace errors |
| PC 1920x1080 screenshot | N/A | Headless asset inspection handlers, schemas, tests, docs, and routing guidance have no visual surface |
| Discord screenshot upload | N/A | No relevant visual artifact; `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked |

---

## 6. Result

Pass. The `input` namespace adds five cohesive read-only actions with deterministic paging, explicit completeness, no asset mutation path, and verified UE 5.7/5.8 behavior.
