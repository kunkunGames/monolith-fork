# Dataflow Read and Validation Actions Verification

**Date:** 2026-07-30
**Branch:** `jules/codex/dataflow/read-validation-actions`
**Fork base:** `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Reference source:** `kunkunGames/monolith@a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1`
**Scope:** Eight bounded, read-only `dataflow` inspection and validation actions in a new editor module
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Goal

Verify that the public fork gains a practical Dataflow inspection surface
without adding graph authoring, evaluation, regeneration, saving,
project-specific dependencies, or any explicitly excluded feature class. The
accepted implementation must:

- add exactly eight `dataflow` actions and remove none;
- use the engine-source `DataflowCore` and `DataflowEngine` modules on both UE
  5.7 and UE 5.8 instead of incorrectly treating them as optional plugins;
- accept only exact JSON scalar types, bounded integers, canonical package
  paths, exact object identities, and case-exact registered node types;
- bound every returned collection and free-form text field while reporting
  independent truncation and completeness;
- avoid unbounded generic property/container serialization;
- preserve package dirty state and expose explicit read-only postconditions;
- never claim graph validity when a node or connection scan is incomplete;
- compile and pass focused automation on UE 5.7 and UE 5.8; and
- exclude security, benchmark, invocation-log, action-metadata,
  reinforcement-learning, execution-policy, and search-planning features.

---

## 2. Action Surface

| Action | Contract |
| --- | --- |
| `dataflow.get_status` | Reports the exact eight-action roster, Dataflow plugin/module state, and explicit authoring/evaluation/regeneration=false flags. |
| `dataflow.list_assets` | Enumerates a bounded AssetRegistry slice of exact `UDataflow` rows below one canonical `/Game` package directory without loading assets. |
| `dataflow.get_dataflow_graph` | Returns independently bounded node and connection slices, bounded pin/property rows, explicit registered/declared pin provenance, and package-dirty postconditions. |
| `dataflow.list_dataflow_node_types` | Returns valid registered factory types in deterministic category/type order with exact counts and bounded optional pin schemas. |
| `dataflow.get_dataflow_node_schema` | Resolves one case-exact valid registered factory type and returns bounded default pin/property schemas. |
| `dataflow.validate_dataflow_graph` | Performs bounded node/connection integrity checks and omits `valid` whenever either scan is incomplete. |
| `dataflow.list_dataflow_variables` | Returns bounded property-bag descriptors and bounded scalar values with explicit omission status for containers, structs, and fixed arrays. |
| `dataflow.list_dataflow_comments` | Returns bounded editor comments and geometric node-membership hints under an explicit comparison-work budget. |

`MonolithDataflow` owns registration and shutdown unregistration for the new
namespace. It has no Speed-, Lyra-, or other game-project dependency.

---

## 3. Fail-Closed Contract

| Contract | Result |
| --- | --- |
| JSON scalar types | PASS — declared strings, booleans, and integers require their exact raw JSON types. String-encoded and fractional integers fail with `-32602`. |
| Integer ranges | PASS — all 16 bounded numeric parameters publish inclusive `minimum`/`maximum` fields and reject out-of-range values without clamping. |
| Schema helper | PASS — `FParamSchemaBuilder::Range` fails fast on missing parameters, non-finite bounds, or an inverted interval; it does not silently omit an invalid discovery contract. |
| Package directory | PASS — only `/Game` or a canonical directory below `/Game/` is accepted. `/GameX`, dotted paths, backslashes, colons, and trailing slashes fail. |
| Object identity | PASS — only exact `/Game/.../Asset.Asset` identities are accepted. Shorthand names, `.uasset` paths, subobjects, redirects, case substitutions, and alternate resolved objects fail. |
| Object type | PASS — missing objects and exact non-`UDataflow` objects return distinct structured errors; wrong-type reads do not dirty the package. |
| Node type identity | PASS — case-exact valid registered types succeed; a case-only substitution returns `node_type_case_mismatch`; an invalid registered entry fails explicitly. |
| Graph slices | PASS — node and connection bounds are independent; endpoint resolution therefore does not depend on the returned node slice. |
| Validation completeness | PASS — incomplete node or connection scans return `validity_status=incomplete`, `validation_complete=false`, and no `valid` field. |
| Property values | PASS — bool, enum, numeric, name, string, text, soft-object, and object scalar values use direct bounded readers. Dynamic containers, structs, fixed arrays, and unsupported types report an omission status and no `value`. |
| Aggregate output | PASS — all dynamic top-level and nested arrays share a 4,096-row budget, and reflected/free-form strings passed through the bounded reader share a 1,048,576-character budget after per-field caps. Fixed-size metadata is bounded by the row ceiling; exhaustion is explicit and never allocates omitted rows/text. |
| Free-form text | PASS — each returned text field is capped at 4,096 characters and contributes to `truncated_text_field_count`; aggregate text exhaustion is separately reported. Truncation never splits a UTF-16 surrogate pair, so a bounded value always remains UTF-8 encodable. |
| Registry scan completeness | PASS — `list_assets` publishes `asset_registry_scan_in_progress` and withholds `count_complete`/`total_count` while the Asset Registry is still performing its initial scan, because enumeration can only observe assets discovered so far. |
| Comment membership | PASS — comment boxes remain membership candidates, so a comment nested inside another is reported as a contained node. `IsNodeInsideComment` rejects self-membership, and `considered_non_comment_node_count` plus `considered_membership_candidate_count` report both populations. |
| Comment work | PASS — `comment_limit * graph_node_scan_limit` above 1,000,000 is rejected and never clamped. |
| Read-only state | PASS — asset-backed reads report loaded/dirty before/after state. The loaded package is captured before case/redirect/type rejection, and every dirty-state transition is a hard failure. |
| Mutation | PASS — the eight production module files contain no authoring, transaction, package creation/save, dirty-mark, graph mutation, evaluation, or regeneration call. |

### 3.1 Root fix: bounded value inspection

The first implementation capped a returned property string after calling
`FProperty::ExportTextItem_Direct` or
`FInstancedPropertyBag::GetValueSerializedString`. That bounded the response
but did not bound the work or allocation needed to serialize a large nested
container first.

The final implementation removes both generic export calls from production.
It reads supported scalar types directly, caps string/text output at 4,096
characters, and returns an explicit `value_read_status` with no `value` for
containers, structs, fixed arrays, or unsupported types. Automation creates a
5,000-character variable and a container variable and verifies the 4,096
character cap, omission status, absent container value, and preserved package
dirty state.

### 3.2 AI review remediation

Both actionable Codex findings on the initial head
`a73f9bbfa69983d7a44f4fa5348a7cea6f080531` were reproduced as contract gaps
and closed in shared production paths.

| Review finding | Root fix | Regression evidence |
| --- | --- | --- |
| Individually valid graph limits could multiply into hundreds of thousands of nested rows and more than a gigabyte of serialized text | `FOutputBudget` enforces one shared 4,096-row budget across graph nodes, pins, properties, connections, factory types, schema rows, issues, variables, container descriptors, comments, and contained nodes, plus a 1,048,576-character budget for reflected/free-form strings. Builders stop allocating omitted rows/text and every result publishes aggregate budget counters and truncation flags. | `ParamGuards` proves the exact row/text ceilings directly. `ReadOnlyContracts` creates ten overlapping comments and 500 contained nodes, requests a valid 5,000-row shape, and verifies exactly 4,096 rows, `output_budget_exhausted=true`, incomplete comments, and a clean package. |
| Rejected post-load identities returned before enforcing dirty-state preservation | `LoadExactDataflowAsset` captures the loaded object's outer package before exact-path, redirector, or `UDataflow` type decisions. All post-load branches share one dirty-state postcondition; failed object lookup also rechecks a package loaded as a side effect. Error data reports package capture and before/after preservation. | Wrong-type and case-mismatched fixture reads must expose `package_captured_after_load=true`, `package_dirty_state_preserved=true`, retain their distinct identity errors, and leave the package clean. |

### 3.3 AI review remediation, head `4b40e6cc`

| Review finding | Disposition | Evidence |
| --- | --- | --- |
| Truncating a bounded field can split a UTF-16 surrogate pair | Fixed — `SurrogateSafeCutLength` moves the cut back one code unit when the last kept unit is a high surrogate, for both the ellipsis and non-ellipsis branches. | `Monolith.Dataflow.SurrogateTruncation` sweeps every cut point across a supplementary-character string and asserts no unpaired surrogate survives and the cap still holds. |
| `list_assets` reports `count_complete=true` while the Asset Registry initial scan is still running | Fixed — the action samples `IAssetRegistry::IsLoadingAssets()` before enumerating, publishes `asset_registry_scan_in_progress`, and withholds `count_complete`/`total_count` while a scan is in flight. | Contract row in section 3; the partial count is no longer emitted as `total_count`. |
| Nested comment boxes are excluded from comment membership results | Fixed — comments stay in the membership candidate list. `IsNodeInsideComment` already rejects self-membership, so no comment can contain itself. `considered_non_comment_node_count` keeps its meaning and `considered_membership_candidate_count` reports the compared population. | Contract row in section 3. The `comment_limit * graph_node_scan_limit` budget is unchanged because candidates remain bounded by `graph_node_scan_limit`. |
| `list_assets` should treat a limit-triggered enumeration stop as `asset_registry_enumeration_failed` | Rejected — not a defect. | `IAssetRegistry::EnumerateAssets` returns `false` only for an empty or invalid filter. `UAssetRegistryImpl::EnumerateAssets` returns `true` on early stop from both the in-memory path (`if (bStopIteration) { return true; }`) and the on-disk path (`break` followed by `return true`), and the interface header documents "@return False if filter is invalid, otherwise true." A callback that returns `false` therefore never produces `bEnumerated == false`, so the bounded result is already returned as intended. |

---

## 4. Verification Environment and Source Identity

| Engine | Engine association | Resolved engine root | Host project | Plugin source |
| --- | --- | --- | --- | --- |
| UE 5.7 | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\MonolithDataflowUE57Host\MonolithDataflowUE57Host.uproject` | Physical copy under `D:\P4\MonolithDataflowUE57Host\Plugins\Monolith` |
| UE 5.8 | `5.8` | `D:\Engine\UE_5.8` | `D:\P4\MonolithDataflowUE58Host\MonolithDataflowUE58Host.uproject` | Physical copy under `D:\P4\MonolithDataflowUE58Host\Plugins\Monolith` |

Each engine root was resolved from the corresponding host project's
`EngineAssociation` and registered installed-engine root. The hosts use
independent physical plugin copies, so UHT records, import libraries, and DLLs
cannot cross-contaminate engine versions.

The final worktree, UE 5.7 host, and UE 5.8 host matched for all 13 build inputs
(the descriptor, shared schema header, module rules, and every
implementation/test source). Representative final SHA-256 values:

```text
Monolith.uplugin
02D236D64BC3228355C5103734907D9DF7698CC02BD26CF1F5C9C34C8F9652F5

MonolithParamSchema.h
0359E0F8381D9BDFF057ABB627C91E77898C925D1E96A08FE40839A3A85C3C95

MonolithDataflow.Build.cs
53F105BCB2498628C7E165C77F617C762817AD558D8258D8CF1107E95E4A45C2

MonolithDataflowActions.cpp
4B99E700F3296CB2EEE8C88A33BC0AD0BFB21C58B3F241B39275766E6717F10F

MonolithDataflowCommon.cpp
CC0296085B1CD5C35ADCB158DE68D9298580AAFB548DDAF69C3D59B8BC4E962B

MonolithDataflowInspection.cpp
71F1235DC089764528D33F5BD5F458594CE44BA992F861ED8784E06FF9B1C3A2

MonolithDataflowParamGuardTests.cpp
77680A2310459C9E94E60B55ED90FCA268938C6A76A12EC32E52CA49F1B2FD10

MonolithDataflowReadOnlyTests.cpp
8C23D4071127C5B0E7D34A9697838A7A66E0C6777AC6DEE4E16AE620242F932C
```

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
| --- | --- | --- | --- |
| UE 5.7 | Physically isolated full editor-target build | PASS — 202/202 actions explicitly compiled every Dataflow implementation and test file and linked `UnrealEditor-MonolithDataflow.dll`. | `D:\P4\MonolithDataflowUE57Host\UBT-UE57-Dataflow-Final-20260730.log` |
| UE 5.7 | Final AI-review rebuild | PASS — 8/8 actions recompiled the common/output-budget path, handlers, inspection implementation, and both affected test files, then freshly linked the Dataflow DLL with `Result: Succeeded`. | `D:\P4\MonolithDataflowUE57Host\UBT-UE57-Dataflow-ReviewAccepted-20260730.log` |
| UE 5.7 | Final response-contract/test rebuild | PASS — 4/4 actions compiled the final bounded-text metadata assertion and freshly relinked the Dataflow DLL. | `D:\P4\MonolithDataflowUE57Host\UBT-UE57-Dataflow-ReviewFinal2-20260730.log` |
| UE 5.7 | Final affected DLL | PASS — 376,320 bytes, SHA-256 `AD8BE0FEA7A07B2C70F1FABD38C35F907525971E613A96ACD4D6A906698F4F84`. | `D:\P4\MonolithDataflowUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithDataflow.dll` |
| UE 5.8 | Physically isolated full editor-target build | PASS — 202/202 actions explicitly compiled every Dataflow implementation and test file and linked `UnrealEditor-MonolithDataflow.dll`. | `D:\P4\MonolithDataflowUE58Host\UBT-UE58-Dataflow-Final-20260730.log` |
| UE 5.8 | Final AI-review rebuild | PASS — 8/8 actions recompiled the common/output-budget path, handlers, inspection implementation, and both affected test files, then freshly linked the Dataflow DLL with `Result: Succeeded`. | `D:\P4\MonolithDataflowUE58Host\UBT-UE58-Dataflow-ReviewAccepted-20260730.log` |
| UE 5.8 | Final response-contract/test rebuild | PASS — 4/4 actions compiled the final bounded-text metadata assertion and freshly relinked the Dataflow DLL. | `D:\P4\MonolithDataflowUE58Host\UBT-UE58-Dataflow-ReviewFinal2-20260730.log` |
| UE 5.8 | Final affected DLL | PASS — 339,968 bytes, SHA-256 `341C4C91753A2E72AC10E9A2B83BA75C7DAE1378D2C69A3B2AAC26F64A13F881`. | `D:\P4\MonolithDataflowUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithDataflow.dll` |

The isolated full UE 5.7 build emitted no compiler warning. The isolated full
UE 5.8 build emitted ten pre-existing C4996 warnings in Index, Blueprint,
Animation, Material, and UI sources. The final affected-file rebuilds emitted
no compiler warning or error on either engine, and no Dataflow source emitted a
compiler warning or error in any accepted build.

---

## 6. Automation Results

Both engines ran one fresh process directly through PowerShell's native `&`
operator with:

```text
-ExecCmds="Automation RunTests Monolith.Dataflow"
-TestExit="Automation Test Queue Empty"
```

| Engine | Discovered | Succeeded | Failed | Queue-empty marker | TestExit marker | Bad runtime/assertion markers | Accepted log |
| --- | ---: | ---: | ---: | --- | --- | ---: | --- |
| UE 5.7 | 3 | 3 | 0 | present | present | 0 | `D:\P4\MonolithDataflowUE57Host\Dataflow-UE57-ReviewFinal2-Console-20260730.log` |
| UE 5.8 | 3 | 3 | 0 | present | present | 0 | `D:\P4\MonolithDataflowUE58Host\Dataflow-UE58-ReviewFinal2-Console-20260730.log` |

The three suites are:

1. `Monolith.Dataflow.ParamGuards`
2. `Monolith.Dataflow.ReadOnlyContracts`
3. `Monolith.Dataflow.RegistryAndSchemas`

Together they verify:

- wrong JSON types, fractional/out-of-range integers, unknown keys, `/GameX`,
  shorthand/file asset paths, and excessive comment work;
- all 16 numeric discovery ranges;
- exact eight-action registration;
- node factory discovery, case-exact schema selection, and case-only rejection;
- exact `UDataflow` identity and distinct wrong-type rejection;
- package capture and dirty-state preservation on wrong-type and case-mismatch
  post-load rejection paths;
- empty graph reads, complete valid validation, and dirty-state preservation;
- incomplete node-scan behavior with `valid` omitted;
- exact aggregate 4,096-row and 1,048,576-character budget behavior;
- a valid 5,000-row nested comment request stopping at 4,096 rows with
  `output_budget_exhausted=true` and incomplete output;
- a 5,000-character variable capped to 4,096 characters;
- explicit container-value omission without generic serialization; and
- editor comment membership with preserved package state.

An earlier UE 5.7 process returned exit code 0 but its log stopped after the
second success and contained neither `RegistryAndSchemas`, queue-empty, nor
`TestExit`. That log,
`D:\P4\MonolithDataflowUE57Host\Dataflow-UE57-IncompleteContract-20260730.log`,
was rejected. A direct retry completed 3/3, and the later bounded-value build
and accepted log above supersede both earlier runs. Process exit code alone is
not treated as automation proof.

---

## 7. Catalog, Static, Mutation, and Exclusion Gates

The latest upstream generator and static checker were run independently
against the exact clean fork base and the Dataflow worktree.

| Gate | Result |
| --- | --- |
| Catalog generator tests | PASS — 4/4. |
| Base catalog | PASS — 1,561 actions across 24 namespaces; semantic source hash `268fe956aa86e93ed2289f0fe0204a050de318d4f0b287027f7c6dcd08b8eaae`. |
| Target catalog | PASS — 1,569 actions across 25 namespaces; semantic source hash `8632b9870476ff0784dc80c0d86b6466c63a8932a88a05b1968769a18ca5d092` (`action_semantics_v1`). |
| Exact catalog delta | PASS — eight additions, all under the new `dataflow` namespace; zero removals. |
| Duplicate full names | PASS — zero. |
| Latest checker self-test | PASS. |
| Base-vs-target static parity | PASS — base 8 blockers/11 advisories; the post-review target rerun remains 8/11; `D:\P4\MonolithDataflowVerification\TargetStaticReviewFinal.log` is byte-identical to the base log with SHA-256 `0260D19B22263DF119075866D813C6295DD7AAA5E21632241D6073CAFE7815CC`. |
| Production mutation scan | PASS — zero mutation-call matches across the eight production Dataflow files. |
| Generic export scan | PASS — zero `ExportTextItem_Direct` or `GetValueSerializedString` calls across production Dataflow files. |
| Production log/metadata scan | PASS — zero `UE_LOG`, `LogMonolith`, or invocation-log matches. |
| Excluded-feature scan | PASS — zero security, auth/credential, benchmark, invocation-log, action-metadata, reinforcement-learning/reward, execution-policy, telemetry/planner, or search-plan implementation matches. |
| Diff hygiene | PASS — `git diff --check` reports no whitespace error. |

The fork lacks the latest upstream hosted-static workflow and several
auxiliary systems. The same temporary static configuration was therefore used
for base and target. Proxy, analyzer/invocation-log, benchmark, offline
catalog/parity/executable-freshness, and skill-drift checks were disabled
because those systems are absent or explicitly excluded. Repository-wide CRLF
findings were symmetrically allowlisted.

`Dataflow`, `DataflowCore`, and `DataflowEngine` were intentionally removed
from the checker's optional dependency-token list. `DataflowCore` and
`DataflowEngine` are engine-source modules in both supported engines, while the
optional `Dataflow` plugin reference is declared in the descriptor and is not a
compiled module dependency.

The identical remaining findings are pre-existing: seven Niagara high-risk
registrations lack the latest checker marker, the hosted static workflow is
absent, ten Niagara raw-parameter/direct-load advisories remain, and
`.claude/agents` is an external prerequisite.

The test fixture intentionally uses `CreatePackage`, `NewObject`, and
`SetDirtyFlag` under `Private\Tests` to construct and verify disposable
read-only cases. Those calls are not present in the production module surface.

---

## 8. Visual and Discord Scope

| Gate | Result | Reason |
| --- | --- | --- |
| PC 1920x1080 screenshot | N/A | This change adds headless editor action handlers, schemas, automation, docs, and a routing skill. It does not change gameplay, runtime/editor UI, VFX, animation presentation, materials, or project assets. |
| Discord screenshot upload | N/A | No visual artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 9. Result

PASS. The fork gains exactly eight bounded, read-only Dataflow discovery,
inspection, and validation actions through an isolated module. The same final
source compiles and links on UE 5.7 and UE 5.8, passes 3/3 focused tests on each
engine with complete queue/termination proof, changes the generated catalog
only by the intended namespace, introduces no new static finding, avoids
unbounded generic property export, caps aggregate nested output before
allocation growth, enforces dirty-state preservation on successful and
rejected loads, closes both AI-review findings with executable regressions,
and adds none of the excluded feature classes.
