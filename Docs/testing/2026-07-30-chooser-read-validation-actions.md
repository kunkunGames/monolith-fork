# Chooser Read and Validation Actions Verification

**Date:** 2026-07-30
**Branch:** `jules/codex/chooser/read-validation-actions`
**Fork base:** `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Reference source:** `kunkunGames/monolith@a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1`
**Scope:** Six bounded, read-only `chooser` actions owned by `MonolithAnimation`
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Goal

Verify that the public fork gains the practical Chooser preflight and read-back
surface that is missing from its existing ten-action authoring surface. The
accepted implementation must:

- add exactly six `chooser` actions and remove none;
- keep Chooser ownership in `MonolithAnimation`, which already registers and
  unregisters the namespace's authoring actions;
- require canonical package or object paths without normalization, redirects,
  case correction, extension stripping, or short-name substitution;
- keep reflection output depth-, field-, container-, row-, table-, and
  reference-bounded;
- report optional Chooser dependency absence explicitly;
- perform no compile, save, dirtying, or asset mutation;
- compile and pass focused automation on UE 5.7 and UE 5.8; and
- exclude security, benchmark, invocation-log, action-metadata, and
  reinforcement-learning features.

---

## 2. Action Surface

| Action | Contract |
|---|---|
| `chooser.list_chooser_tables` | Lists exact registry-visible Chooser Table identities with an optional canonical package-prefix filter; capped at 1,000 tables. |
| `chooser.get_chooser_table` | Reports table/class/result/context/column/reference summaries and optionally up to 500 bounded rows. |
| `chooser.list_chooser_columns` | Returns bounded reflected column summaries; capped at 512 columns. |
| `chooser.list_chooser_rows` | Pages exact rows with `start_row` and `limit`; each cell uses the local bounded serializer. |
| `chooser.list_chooser_references` | Walks reflected hard/soft object references under explicit depth/container/result bounds. |
| `chooser.validate_chooser_table` | Reports structural errors and non-fatal warnings without compiling, saving, or modifying the table. |

The existing authoring and deep-inspection actions remain unchanged. The final
catalog contains 16 total `chooser` actions: the prior 10 plus these 6.

---

## 3. Fail-Closed Read Contract

| Contract | Result |
|---|---|
| Asset identity | PASS — accepts only canonical `/Mount/Package` or `/Mount/Package.Object` input whose package leaf and object name agree exactly. |
| Rejected input | PASS — relative paths, filesystem paths, backslashes, whitespace, extensions, subobjects, leaf/object mismatch, case mismatch, redirectors, and missing assets do not substitute. |
| Registry filter | PASS — `path_filter` matches an exact package or a slash-delimited descendant; arbitrary string prefixes do not match. |
| Serializer depth | PASS — reflected row/fallback values stop after depth 3. |
| Struct fields | PASS — full values cap at 128 fields; compact column summaries cap at 16. |
| Container values | PASS — full arrays/sets/maps cap at 256 entries; compact summaries cap at 8 and report `count` plus `truncated_after`. |
| Depth boundary | PASS — depth-limited structs and containers return explicit metadata without recursively exporting their contents. |
| String values | PASS — strings, localized text, and otherwise unsupported export text cap at 4,096 characters and return explicit truncation metadata. |
| Table and graph bounds | PASS — columns 512, rows 500 per request, listed tables 1,000, references 4,096, reference depth 12, and container entries per reflected reference property 4,096. |
| Mutation | PASS — handlers do not call compile/save/modify/dirty APIs and the authoring round-trip test reads a nine-row authored table without changing it. |
| Validation semantics | PASS — errors make `valid=false`; warnings remain separately visible and do not invalidate the table. |
| Optional dependency | PASS — builds without Chooser retain the namespace-owned registration surface and return an explicit optional-dependency-unavailable result. |

The read implementation deliberately uses a Chooser-local bounded serializer
instead of the shared general reflection reader. That prevents a large or
cyclic Chooser value graph from escaping the action-specific response limits.

---

## 4. Verification Environment

| Engine | Engine association | Host project | Plugin source |
|---|---|---|---|
| UE 5.7 | `5.7` | `D:\P4\MonolithChooserUE57Host\MonolithChooserUE57Host.uproject` | Physical copy under `D:\P4\MonolithChooserUE57Host\Plugins\Monolith` |
| UE 5.8 | `5.8` | `D:\P4\MonolithChooserUE58Host\MonolithChooserUE58Host.uproject` | Physical copy under `D:\P4\MonolithChooserUE58Host\Plugins\Monolith` |

Each engine root was resolved from the corresponding host project's
`EngineAssociation`. The final source, test, module, Build.cs, header, and
descriptor hashes matched the branch worktree before compilation. In
particular:

```text
MonolithChooserReadActions.cpp
SHA256 7C25B778C6451F7AD47A6BA035056AE2EAE1A63678B9CC9D6B7B542C92027DF1

MonolithChooserReadActionsTests.cpp
SHA256 3817F85E78DE1F0BE4963E7F08574702736464A991D32E356B3A3F7A36CD3B46

Monolith.uplugin
SHA256 5D9139060EE41DD0E067D6DB390104B3FB73C668814A5B55F7F24F1A9618F9C8
```

The hosts use independent physical plugin copies. A shared source junction
cannot be used for consecutive cross-version verification because Unreal puts
UHT records, import libraries, and DLLs under the plugin's own
`Binaries`/`Intermediate` directories.

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | Physically isolated editor target build | PASS — the fresh 433-action build compiled the new test and exposed one C2446 in the new reader; the root fix and final response-bound hardening each recompiled and freshly linked `MonolithAnimation` in 4/4 actions with `Result: Succeeded`. | `D:\P4\MonolithChooserUE57Host\Build-UE57-Isolated-20260730-040247.log`; `D:\P4\MonolithChooserUE57Host\Build-UE57-FinalBounded-20260730-042239.log` |
| UE 5.7 | Final affected DLL | PASS — 2,850,304 bytes, SHA-256 `69BF99EE34C9BBDFE5C5E0E096A3B8C6CB270607A9BDBBC540E5BDC5F0955ADF`. | `D:\P4\MonolithChooserUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAnimation.dll` |
| UE 5.8 | Physically isolated full editor target build | PASS — 433/433 actions explicitly compiled the new reader and tests; the final response-bound hardening then recompiled and freshly linked `MonolithAnimation` in 4/4 actions with `Result: Succeeded`. | `D:\P4\MonolithChooserUE58Host\Build-UE58-Isolated-Final-20260730-040944.log`; `D:\P4\MonolithChooserUE58Host\Build-UE58-FinalBounded-20260730-042329.log` |
| UE 5.8 | Final affected DLL | PASS — 2,682,368 bytes, SHA-256 `775C0711999BBDB9DFFE445E0A705CF8F83E738B2943FE8E2CE4CCA3D59F3BEB`. | `D:\P4\MonolithChooserUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAnimation.dll` |

The UE 5.7 compiler error was caused by a conditional expression attempting to
combine `TSharedRef<FJsonValueString>` and
`TSharedRef<FJsonValueNumber>`. Replacing the expression with explicit
enum/non-enum returns preserves the intended JSON type and compiles on both
supported engines.

The installed UE 5.8 engine was missing optional-plugin import libraries.
They were generated from the exact installed engine plugin descriptors with
UBT's foreign-plugin `-Plugin`, `-Module`, and `-gather` arguments:

| Dependency | Bytes | SHA-256 |
|---|---:|---|
| `UnrealEditor-Chooser.lib` | 205,272 | `D53B032DCBCAD48E34205200EAABCB0711403752BBB80BE02C51BA6B4E16B2C7` |
| `UnrealEditor-BlendStack.lib` | 55,144 | `E7DEC78E050AB916138C5A94CAF128974A4030B31592DD4662E500AA0423CEAA` |

A separate Speed UE 5.8 editor already had the installed
`UnrealEditor-BlendStack.dll` loaded, so an unnecessary engine-DLL relink could
not replace that file. No foreign editor was stopped. This was not an
acceptance blocker: the isolated host subsequently linked all 433 target
actions, loaded the installed dependency DLL, loaded the new Monolith DLL, and
completed the focused automation successfully.

---

## 6. Automation Results

Both engines ran:

```text
Automation RunTests Monolith.Chooser.Read
```

| Engine | Success | Failed / not run | Expected warnings | Errors | Final marker | Log |
|---|---:|---:|---:|---:|---|---|
| UE 5.7 | 4 | 0 | 3 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithChooserUE57Host\ChooserRead-UE57-FinalBounded-20260730-042258.log` |
| UE 5.8 | 4 | 0 | 2 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithChooserUE58Host\ChooserRead-UE58-FinalBounded-20260730-042355.log` |

The four tests on each engine are:

1. `Monolith.Chooser.Read.AuthoringRoundTrip`
2. `Monolith.Chooser.Read.EmptyTableValidation`
3. `Monolith.Chooser.Read.ParamGuards`
4. `Monolith.Chooser.Read.RegistrationAndSchemas`

The expected warnings come from the missing-asset negative-path assertion and
confirm that no default or substitute asset was loaded. No automation
controller error was emitted.

---

## 7. Catalog and Static Gates

The catalog generator was run independently against the exact fork base and the
Chooser branch:

```powershell
python D:\P4\MonolithPortAudit\Tools\MonolithQuery\generate_monolith_catalog_snapshot.py `
    --root D:\P4\MonolithForkChooser `
    --out D:\P4\MonolithChooserTargetCatalog.json
```

| Gate | Result |
|---|---|
| Base catalog | PASS — 1,561 actions across 24 namespaces. |
| Target catalog | PASS — 1,567 actions across 24 namespaces; source hash `2908f66998d30900234d4a15fc5e92ea2d2c71c09d69717e8a400e9c7fe55b1e`. |
| Snapshot reproducibility | PASS — a fresh pre-commit regeneration preserved the action count and semantic `source_hash`. The JSON file's whole-file hash is not an acceptance identity because its `generated_at` field changes on every run. |
| Exact delta | PASS — 6 additions, all under `chooser`; 0 removals. |
| Duplicate full names | PASS — 0. |
| Final Chooser roster | PASS — exactly 16 actions. |
| Latest checker self-test | PASS. |
| Base-vs-target static parity | PASS — base 8 blockers/11 advisories; target 8/11; new findings 0 and resolved findings 0. |
| Diff hygiene | PASS — `git diff --check` reports no whitespace errors. |
| Excluded-feature scan | PASS — no search/planning/execution-policy metadata class, invocation-log, benchmark, security, or reinforcement-learning implementation reference in the changed `Source\MonolithAnimation` files. |

The fork does not contain the upstream hosted-static-CI workflow or its current
configuration. The latest upstream checker therefore ran against the exact
clean base and the target with one identical temporary configuration. Proxy,
analyzer/invocation-log, benchmark, offline parity/catalog, and skill-drift
checks were disabled because those systems are absent from the fork or
explicitly outside this PR. Repository-wide CRLF findings were symmetrically
allowlisted.

The identical remaining findings are pre-existing: seven Niagara high-risk
registrations lack the latest checker marker, the hosted static workflow is
absent, ten Niagara raw-parameter/direct-load advisories remain, and
`.claude/agents` is an external prerequisite.

---

## 8. Visual and Discord Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This change adds headless editor action handlers, schemas, tests, docs, and a routing skill. It does not change gameplay, runtime/editor UI, VFX, animation presentation, materials, or project assets. |
| Discord screenshot upload | N/A | No visual artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 9. Result

PASS. The fork gains exactly six bounded, read-only Chooser actions without
changing the existing ten-action authoring surface. The same source compiles
and links on UE 5.7 and UE 5.8, passes 4/4 focused tests on each engine, changes
the generated catalog only by the intended six actions, introduces no new
static finding, and does not include any excluded feature class.
