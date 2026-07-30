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
| `chooser.get_chooser_table` | Reports authoritative editor rows, root-owned context, table/class/result/column/reference summaries, and optionally up to 500 bounded rows. |
| `chooser.list_chooser_columns` | Returns bounded reflected column summaries; capped at 512 columns. |
| `chooser.list_chooser_rows` | Pages exact rows with `start_row` and `limit`; each cell uses the local bounded serializer. |
| `chooser.list_chooser_references` | Walks reflected hard/soft object references under explicit depth/container/result/global-visit bounds, sorts the complete bounded result before pagination, and reports existence only for exact object evidence. |
| `chooser.validate_chooser_table` | Reports structural, known result-payload, reference-resolution, and scan-completeness errors plus non-fatal warnings without compiling, saving, or modifying the table. |

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
| Global traversal bound | PASS — the reference walker stops after 65,536 property/element visits even when individually bounded containers form a nested product; endpoints publish visits and the visit limit. |
| Authoritative rows and context | PASS — `ResultsStructs` owns editor row count, `CookedResults` is a stripped-data fallback only, and `context_entry_count` follows `UChooserTable::GetContextData()` through `RootChooser`. |
| Result payloads | PASS — invalid `FInstancedStruct` rows and null targets for known `AssetChooser`, `SoftAssetChooser`, `EvaluateChooser`, `NestedChooser`, and `ClassChooser` payloads are validation errors. |
| Mutation | PASS — handlers do not call compile/save/modify/dirty APIs and the authoring round-trip test reads a nine-row authored table without changing it. |
| Validation semantics | PASS — errors make `valid=false`; warnings remain separately visible and do not invalidate the table. |
| Soft-reference identity | PASS — a loaded or on-disk package without the referenced export is not accepted as existence. The regression fixture injects a missing `FSoftAssetChooser` path while keeping an empty package shell loaded. |
| Scalar types | PASS — bounded integers, canonical asset paths, optional path filters, and `include_rows` accept only their declared native JSON number/string/boolean types. |
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
Git blob 2826711008e70f1b581c03555dd48687b481f29d

MonolithChooserReadActionsTests.cpp
Git blob f03c41e026846b2d238c5f06141733346b4808a2

Monolith.uplugin
SHA256 5D9139060EE41DD0E067D6DB390104B3FB73C668814A5B55F7F24F1A9618F9C8
```

`git hash-object --path=<checkout-relative-path>` produced those same blob
identities for both physical host copies. Raw SHA-256 differs only because
`git archive` writes canonical LF while the Windows checkout materializes
CRLF.

The hosts use independent physical plugin copies. A shared source junction
cannot be used for consecutive cross-version verification because Unreal puts
UHT records, import libraries, and DLLs under the plugin's own
`Binaries`/`Intermediate` directories.

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | Physically isolated full editor target build | PASS — the first review build rejected the enum raw-value fallback's mixed shared-pointer conditional; after replacing it with explicit typed returns, the exact source compiled and linked all 403/403 actions with `Result: Succeeded`. | Rejected: `D:\P4\MonolithChooserUE57Host\Chooser-FinalReview2-Build-UE57-20260730.log`; accepted: `D:\P4\MonolithChooserUE57Host\Chooser-FinalReview3-Build-UE57-20260731.log` |
| UE 5.7 | Final affected DLL | PASS — final 4/4 incremental compile/link after the subobject and deduplication hardening; 2,892,800 bytes, SHA-256 `3F5349C7417DDE1FCA9FB886E7FEB3B6154E0AE188165059E37421B26CB51ED5`. | `D:\P4\MonolithChooserUE57Host\Chooser-FinalReview4-Build-UE57-20260731.log`; `D:\P4\MonolithChooserUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAnimation.dll` |
| UE 5.8 | Physically isolated full editor target build | PASS — the exact source compiled and linked all 403/403 actions with `Result: Succeeded`. | `D:\P4\MonolithChooserUE58Host\Chooser-FinalReview-Build-UE58-20260731.log` |
| UE 5.8 | Final affected DLL | PASS — one concurrent attempt stopped before source compilation because another UBT process held the shared user log; the required standalone rerun compiled/linked 4/4 actions. Final DLL is 2,723,328 bytes, SHA-256 `1C0E70CED4E780D91C5CCA3A2A23386D5696D0356DED05D7E542A69212C271BA`. | Non-source infrastructure stop: `D:\P4\MonolithChooserUE58Host\Chooser-FinalReview4-Build-UE58-20260731.log`; accepted: `D:\P4\MonolithChooserUE58Host\Chooser-FinalReview5-Build-UE58-20260731.log`; `D:\P4\MonolithChooserUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAnimation.dll` |

The UE 5.7 compiler error was caused by a conditional expression attempting to
combine `TSharedRef<FJsonValueString>` and
`TSharedRef<FJsonValueNumber>`. Replacing the expression with explicit
enum/non-enum returns preserves the intended JSON type and compiles on both
supported engines.

The final-review builds used `-NoEngineChanges`; they did not regenerate
installed-engine import libraries. The following files are pre-existing
external environment state and are not a source change, PR artifact, Speed
CL 1351 protected-build receipt, or evidence that its still-unmet protected
build gate passed:

| External installed-engine state | Bytes | SHA-256 | Last modified (KST) |
|---|---:|---|---|
| `UnrealEditor-Chooser.lib` | 205,272 | `D53B032DCBCAD48E34205200EAABCB0711403752BBB80BE02C51BA6B4E16B2C7` | 2026-07-30 03:49:09 |
| `UnrealEditor-BlendStack.lib` | 55,144 | `E7DEC78E050AB916138C5A94CAF128974A4030B31592DD4662E500AA0423CEAA` | 2026-07-30 03:58:31 |
| `UnrealEditor-Localization.lib` | 193,430 | `5EAC9FBB1346DB5ED54248F54A66DC51C9070770B4B6C4E5F251C5BE08B5A175` | 2026-07-30 21:13:51 |

The Chooser/BlendStack hashes and timestamps match the earlier external state;
the Localization library predates this final review and remains separately
attributable to the non-protected external build described by the coordinator.
No foreign editor or build process was stopped.

---

## 6. Automation Results

Both engines ran:

```text
Automation RunTests Monolith.Chooser.Read
```

| Engine | Success | Failed / not run | Test errors | Final marker | Log |
|---|---:|---:|---:|---|---|
| UE 5.7 | 6 | 0 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithChooserUE57Host\ChooserRead-UE57-FinalReview2-20260731.log` |
| UE 5.8 | 6 | 0 | 0 | `TEST COMPLETE. EXIT CODE: 0` | `D:\P4\MonolithChooserUE58Host\ChooserRead-UE58-FinalReview2-20260731.log` |

The six tests on each engine are:

1. `Monolith.Chooser.Read.AuthoringRoundTrip`
2. `Monolith.Chooser.Read.DeletedAssetPackageShell`
3. `Monolith.Chooser.Read.EmptyTableValidation`
4. `Monolith.Chooser.Read.ParamGuards`
5. `Monolith.Chooser.Read.RegistrationAndSchemas`
6. `Monolith.Chooser.Read.RootContextAndResultPayloadValidation`

The new regression writes a missing soft-object path into a Chooser row while
the corresponding empty package shell remains loaded. Readback returns
`exists=false`, validation returns `unresolved_soft_reference`, and the table
package remains clean on both engines. The new root/payload regression also
proves inherited context readback, invalid result-struct detection, null known
result-target detection, and stale `CookedResults` exclusion. Host startup
logged that port 9316 was already owned by the existing Monolith endpoint;
this is outside the headless action tests and did not produce an
automation-controller error.

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
and links on UE 5.7 and UE 5.8, passes 6/6 focused tests on each engine,
uses authoritative row/context contracts, validates result payloads, rejects
package-only evidence for a missing referenced export, keeps pagination stable
under explicit global bounds, changes the generated catalog only by the
intended six actions, introduces no new static finding, and does not include
any excluded feature class.

---

## AI review remediation, head `9184f5c6`

| Review finding | Root fix |
| --- | --- |
| Every path under `/Script/` was reported as existing when its package was absent | Script exports require exact resolved-object evidence; absence of a package is no longer inverted into `exists=true`. |
| Any ordinary asset named `…_C` could validate an unrelated soft reference | Blueprint fallback runs only for `FSoftClassProperty`, reads the owning Blueprint's `GeneratedClassPath` AssetRegistry tag, normalizes the export-text path, and requires an exact generated-class object-path match. |
| `TScriptInterface` hard references were never collected | `FInterfaceProperty` is not an `FObjectPropertyBase`, so it fell through every branch. An explicit branch now reads the interface's object pointer via the typed accessor. |
| Only element zero of a reflected fixed-size array was serialized or visited | Serialization returns a bounded `count` + `items` container, and reference collection visits every `ArrayDim` element through indexed `ContainerPtrToValuePtr`. |
| Deprecated properties produced false `unresolved_soft_reference` findings | The field iterator uses `EFieldIteratorFlags::ExcludeDeprecated`, matching the serializer's policy. |
| `list_chooser_rows` silently capped every row at 512 cells | The row endpoints report `column_count`, `row_cells_per_row`, and `row_cells_truncated`, so a partial predicate/output set is explicit. |
| `int64`/`uint64` values above 2^53−1 were rounded through a double | Integers outside the exactly representable JSON range are emitted as decimal strings; smaller values keep their numeric form. |
| Invalid enum ordinals serialized as an empty string | Valid ordinals retain symbolic names; invalid ordinals fall back to an exact JSON number or decimal string. |
| Stale cooked/disabled/column arrays could inflate `row_count` | Editor `ResultsStructs` is authoritative; `CookedResults` is consulted only when the editor-only property is unavailable. Alignment problems remain visible to validation instead of becoming phantom rows. |
| Child Choosers reported their local empty context instead of root-owned context | `context_entry_count` uses `UChooserTable::GetContextData()` behind the existing optional-dependency gate, with a reflection root fallback. |
| Validation checked only result-array length | Every bounded editor result row must contain a valid `FInstancedStruct`; known result types must also have a non-null/non-empty `Asset`, `Chooser`, or `Class` target. |
| Per-container limits still allowed multiplicative nested traversal | `FReferenceScan` enforces a global 65,536 property/element visit budget and publishes the used/maximum visits. |
| Set/map sparse indices made pagination order unstable | Set/map source locations use semantic container markers, references deduplicate by stable path/location, and the complete bounded result is sorted before slicing a page. |
| Depth-limited scans still reported `complete=true` (carried over from head `0a84192d`, not closed by the prior fix) | `FReferenceScan` gains `bDepthLimited`, kept separate from `bTruncated` so a depth stop does not abort sibling traversal. `IsComplete()` requires both to be clear, `validate_chooser_table` raises `reference_scan_depth_limited`, and the read endpoints publish `scan_depth_limited`/`references_depth_limited` plus an explicit completeness flag. |
