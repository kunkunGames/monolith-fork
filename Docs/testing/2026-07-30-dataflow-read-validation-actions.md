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
| Universal response shaping | PASS — all eight handlers accept the framework-reserved `_fields`, `_omit`, `_compact_json`, `_row_fields`, and `_path_fields` keys while still rejecting genuinely unknown action parameters. `FMonolithParamSchema::IsUniversalResponseShapingParam` is the single exact-key contract used by registry and action-local validation. |
| Aggregate output | PASS — all dynamic top-level and nested arrays share a 4,096-row budget, and reflected/free-form strings passed through the bounded reader share a 1,048,576-character budget after per-field caps. Fixed-size metadata is bounded by the row ceiling; exhaustion is explicit and never allocates omitted rows/text. |
| Free-form text | PASS — each returned text field is capped at 4,096 characters and contributes to `truncated_text_field_count`; aggregate text exhaustion is separately reported. Truncation never splits a UTF-16 surrogate pair, so a bounded value always remains UTF-8 encodable. |
| Registry scan completeness | PASS — `list_assets` publishes `asset_registry_scan_in_progress` and withholds `count_complete`/`total_count` while the Asset Registry is still performing its initial scan, because enumeration can only observe assets discovered so far. |
| Comment membership | PASS — comment boxes remain membership candidates, so a comment nested inside another is reported as a contained node. `IsNodeInsideComment` rejects self-membership, and `considered_non_comment_node_count` plus `considered_membership_candidate_count` report both populations. |
| Comment work | PASS — `comment_limit * graph_node_scan_limit` above 1,000,000 is rejected and never clamped. |
| Read-only state | PASS — asset-backed reads report loaded/dirty before/after state. The loaded package is captured before case/redirect/type rejection, and every dirty-state transition is a hard failure. |
| Offline proxy discovery | PASS — Python and native editor-down seed lists are identical, unique, contain 17 dispatchers, and include `dataflow_query`; the native proxy builds through dynamically discovered Visual Studio C++ tooling. |
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

### 3.4 AI review remediation, head `dcf7447d`

| Review finding | Disposition | Evidence |
| --- | --- | --- |
| Aggregate graph rows can exceed the individual node/pin/property limits | Already fixed in the production path before this review round — one `FOutputBudget` is passed through top-level nodes, nested pins/properties, and connections. | Both fresh engine runs pass the direct 4,096-row budget assertions and the 5,000-row nested-comment regression. |
| A limit-triggered `EnumerateAssets` stop should be treated as success | Rejected after engine-source verification — callback early-stop returns `true`; only an invalid filter returns `false`. | The existing bounded result and explicit `bSawExtra` truncation contract are retained. No failure is masked. |
| The committed source registers four tests but accepted logs showed only three | Fixed by rebuilding and rerunning the final tree. | UE 5.7 and UE 5.8 each discover and pass all four named `Monolith.Dataflow` tests, followed by queue-empty and `TestExit`. |
| Offline Python/native proxy seeds omit `dataflow_query` | Fixed in both seed lists with a source-level parity regression and a fresh native rebuild. | `python Scripts/test_proxy_seed_parity.py` reports 17 identical unique dispatchers including `dataflow_query`; both `build_proxy.bat` and the compatibility `build.bat` entry point succeed. |
| Action-local strict validation rejects universal response-shaping params | Fixed at the shared contract boundary. `FMonolithParamSchema::IsUniversalResponseShapingParam` now drives both registry and Dataflow local validation. | `Monolith.Dataflow.ParamGuards` submits all five keys together and succeeds after post-dispatch shaping; `Monolith.ResponseShaping` passes 14/14. |

---

## 4. Verification Environment and Source Identity

| Engine | Engine association | Resolved engine root | Host project | Plugin source |
| --- | --- | --- | --- | --- |
| UE 5.7 | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\MonolithDataflowUE57Host\MonolithDataflowUE57Host.uproject` | Physical copy under `D:\P4\MonolithDataflowUE57Host\Plugins\Monolith` |
| UE 5.8 | `5.8` | `D:\Engine\UE_5.8` | `D:\P4\MonolithDataflowUE58Host\MonolithDataflowUE58Host.uproject` | Physical copy under `D:\P4\MonolithDataflowUE58Host\Plugins\Monolith` |

Each engine root was resolved from the corresponding host project's
`EngineAssociation` and registered installed-engine root. The hosts use
independent project/plugin working directories and build-output roots, so UHT
records, import libraries, and DLLs cannot cross-contaminate engine versions.

The final worktree, UE 5.7 host, and UE 5.8 host matched for all 14 build inputs
(the descriptor, shared schema header/implementation, and every Dataflow module
rule, public/private header, implementation, and test source). Representative
final SHA-256 values:

```text
Monolith.uplugin
02D236D64BC3228355C5103734907D9DF7698CC02BD26CF1F5C9C34C8F9652F5

MonolithParamSchema.h
9EE515FDF582A6E47262EED7EDFBE9956FC0B238990DE1BA5F1826BE22FA0A9B

MonolithToolRegistry.cpp
DF5F7039658BE3A87F1D16A7524808C18B7E0E451E9E54D1A4B99229013A2F1A

MonolithDataflow.Build.cs
53F105BCB2498628C7E165C77F617C762817AD558D8258D8CF1107E95E4A45C2

MonolithDataflowActions.cpp
C6E3BB60DF80E6F4F0CEB53C5263ADC1425ECBB35E6E4C3DA59EAD3033F417C8

MonolithDataflowCommon.cpp
FA77AAFD8451716E58ED292800D21D2920660D5DF30A3B4D445BB2A451D26A6D

MonolithDataflowInspection.cpp
CFD8810B82E0396C91CDC8513B0D6CF25B53D15201168AF5B78F4299174159FB

MonolithDataflowParamGuardTests.cpp
5372C5951624EECA7D62E237E7C43A28D5E5F0838FFDA66F8CF0645A0E2F45F8

MonolithDataflowReadOnlyTests.cpp
8C23D4071127C5B0E7D34A9697838A7A66E0C6777AC6DEE4E16AE620242F932C
```

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
| --- | --- | --- | --- |
| UE 5.7 | Final isolated full editor-target build | PASS — 199/199 actions compile the final shared schema helper, every Dataflow implementation/test translation unit, and freshly link `UnrealEditor-MonolithDataflow.dll`. | `D:\P4\MonolithDataflowUE57Host\Build-UE57-Dataflow-PR11-20260730.log` / `UBT-UE57-Dataflow-PR11-20260730.log` |
| UE 5.7 | Final affected DLL | PASS — 386,048 bytes, SHA-256 `221F5D45F220D5645AFB44FBE9E8197941230F4C173C17F3C0BCA36775752967`. | `D:\P4\MonolithDataflowUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithDataflow.dll` |
| UE 5.8 | Final isolated full editor-target build | PASS — 199/199 actions compile the same final inputs and freshly link `UnrealEditor-MonolithDataflow.dll`. | `D:\P4\MonolithDataflowUE58Host\Build-UE58-Dataflow-PR11-20260730.log` / `UBT-UE58-Dataflow-PR11-20260730.log` |
| UE 5.8 | Final affected DLL | PASS — 349,184 bytes, SHA-256 `4B2A69939019BA0AFDC46C372C3C2A9B1DE7607E4CA7984E3B6077B6DFC6E37B`. | `D:\P4\MonolithDataflowUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithDataflow.dll` |
| Native proxy | Toolchain discovery and rebuild | PASS — `build_proxy.bat` locates the installed x64 C++ toolchain with `vswhere`; legacy `build.bat` delegates to that path. Both build/copy the same executable. | `Tools\MonolithProxy\build_proxy.bat`; final local executable SHA-256 `333638C848BE84A4D83E933562BACD7354CD7174CAC056F42C94E0057876C31B` |

The final UE 5.7 build emitted no compiler warning. The final UE 5.8 build
emitted ten pre-existing C4996 warnings in Index, Blueprint, Animation,
Material, and UI sources; no `MonolithCore` or `MonolithDataflow` source emitted
a compiler warning or error.

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
| UE 5.7 | 4 | 4 | 0 | present | present | 0 | `D:\P4\MonolithDataflowUE57Host\Dataflow-UE57-PR11-Console-20260730.log` |
| UE 5.8 | 4 | 4 | 0 | present | present | 0 | `D:\P4\MonolithDataflowUE58Host\Dataflow-UE58-PR11-Console-20260730.log` |

The four suites are:

1. `Monolith.Dataflow.ParamGuards`
2. `Monolith.Dataflow.ReadOnlyContracts`
3. `Monolith.Dataflow.RegistryAndSchemas`
4. `Monolith.Dataflow.SurrogateTruncation`

The shared response-shaping regression was also run from the final UE 5.7
binary: `Monolith.ResponseShaping` discovered and passed 14/14 tests with zero
failures or fatal/assertion markers, followed by queue-empty and `TestExit`.
Accepted log:
`D:\P4\MonolithDataflowUE57Host\ResponseShaping-UE57-PR11-Console-20260730.log`.

Together they verify:

- wrong JSON types, fractional/out-of-range integers, unknown keys, `/GameX`,
  shorthand/file asset paths, and excessive comment work;
- all five universal response-shaping keys pass the action-local strict reader
  and are still applied by the registry after dispatch;
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
was rejected. The final 4/4 runs above supersede every earlier log. Process exit
code alone is not treated as automation proof.

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
| Final catalog regeneration | PASS — `TargetCatalogPR11.json` has the same 1,569 actions and `action_semantics_v1` hash as the prior accepted target; only the generated timestamp line differs. |
| Base-vs-target static parity | PASS — base 8 blockers/11 advisories; the final target rerun remains 8/11; `D:\P4\MonolithDataflowVerification\TargetStaticPR11.log` is byte-identical to the base log with SHA-256 `0260D19B22263DF119075866D813C6295DD7AAA5E21632241D6073CAFE7815CC`. |
| Offline proxy seed parity | PASS — `python Scripts/test_proxy_seed_parity.py` reports 17 identical unique Python/native seed dispatchers including `dataflow_query`; the proxy and parity-test Python files compile. |
| Native proxy build | PASS — dynamic `vswhere` discovery finds the installed x64 C++ toolchain; both native build entry points compile and copy the proxy executable. |
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
source compiles and links on UE 5.7 and UE 5.8, passes 4/4 focused Dataflow
tests on each engine plus 14/14 shared response-shaping regressions, and records
complete queue/termination proof. It changes the generated catalog only by the
intended namespace, introduces no new static finding, advertises
`dataflow_query` during editor-down proxy startup, preserves strict unknown-key
rejection without blocking universal shaping, avoids unbounded generic property
export, caps aggregate nested output before allocation growth, enforces
dirty-state preservation on successful and rejected loads, addresses every
current review finding with code, regression proof, or engine-source evidence,
and adds none of the excluded feature classes.
