# Monolith Practitioner Workflow ROI Backlog

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md), [SPEC_MonolithCore.md](SPEC_MonolithCore.md), [SPEC_MonolithActionAuthoring.md](SPEC_MonolithActionAuthoring.md), [SPEC_MonolithToolCallReliabilityBacklog.md](SPEC_MonolithToolCallReliabilityBacklog.md), [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md)
**Engine:** Unreal Engine 5.7+
**Status:** Investigation complete; implementation-ready backlog
**Created:** 2026-06-26
**Owner modules:** MonolithCore, MonolithEditor, MonolithSource, MonolithIndex, MonolithBlueprint, MonolithGAS, MonolithAI, MonolithUI, MonolithAudio, MonolithLevelSequence, MonolithMesh, MonolithWorldGen, MonolithConsole, MonolithToolsetBridge, `Tools/MonolithQuery`, `Analyzer/`, `Scripts/`, and the optional domain modules named below.
**Scope:** High-ROI, root-cause work found by treating Monolith users as real Unreal practitioners: gameplay engineer, technical artist, level/world builder, UI/cinematics/audio/localization practitioner, and engine/tools maintainer. This spec combines current code/docs inspection, sub-agent role audits, and recent invocation/session log evidence.
**Non-goals:** Implementing every domain authoring request in one PR, porting external agent consoles/WebUI/ACP/Lua runtimes, changing public action names without additive compatibility, replacing existing domain specs, or treating this document as a routine task journal.

---

## 1. Decision

The highest ROI is **not** more isolated single-action sprawl. Monolith already has many domain actions. The recurring failure pattern is that agents can inspect or perform a narrow edit, but cannot reliably complete an Unreal practitioner workflow:

1. discover the right live/offline capability,
2. plan the operation with accurate preconditions,
3. run or dry-run the edit,
4. verify the changed asset/world state,
5. save or report dirty packages,
6. prepare source control where applicable,
7. return bounded proof and next actions.

Therefore the backlog has two tracks:

| Track | Priority | Why |
|---|---:|---|
| Reliability and trust substrate | P0 | If MCP is down, discover is unavailable, health lies, payloads explode, or high-traffic actions return weak errors, every practitioner workflow stalls. |
| Workflow contracts | P1 | Domain CRUD must be composed into explicit, testable workflows with proof objects. This gives agents a stable "do the job" path without inventing multi-step sequences. |
| Domain authoring gaps | P2 | Inspection-only domains should gain authoring only after the shared safety/proof contract exists, so new verbs do not become another fragmented surface. |

New actions should be added only when they close a real workflow. Each workflow must return a standard proof object, not just a success envelope.

## 2. Evidence Baseline

The investigation used read-only repo inspection, offline CLI health checks, recent log analysis, and five role-specific sub-agent audits. No live editor mutation was performed for this spec.

### 2.1 Current command evidence

| Evidence | Result |
|---|---|
| `python Analyzer\analyze_invocation_logs.py --log-root Logs --since 20260620 --out C:\Users\12336\AppData\Local\Temp\monolith_agent_audit_invocation --format markdown,json --top 40 --rank-by-recency` | Scanned 34,445 records across 12 files, 72 findings, 0 parse warnings. |
| `python Analyzer\analyze_session_transcripts.py --since 20260620 --out C:\Users\12336\AppData\Local\Temp\monolith_agent_audit_sessions` | Scanned 2,734 session files; 1,167 Monolith tool results; 68 errors, including 32 transport/availability errors across 12 sessions. |
| `Binaries\monolith_query.exe source health --include-counts=true` | Structural/CRG checks mostly OK, but offline CLI reports `schema_version=3 (expected 1)` warning. Source schema constant is v3. |
| `Binaries\monolith_query.exe project health --include-counts=true` | ProjectIndex structural, FTS, and CRG parity OK in the sampled checkout. |
| `gh pr list --state open --limit 100 ...` | Only open PR #1415 touches `Source/MonolithIndex/Private/Actions/ProjectExportAssetTextAction.cpp`; this spec file does not overlap. |
| Sub-agent engine/tools audit | `Scripts/recover_mcp.ps1 -ProbeOnly` observed MCP down: `RESULT=MCP_DOWN probe_only=true url=http://localhost:9316/health`. |

### 2.2 Historical log snapshot and current disposition

The table preserves the ranked invocation-log snapshot used by the original audit. It is evidence, not a current open queue: the graph export was retired on 2026-07-21 and old builder calls are now classified as `retired_action`.

| Rank | Finding | Recent evidence |
|---:|---|---|
| 1 | `source.build_crg_graph` maintenance loop — **retired/resolved 2026-07-21** | 37 historical calls, score 2525.05, latest 20260626; the separate graph export no longer exists. |
| 2 | `source.repair_crg_cache` maintenance loop | 37 recent calls, score 2493.68, latest 20260626. |
| 3 | `monolith.discover` routing/availability errors | 3,711 recent calls, 31 recent errors, max payload 535,812 bytes. |
| 4 | Blueprint mutating action failures | `remove_event_dispatcher` 73/73 recent errors, `add_event_node` 58/58, `rename_function` 20/20, `override_parent_function` 16/16, `duplicate_graph` 12/12. |
| 5 | `source.read_file` coverage/read failures | 91 recent calls, 40 recent errors. |
| 6 | Large payloads | `console.search_objects` max 697,478 bytes; `project.search` max 202,559 bytes. |
| 7 | Duplicate retries | `cppreflect`, `risk`, `decision`, and Blueprint read actions repeat identical retry signatures. |

This historical evidence means the spec must not focus only on future domain wishlist. Current prioritization excludes the retired graph builder while retaining the remaining agent-facing reliability findings.

## 3. Practitioner Blockers

### 3.1 Engine/tools maintainer

| Blocker | Evidence | Required direction |
|---|---|---|
| MCP availability is the largest immediate blocker and server-log invisible. | `Analyzer/analyze_session_transcripts.py` exists because server logs only record calls that reach Monolith; current probe was down. | Add first-class availability/preflight workflow and Codex-direct/proxy reconnect hardening. |
| No useful offline catalog/schema path when MCP is down. | Docs and skills say live `monolith_discover` is authoritative; offline CLI exposes only partial `monolith guide` for core. | Add snapshot-backed offline `monolith discover/find/status/get_action_metadata_coverage`. |
| Offline source health is stale. | `Tools/MonolithQuery/monolith_query.cpp` expects source schema `"1"` while `MonolithSourceSchema.h` is v3 and live health uses the v3 constant. | Make offline source health use the shared schema authority or update the expected contract. |
| Project health is too structural. | `project health` can be OK while `project review_context` reports missing detail signals such as Blueprint indexed with 0 graph nodes. | Add semantic/detail coverage samples to project health/readiness. |
| Bridge offline/live contract drift. | Offline bridge exposes only `search_asset_symbols` but can suggest live-only `bridge.build_attachment`, `bridge.get_index_status`, `bridge.start_indexing`. | Make `next_actions` availability-aware. |
| Read-only offline workflows append diagnostics by default. | Query/proxy logs append by default unless env disables logging. | Add first-class `--no-log` / `--readonly` controls and verify no files are written. |
| Action metadata is not a hard gate. | Metadata coverage tracks outputs/next actions/planning signals, but high-traffic domains still have `not_declared` and ParamGuard debt. | Gate high-traffic namespaces on metadata/param coverage. |

### 3.2 Gameplay engineer

| Blocker | Required direction |
|---|---|
| No first-class gameplay feature workflow across Blueprint, GAS, Input, AI, GameFeatures, and WorldConditions. | Add a manifest workflow for `input -> GAS -> Blueprint/AI -> runtime proof`. |
| Enhanced Input authoring cannot set triggers/modifiers deeply enough for real input behavior. | Add trigger/modifier writers for Input Actions and mappings, plus richer validation. |
| GameFeatures is inspection-only. | Add guarded authoring and activation-readiness planning; runtime activation requires explicit confirmation and PIE/editor-state gates. |
| WorldConditions is read-only/default-off and cannot author SmartObject/world-state gates. | Add schema describe, spec import/export, dry-run strict apply, and validation. |
| LogicDriver/ComboGraph optional plugin surfaces have schema/skill drift risk. | Add fixture-backed live schema parity checks and optional-plugin on/off matrix. |

### 3.3 Technical artist / content creator

| Blocker | Required direction |
|---|---|
| Several content domains are discovery-only. Cloth, Chaos Fracture, Dataflow, Paper2D, MetaHuman, and nDisplay often stop at status/list/metadata. | Promote only the highest-value authoring slices behind optional-module gates and proof fixtures. |
| ModelGen is a deterministic placeholder/import boundary, not production text-to-3D. | Treat remote generation as provider-owned; add game-ready import validation/provenance rather than pretending generation is solved. |
| Game-ready asset flow is fragmented across ImageGen, ModelGen, Interchange, Mesh, Material, Sprite, Niagara, Animation, and Asset. | Add a `game_ready_asset` workflow with standard compile/preview/budget/saved proof. |
| Verification quality is uneven. | Normalize content proof: compile diagnostics, preview artifact, asset validation, budget check, save status, warnings/errors. |

### 3.4 Level/world builder

| Blocker | Required direction |
|---|---|
| WorldGen's town/building pipeline is gated off by default. | Promote one stable always-on or explicitly gated `plan/build/validate` path with deterministic seeds. |
| PCG and Water are inspection-only. | Add guarded authoring specs only after capability status and plugin matrix tests are reliable. |
| HLOD cannot close the optimization loop. | Add async build/clear/poll/validate actions or return structured blocker, never report-only success. |
| LevelInstance lifecycle is mostly unavailable/preview. | Add dialog-free lifecycle ownership or clearly keep it out of unattended workflows. |
| No durable world-builder transaction across scene edit -> analysis -> save -> source control. | Add `level_workflow` manifest returning touched actors/assets/packages, dirty state, validation, and recovery limits. |

### 3.5 UI, cinematics, audio, localization practitioner

| Blocker | Required direction |
|---|---|
| UMG/CommonUI ViewModel boundary is policy, not enforceable workflow. | Add ViewModel property/event binding, boundary audit, and UI spec integration. |
| Localization is StringTable CRUD, not a shipping localization pipeline. | Add targets/gather/export/import/compile/audit flows and UI literal migration. |
| LevelSequence and MRQ are split primitives, not shot-to-render workflow. | Add sequence validate/apply MRQ preset/render smoke/verify output contract. |
| Slate/EUW can observe editor UI but cannot prove interaction flows. | Add explicit test-mode click/type/key/wait/capture actions or keep the limitation visible. |
| MetaSound authoring lacks live interface discovery and preview/audition proof. | Add interface listing, strict preflight, validate, render preview, and analysis. |

## 4. Shared Workflow Result Contract

All new P1 practitioner workflows must return a consistent object. Domain fields may vary, but the top-level contract is stable:

```json
{
  "status": "planned|applied|partial|blocked|failed",
  "workflow_id": "gameplay_feature|game_ready_asset|level_workflow|ui_shipping|shot_render|...",
  "input": {},
  "dry_run": true,
  "confirm": false,
  "plan": {
    "steps": [],
    "preconditions": [],
    "optional_dependencies": []
  },
  "actions": [],
  "touched": {
    "actors": [],
    "assets": [],
    "packages": [],
    "files": []
  },
  "dirty_packages": [],
  "source_control": {
    "provider": "",
    "prepared": false,
    "checked_out": [],
    "marked_for_add": [],
    "blocked": []
  },
  "validation": {
    "compile": {},
    "asset_validation": {},
    "runtime": {},
    "budget": {},
    "accessibility": {}
  },
  "proof": {
    "read_back": [],
    "preview_artifacts": [],
    "logs": [],
    "benchmarks": []
  },
  "artifacts": [],
  "warnings": [],
  "errors": [],
  "rollback": {
    "automatic": false,
    "limitations": []
  },
  "next_actions": []
}
```

Rules:

- `dry_run=true` must not dirty packages.
- Mutating workflow actions require `confirm=true` unless the existing namespace contract already defines a safe fixture-only mutation.
- Every mutating workflow must disclose dirty packages and source-control status.
- `status=applied` requires read-back proof, not just handler success.
- `partial` and `blocked` are valid outcomes only when the response names the exact failed step and recovery path.
- Large arrays must be capped and paginated with `truncated`, `next_cursor`, and `limits`.
- A workflow must not suggest a `next_action` that is unavailable in the current live/offline/plugin/profile context without marking it as unavailable.

## 5. P0 Requirements - Reliability and Trust

### P0.1 MCP availability and offline catalog

| Requirement | Contract |
|---|---|
| P0.1.1 deterministic probe | `Scripts/recover_mcp.ps1 -ProbeOnly` returns stable `RESULT=` tokens and never launches the editor. |
| P0.1.2 recovery plan | A new or extended readiness action reports endpoint URL, listener status, editor candidate status, headless log path, and bounded next steps. |
| P0.1.3 offline catalog snapshot | `Binaries\monolith_query.exe monolith discover`, `find`, `status`, and `get_action_metadata_coverage` work while MCP is down, using a stamped snapshot and `requires_live_editor` markers. |
| P0.1.4 watchdog supervisor | `Scripts/watch_mcp.ps1` keeps `/health` supervised during long agent sessions; when the editor process is gone, it runs the host editor UBT build, restart-triggered source maintenance, relaunches through `recover_mcp.ps1`, then runs post-health asset maintenance; when a headless editor process exists but stays unhealthy through the recover timeout, it stops only that headless process and reruns the same sequence; recover uses modal-safe headless asset-editor settings. |
| P0.1.5 Codex-direct path | Availability fixes cover direct streamable-HTTP clients, not only `monolith_proxy.py` / `.js`. |

Acceptance:

```powershell
Scripts\recover_mcp.ps1 -ProbeOnly
Scripts\watch_mcp.ps1 -ProbeOnly
Binaries\monolith_query.exe monolith status
Binaries\monolith_query.exe monolith discover --namespace blueprint --mode actions --limit 10
Binaries\monolith_query.exe monolith get_action_metadata_coverage --namespace blueprint
python Analyzer\analyze_session_transcripts.py --since 20260626 --out Saved\Monolith\SessionAnalysis\roi-current
```

Transport/availability errors in fresh session transcripts must drop, watchdog runs must emit a clear `[timestamp][EventType] result=<camelCase>` line, and offline catalog output must be clearly marked as snapshot data.

### P0.2 Source/project/bridge readiness trust

| Requirement | Contract |
|---|---|
| P0.2.1 source schema parity | Offline source health uses `MonolithSourceSchema::SchemaVersion` v3 or an equivalent generated constant. A healthy v3 DB must not warn `expected 1`. |
| P0.2.2 project semantic readiness | `project.health` warns when sampled Blueprint/detail-index rows are structurally present but semantically empty, with sample paths and repair/index actions. |
| P0.2.3 bridge next actions | Offline bridge responses never suggest live-only actions without `requires_live_editor=true` or a live recovery path. |
| P0.2.4 no-log mode | `monolith_query.exe --no-log` or `--readonly` creates no query/proxy log rows. Env disable remains supported. |

Acceptance:

```powershell
Binaries\monolith_query.exe source health --include-counts=true --include-deep-checks=true
Scripts\check_index_freshness.ps1
Binaries\monolith_query.exe project health --include-counts=true
Binaries\monolith_query.exe bridge search_asset_symbols --symbol UObject --limit 5
$env:MONOLITH_TOOL_LOG_ENABLED=0; Binaries\monolith_query.exe source search_source UObject --limit 1
Binaries\monolith_query.exe --no-log source search_source UObject --limit 1
```

### P0.3 Action contract metadata gate

| Requirement | Contract |
|---|---|
| P0.3.1 metadata fields | High-traffic actions expose factual `skill`, `planning_signals`, `preconditions_status`, `output_contract_status`, `next_actions_status`, `available_offline`, `requires_live_editor`, `mutates_assets`, `writes_logs`, `long_running`, and `supports_progress` where applicable. |
| P0.3.2 high-traffic thresholds | `monolith.get_action_metadata_coverage` or an offline equivalent fails/gates if high-traffic namespaces fall below agreed thresholds. |
| P0.3.3 ParamGuard debt | Existing malformed-param failures must return `-32602` with field-specific error data and candidate/recovery hints. |
| P0.3.4 benchmark proof | `ActionGuidance` remains the benchmark for routing/recovery quality and is refreshed after metadata changes. |

Acceptance:

```powershell
python Scripts\action_guidance_benchmark.py generate
python Scripts\test_action_guidance_benchmark.py
python Scripts\ci_static_checks.py --config .github/monolith-static-ci.json --github check
```

When a live editor is available, run the full ActionGuidance benchmark against `http://localhost:9316/mcp` and compare to the latest checked-in result.

### P0.4 Blueprint high-error action recovery

Recent logs show high-error rates on exactly the mutating actions agents use for real Blueprint work. Fixing these is more valuable than adding adjacent Blueprint verbs.

| Action | Recent problem evidence | Required response contract |
|---|---:|---|
| `remove_event_dispatcher` | 73/73 recent errors | Missing target must include available dispatchers, accepted aliases, and whether no-op removal is allowed. |
| `add_event_node` | 58/58 recent errors | Must distinguish custom event, parent override, interface event, RPC constraints, and duplicate name. |
| `rename_function` | 20/20 recent errors | Must name offending function, list candidate functions, and preserve references or explain why not. |
| `override_parent_function` | 16/16 recent errors | Must list overrideable parent functions and explain non-overridable choices. |
| `duplicate_graph` | 12/12 recent errors | Must distinguish graph-not-found, name conflict, function/macro/event graph kinds, and copy limitations. |
| `add_component` | 29 recent errors | Must resolve class names/friendly names with candidates and reject unsafe duplicates explicitly. |
| `create_blueprint` | 18 recent errors | Existing target path must return duplicate guidance, not a generic failure. |

Acceptance:

```powershell
python Scripts\asset_editing_benchmark.py generate
python Scripts\test_asset_editing_benchmark.py
python Analyzer\analyze_invocation_logs.py --log-root Logs --since 20260626 --rank-by-recency --category high_error_rate
```

Live acceptance requires focused AssetEditing rows for these actions to pass with read-back proof, plus the analyzer showing reduced recent high-error findings after the fix reaches the running editor.

### P0.5 Discovery and large-result payload control

| Requirement | Contract |
|---|---|
| P0.5.1 `monolith.discover` projection | Default discover output remains bounded. Large namespace listings must support `limit`, `offset`, `filter`, `category`, `detail=false`, `planning_detail=compact`, `planning_detail=full` opt-in, `schema_detail=compact`, `schema_detail=full` opt-in, `detail=true` page capping to the default limit, `truncated`, `next_cursor`, and `limits`. |
| P0.5.2 `console.search_objects` cap | Default response must be compact; full help/value rows require explicit detail/projection. |
| P0.5.3 `project.search` cap | Content-inclusive search must expose provenance fields but keep result rows bounded and cursorable. |
| P0.5.4 analyzer guard | `Analyzer/analyze_invocation_logs.py` continues to flag `large_result` with recent windows. |

Acceptance:

```powershell
python Analyzer\analyze_invocation_logs.py --log-root Logs --since 20260626 --rank-by-recency --category large_result
Binaries\monolith_query.exe console search_objects r.Streaming --limit 10
Binaries\monolith_query.exe project search Health --limit 10 --include-content=true
```

### P0.6 Long-action job/progress/cancel contract

| Requirement | Contract |
|---|---|
| P0.6.1 job shape | Long actions return `status=started`, `job_id`, `poll_action`, and cancellation/progress support where possible. |
| P0.6.2 pre-SSE progress | Before real SSE, progress is available through poll resources/actions and invocation logs. |
| P0.6.3 cancellation | Reindex, CRG graph, build, import, and asset-batch actions opt into cooperative cancellation when they have loop boundaries. |
| P0.6.4 no false complete | Actions must not report `Completed` until the underlying UE operation has really finished. |

## 6. P1 Requirements - Practitioner Workflow Contracts

### P1.1 Gameplay feature workflow

Add a workflow contract, either under `monolith.workflow` or a domain namespace, for a real gameplay feature:

```text
plan_feature -> validate_feature_manifest -> apply_feature_manifest -> prove_feature_runtime
```

Minimum manifest slices:

- Enhanced Input IA/IMC with mapping triggers/modifiers.
- GAS ability/effect/cue/input binding.
- Blueprint pawn/controller/component wiring.
- AI Behavior Tree/Blackboard/StateTree readiness where enabled.
- Optional GameFeatureData and WorldCondition gates.
- PIE proof: input Started/Completed/Canceled activates/releases ability; cue/event probe records evidence; AI starts on possess.

### P1.2 Game-ready asset workflow

Add a content workflow that composes existing asset, imagegen, modelgen, interchange, material, mesh, niagara, animation, and sprite proof:

```text
plan_asset -> import_or_generate -> build_material_or_effect -> validate_game_ready -> save_and_report
```

Minimum proof object:

- source/provenance sidecar,
- compile diagnostics,
- preview artifact or explicit headless blocker,
- mesh/material/effect budget,
- saved package status,
- source-control prepare status,
- warnings/errors/artifacts.

### P1.3 Level/world builder workflow

Add a `level_workflow` / `worldbuilder` contract:

```text
plan -> apply -> analyze -> save -> source_control_prepare -> verify_reload
```

Minimum slices:

- deterministic seed and dry-run descriptor,
- touched actors/assets/packages,
- scene/worldgen/leveldesign validation,
- collection/source-control status,
- no-dialog guarantee for unattended flows,
- explicit rollback limitations.

### P1.4 UI/localization/cinematics/audio shipping workflow

Add shipping proof workflows rather than only primitive CRUD:

| Workflow | Minimum proof |
|---|---|
| UI ViewModel binding | widget tree read-back, ViewModel boundary audit, binding/event-command proof, compile/save status. |
| Localization | target discovery, gather/export/import/compile, StringTable reference audit, UI literal-to-StringTable migration. |
| Shot render | LevelSequence binding validation, MRQ preset apply, tiny render smoke, output artifact verification. |
| MetaSound/audio | interface discovery, strict spec preflight, validate, preview/audition artifact or explicit blocker. |
| Slate/EUW test flow | test-mode gated click/type/key/wait/capture, with no production input simulation unless explicitly enabled. |

## 7. P2 Requirements - Domain Authoring Gaps

P2 work should only start after the P0 substrate and at least one P1 workflow prove the shared contract.

| Domain | Candidate authoring slice | Gate |
|---|---|---|
| Enhanced Input | action/mapping trigger and modifier writers | Fixture IA/IMC proves Hold/DeadZone/Chord/Swizzle read-back. |
| GameFeatures | plugin creation plan, GameFeatureData feature actions, activation readiness | Explicit confirm and plugin-enabled matrix. |
| WorldConditions | query schema, condition apply/export/import/validate | Strict dry-run, array/map/set limitations declared. |
| PCG | graph-from-spec, node params, wiring, execute generation | Plugin matrix and generated actor validation. |
| Water | spawn/configure body, spline edit, material assign, rebuild zone | Plugin matrix and world reload proof. |
| HLOD | async build/poll/clear/validate | Real HLOD output exists or structured blocker. |
| LevelInstance | dialog-free create/edit/commit/discard/load/unload | No-dialog unattended test and conflict detection. |
| Dataflow | graph-from-spec and node/property edits | Optional plugin enabled fixture and compile/validate proof. |
| Chaos/Cloth/Paper2D/MetaHuman/nDisplay | smallest useful guarded authoring slice | Release-safe optional dependency guard and read-back proof. |

## 8. Sequencing

| Phase | Work | Done gate |
|---:|---|---|
| 0 | Freeze this spec and verify current evidence | Static checks, diff check, status check. |
| 1 | MCP availability + offline catalog + source health schema parity | Offline `monolith discover/status/coverage` works while MCP is down; source health no longer warns on healthy v3 DB. |
| 2 | Metadata and payload gates | ActionGuidance refreshed; large-result findings reduced; metadata coverage gate exists for high-traffic namespaces. |
| 3 | Blueprint high-error action recovery | Focused AssetEditing rows pass; latest analyzer window no longer ranks the same high-error Blueprint actions. |
| 4 | First P1 workflow | Choose one of gameplay feature, game-ready asset, or level workflow; implement as a manifest/proof contract with live fixture acceptance. |
| 5 | Optional/domain authoring slices | Add only slices that plug into the workflow contract and pass optional-plugin matrix tests. |

Do not bundle unrelated phase work into a single PR. Each PR should list the WorkFingerprint and duplicate/collision checks required by `AGENTS.md`.

## 9. Verification Plan

### Static gates

```powershell
python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check
git diff --check
git status --short
```

### Analyzer gates

```powershell
python Analyzer\analyze_invocation_logs.py --log-root Logs --since 20260626 --rank-by-recency --out Saved\Monolith\LogAnalysis\roi-current
python Analyzer\analyze_session_transcripts.py --since 20260626 --out Saved\Monolith\SessionAnalysis\roi-current
```

### Offline CLI gates

```powershell
Binaries\monolith_query.exe source health --include-counts=true --include-deep-checks=true
Binaries\monolith_query.exe project health --include-counts=true
Binaries\monolith_query.exe bridge search_asset_symbols --symbol UObject --limit 5
Binaries\monolith_query.exe console search_objects r.Streaming --limit 10
```

### Live editor gates

Run only when the shared MCP endpoint is intentionally available:

```powershell
Scripts\recover_mcp.ps1 -ProbeOnly
python Scripts\action_guidance_benchmark.py run --mcp-url http://localhost:9316/mcp --tasks Benchmarks\ActionGuidance\tasks.jsonl --label roi-current --output-dir Saved\Monolith\Benchmarks\ActionGuidance\roi-current
python Scripts\asset_editing_benchmark.py run --mcp-url http://localhost:9316/mcp --tasks Benchmarks\AssetEditing\tasks.jsonl --label roi-current --output-dir Saved\Monolith\Benchmarks\AssetEditing\roi-current
python Scripts\project_index_benchmark.py run --mcp-url http://localhost:9316/mcp --tasks Benchmarks\ProjectIndex\tasks.jsonl --label roi-current --output-dir Saved\Monolith\Benchmarks\ProjectIndex\roi-current
python Scripts\ai_capability_benchmark.py run --mcp-url http://localhost:9316/mcp --output-dir Saved\Monolith\Benchmarks\AICapability\roi-current --label roi-current
```

### Optional dependency gates

For each optional domain touched:

- plugin disabled: discover/status returns capability status and no hard load failure;
- plugin enabled: schema discovery, malformed-param guard, dry-run, apply, read-back, and save/reload proof pass;
- release build: no hard dependency on absent plugins.

## 10. Traceability

| Req | Task family | Test family |
|---|---|---|
| P0.1 | MCP availability and offline catalog | `recover_mcp.ps1 -ProbeOnly`, `watch_mcp.ps1 -ProbeOnly`, `watch_mcp.ps1 -Once -RunDailyReindexNow` only on an intentional live endpoint maintenance smoke, offline `monolith` CLI actions, session analyzer. |
| P0.2 | Source/project/bridge readiness | source/project health, index freshness script, bridge next-action snapshot tests. |
| P0.3 | Metadata/ParamGuard gate | ActionGuidance, metadata coverage, param guard automation, static CI. |
| P0.4 | Blueprint high-error recovery | focused AssetEditing rows, analyzer high-error regression check. |
| P0.5 | Payload control | large-result analyzer, console/project/discover pagination tests. |
| P0.6 | Long-action progress/cancel | async job registry tests, progress polling, cancellation fixtures. |
| P1.1 | Gameplay feature workflow | IA/IMC + GAS + Blueprint + AI + PIE proof fixture. |
| P1.2 | Game-ready asset workflow | import/generate + compile/preview/budget/save fixture. |
| P1.3 | Level/world workflow | disposable map apply/analyze/save/reload/source-control fixture. |
| P1.4 | UI/localization/cinematics/audio workflow | ViewModel binding, localization compile, MRQ smoke, MetaSound preview fixtures. |
| P2 | Domain authoring gaps | Optional-plugin matrix and read-back proof per domain. |

## 11. Out of Scope For This Spec PR

- Code implementation of the listed backlog.
- Updating README/API action counts.
- Editing existing domain specs unless a future implementation PR changes the corresponding contract.
- Creating a PR while the worktree remains unrelatedly dirty.
- Claiming UE/editor verification without running the live editor commands in the current VM.
