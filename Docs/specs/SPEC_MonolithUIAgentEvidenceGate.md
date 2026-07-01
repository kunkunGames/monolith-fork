# Monolith UI Agent Evidence Gate

**Parent:** [SPEC_MonolithUI.md](SPEC_MonolithUI.md), [SPEC_MonolithEditor.md](SPEC_MonolithEditor.md), [SPEC_MonolithPractitionerWorkflowROI.md](SPEC_MonolithPractitionerWorkflowROI.md)
**Engine:** Unreal Engine 5.7+
**Status:** Proposed implementation spec
**Created:** 2026-07-01
**Owner modules:** MonolithUI, MonolithCore workflow actions, MonolithEditor, MonolithSlate, Analyzer, Skills/unreal-ui
**Scope:** First-slice implementation plan for making Monolith-using agents complete UMG UI work with artifact-backed proof by hardening the existing workflow proof contract instead of adding a separate UI QA platform.
**Non-goals:** Figma/PSD import, AI visual critique, design-token authoring, broad UI restyling, committing `Logs/`, replacing existing `ui` actions, or adding Slate input simulation outside an explicitly test-gated action.

---

## 1. Decision

Monolith UI agents need the existing workflow proof path to prove that a Widget Blueprint was changed, compiled, structurally audited, visually rendered, optionally round-tripped, and optionally exercised at runtime. The evidence loop is inspired by SuperLoopy-style UI proof, but it must be implemented as a thin extension of Monolith's shared workflow result contract over the existing `ui`, `editor`, `workflow`, `slate`, and `Analyzer` surfaces.

The first implementation must not create a second ledger dialect or a broad UI QA platform. It should ship five tightly-scoped improvements:

| Priority | Improvement | Contract |
|---:|---|---|
| P0 | Workflow Proof Hardening | Extend `workflow.ui_shipping_widget_blueprint` with `proof_profile: minimal\|visual\|runtime` and use the shared workflow result shape from `SPEC_MonolithPractitionerWorkflowROI.md`. |
| P0 | Visual Artifact Verifier | Add `ui.verify_widget_visual_artifacts` to validate files produced by `editor.capture_scene_preview(asset_type="widget")` with exists/size/dimensions/hash/nonblank checks. |
| P0 | Limited WBP Round Trip | Require `dump_ui_spec -> dry-run build -> apply build -> dump/compare` only for spec-authored or clearly `FUISpecDocument`-representable WBP edits. |
| P1 | Schema Confusion Guard | Add exact payload examples and analyzer fixtures for common UI action payload mistakes; avoid new helper actions unless measured logs still show recurring failures. |
| P1 | Optional Runtime Profile | Add a runtime proof profile that composes existing async PIE/input/capture primitives before introducing UI-owned start/poll/stop actions. |

Existing `workflow.ui_shipping_widget_blueprint` remains the starting point. It already composes tree/spec/binding/layout/accessibility/navigation/CommonUI read-back and declares compile/preview/save next actions. This spec turns that first slice into stronger proof profiles rather than duplicating every primitive action.

## 2. Current Baseline

The relevant existing surfaces are:

| Surface | Current behavior | Gap |
|---|---|---|
| `ui.dump_ui_spec` + `ui.build_ui_from_spec` | Serializer/builder pair can round-trip supported UMG fields through `FUISpecDocument`. | No agent-facing workflow requires round-trip proof before/after existing WBP edits. |
| `ui.audit_widget_layout` | Structural proof for Canvas misuse, edge-anchor mismatch, and dynamic text containment. | The action explicitly does not replace PC/mobile screenshot gates. |
| `ui.compile_widget` / `ui.dump_blueprint_compile_log` | Structured compile errors/warnings are available. | `workflow.ui_shipping_widget_blueprint` currently declares compile as a next action instead of running it in a final gate. |
| `editor.capture_scene_preview` | Supports `asset_type="widget"` through `FWidgetRenderer` with `resolution`, `scale`, and `output_path`. | It is editor-owned and does not prove the returned PNG exists, has valid dimensions, or contains nonblank pixels. |
| `ui.validate_frontend_menu_flow` | Static frontend/menu contract validation is available. | It states that runtime screen navigation, clicks, CommonUI input routing, and graph wiring are not proven. |
| `editor.run_pie_smoke` / `capture_pie_movement_clip` / PIE helpers | Async PIE proof primitives exist because blocking the editor tick inside a handler is unsafe. | There is no UI workflow profile that composes these primitives into focus/stack/input assertions. |
| Invocation logs + analyzer | Schema confusion and large-result findings can be mined from `Logs/`. | UI-specific schema confusion and evidence-regression fixtures are not first-class. |

One known documentation/API mismatch has been accounted for in this contract: `editor.capture_scene_preview` currently reports the written PNG as `output_file`. New UI wrappers must normalize this as `path` and preserve the original producer field under `producer_output`.

## 3. Shared Workflow Proof Extension

Every UI proof run must return the shared workflow result shape required by `SPEC_MonolithPractitionerWorkflowROI.md`: top-level status, validation, proof, artifacts, warnings/errors, and next_actions. UI-specific evidence must be nested under `proof.ui_evidence` rather than introducing a standalone `ui_evidence_gate.v1` envelope.

The UI evidence block may also be written to `Saved/Monolith/UIEvidence/<run_id>/ui_evidence.json`.

```json
{
  "schema_version": "ui_workflow_proof.v1",
  "workflow_id": "ui_shipping",
  "proof_profile": "minimal|visual|runtime",
  "run_id": "ui-20260701-001",
  "request_id": "caller-request-id",
  "status": "pass|fail|warn|blocked|skipped",
  "target_kind": "widget_blueprint|frontend_flow|slate_widget",
  "target_asset_paths": ["/Game/UI/WBP_Menu"],
  "target_map": "/Game/Maps/TestMap",
  "screen_ids": ["main_menu"],
  "changed_actions": [],
  "checks": [],
  "artifacts": [],
  "action_records": [],
  "summary": {
    "required_count": 0,
    "passed_count": 0,
    "failed_count": 0,
    "blocked_count": 0,
    "warning_count": 0
  },
  "environment": {
    "can_render": true,
    "pie_available": true,
    "null_rhi": false,
    "commonui_available": true,
    "logs_enabled": true
  },
  "limitations": []
}
```

### Check Rows

Each `checks[]` entry must use this shape:

```json
{
  "check_id": "compile:WBP_Menu",
  "category": "compile|layout|accessibility|round_trip|frontend_contract|visual_artifact|runtime|slate|schema|log_analyzer",
  "required": true,
  "status": "pass|fail|warn|blocked|skipped",
  "severity": "info|low|medium|high|critical",
  "namespace": "ui",
  "action": "dump_blueprint_compile_log",
  "expected": {},
  "observed": {},
  "evidence_refs": ["artifact:compile_log"],
  "failure_code": "",
  "message": "",
  "suggested_fix": ""
}
```

Required rows must decide the final workflow status:

- Any required `fail` makes the workflow result `fail`.
- Any required `blocked` makes the workflow result `blocked` unless another required row already failed.
- Optional `warn` rows do not fail unless the request sets `treat_warnings_as_errors=true`.
- `skipped` is allowed only when the row is optional or the `limitations[]` entry names the concrete blocker.

### Artifact Rows

Each `artifacts[]` entry must use this shape:

```json
{
  "artifact_id": "visual:desktop",
  "kind": "png|gif|json|jsonl|log|workflow_proof",
  "producer_action": "editor.capture_scene_preview",
  "path": "Saved/Monolith/UIVisualQA/run/desktop.png",
  "inline_ref": "",
  "sha256": "hex",
  "byte_count": 12345,
  "mime_type": "image/png",
  "width": 1280,
  "height": 720,
  "frame_count": 1,
  "capture_source": "widget_renderer|pie_viewport|slate_widget|thumbnail",
  "fallback_used": false,
  "thumbnail_fallback": false,
  "created_at": "2026-07-01T00:00:00Z"
}
```

Thumbnail evidence is weak evidence. It may appear in `artifacts[]`, but it must not satisfy `visual_artifact` or `runtime` required checks.

## 4. Required Proof Families

### 4.1 `workflow.ui_shipping_widget_blueprint` Proof Profiles

Extend `workflow.ui_shipping_widget_blueprint` with an explicit proof profile. The default profile must remain conservative so existing callers are not surprised by long-running editor work or dirty-state changes.

| Profile | Purpose | Required checks |
|---|---|---|
| `minimal` | Keep the current low-latency read-back workflow, but report exactly which checks remain as required `next_actions`. | tree/spec/binding/layout/accessibility/navigation/CommonUI read-back as requested. |
| `visual` | Prove a changed WBP compiles, audits, and renders offscreen. | compile, layout, accessibility when requested, one or more verified widget capture artifacts. |
| `runtime` | Prove a menu/frontend flow can be interacted with in PIE. | all `visual` checks plus static frontend contract, async runtime steps, focus/stack/property assertions, runtime captures. |

Suggested request:

```json
{
  "widget_asset_paths": ["/Game/UI/WBP_MainMenu"],
  "proof_profile": "visual",
  "request_id": "ui-gate-001",
  "run_id": "ui-gate-001",
  "output_dir": "Saved/Monolith/UIEvidence/ui-gate-001",
  "treat_warnings_as_errors": true,
  "layout_rule_profile": "shipping",
  "suppress_layout_rule_ids": [],
  "visual_profiles": [
    {"name": "desktop", "resolution": [1280, 720], "dpi_scale": 1.0},
    {"name": "mobile", "resolution": [1280, 720], "dpi_scale": 1.0, "safe_zone": {"left": 48, "top": 24, "right": 48, "bottom": 24}}
  ],
  "round_trip_check": "auto|force|off",
  "runtime_flow": null
}
```

Suggested response: the shared workflow result shape with `proof.ui_evidence` populated as described in Section 3.

Implementation rules:

- `minimal` must not run compile, capture, save, source-control checkout, or PIE unless the caller explicitly opts in.
- `visual` may run compile and widget capture because the caller opted into proof; it must report dirty-state and save status but must not silently save unless the existing workflow already owns that behavior.
- `runtime` must be async for PIE work and may return `blocked` with concrete next actions if the editor/runtime context cannot start proof safely.
- `round_trip_check=auto` must run only when the changed surface is spec-authored or clearly representable by `FUISpecDocument`.
- `layout_rule_profile` is forwarded to `ui.audit_widget_layout`; default is `shipping`, or `strict` for `proof_profile="runtime"`. Intentional static-layout exceptions must use `suppress_layout_rule_ids`, which forwards to `ui.audit_widget_layout.suppress_rule_ids`.
- Mobile/console visual profiles must declare both `dpi_scale` and `safe_zone`. Missing data is reported as a `validation.visual_profile.findings[]` entry with `rule_id="DpiSafeZoneProfileMissing"` and blocks the workflow proof; desktop-only profiles remain allowed.

Implementation status (2026-07-02):

- `workflow.ui_shipping_widget_blueprint` now forwards `layout_rule_profile` and `suppress_layout_rule_ids` to the existing `ui.audit_widget_layout` owner action instead of adding a duplicate proof primitive.
- `validation.visual_profile` emits `ui_visual_profile_proof.v1` with `DpiSafeZoneProfileMissing` for mobile/console/handheld/TV profile names that omit `dpi_scale` or `safe_zone`.
- Focused workflow automation covers default `shipping` audit forwarding, runtime `strict` forwarding, invalid layout profile rejection, and a mobile visual profile blocked envelope.

### 4.2 `ui.verify_widget_visual_artifacts`

This action is the P0 visual QA slice. It validates one or more widget PNG artifacts that were produced by `editor.capture_scene_preview(asset_type="widget")` or by a workflow wrapper that delegates to that editor action. It does not need to own capture in v1; it owns artifact trust.

Parameters:

| Param | Type | Meaning |
|---|---|---|
| `asset_path` | string | Widget Blueprint path. |
| `captures` | array | `{profile?, path?, output_file?, expected_resolution?}` rows. `output_file` is accepted for compatibility and normalized to `path`. |
| `output_dir` | string | Optional directory for `manifest.json`. Defaults under `Saved/Monolith/UIVisualQA/<run_id>`. |
| `baseline_dir` | string | Optional folder of baseline PNGs by profile name. |
| `diff_threshold` | number | Optional maximum diff ratio. |
| `fail_on_blank` | bool | Default true. Fails uniform/transparent/near-empty captures. |
| `request_id` | string | Echoed and forwarded to the workflow proof. |

Response:

```json
{
  "ok": true,
  "schema_version": "ui_visual_artifacts.v1",
  "run_id": "ui-gate-001",
  "asset_path": "/Game/UI/WBP_MainMenu",
  "manifest_path": "Saved/Monolith/UIVisualQA/ui-gate-001/manifest.json",
  "captures": [
    {
      "profile": "desktop",
      "path": "Saved/Monolith/UIVisualQA/ui-gate-001/desktop.png",
      "width": 1280,
      "height": 720,
      "sha256": "hex",
      "byte_count": 12345,
      "blank": false,
      "transparent_ratio": 0.02,
      "unique_color_estimate": 512,
      "diff": {
        "baseline_path": "",
        "diff_path": "",
        "diff_ratio": 0.0,
        "passed": true
      }
    }
  ],
  "checks": [],
  "warnings": [],
  "limitations": []
}
```

Requirements:

- The action must return explicit `blocked` or `unavailable_render_path` in commandlet, server, null-RHI, or otherwise non-rendering contexts. It must not return a blank success.
- File validation must check existence, non-zero bytes, PNG dimensions, hash, and nonblank/non-uniform pixel metrics.
- Baseline diff, multi-profile defaults, text-scale sweeps, input-type sweeps, and colorblind profiles are optional v2 work. The v1 response shape reserves stable `diff` fields, but v1 should not require a baseline.
- A generated-WBP automation test must exercise the action without depending on engine sample content.
- If the underlying editor action returns `output_file`, the wrapper must normalize it to `path`.

`ui.capture_widget_visual_matrix` is deferred until repeated real UI tasks show that one-shot capture orchestration is worth promoting. Before that point, `workflow.ui_shipping_widget_blueprint(proof_profile="visual")` can compose `editor.capture_scene_preview` and `ui.verify_widget_visual_artifacts`.

### 4.3 Limited WBP Round-Trip Proof

Widget Blueprint edits must use this workflow only when the changed surface is spec-authored or representable by `FUISpecDocument`:

1. `ui.dump_ui_spec({asset_path, emit_defaults:false, request_id})`
2. Build a candidate spec from the edit plan or from the dumped spec.
3. `ui.build_ui_from_spec({asset_path, spec:<candidate>, dry_run:true, overwrite:<explicit>, request_id})`
4. Inspect both transport success and semantic payload success. A returned payload with `bSuccess=false` is a failed proof check even when the MCP call itself succeeded.
5. Apply only after the dry-run payload reports valid validation and acceptable diff.
6. `ui.dump_ui_spec` again and compare supported fields against the candidate or expected patch.
7. Run compile, layout audit, and visual artifact verification before reporting done.

Canonical action envelope example:

```json
{
  "asset_path": "/Game/UI/WBP_RoundTrip",
  "dry_run": true,
  "overwrite": true,
  "request_id": "ui-rt-001",
  "spec": {
    "version": "1",
    "rootWidget": {
      "type": "CanvasPanel",
      "id": "Root",
      "children": []
    }
  }
}
```

Requirements:

- `rootWidget` is nested under `spec`. It is never a top-level action parameter.
- Action envelope schema comes from focused `monolith_discover({namespace:"ui", action:"build_ui_from_spec", mode:"schema"})`.
- Nested document schema comes from `ui.dump_ui_spec_schema`.
- Call builders and examples must keep those two schemas separate.
- `dry_run=true` must remain side-effect free: no asset creation, compile, save, source-control checkout, or dirty-package side effects.
- Automation should strengthen supported-slot round-trip coverage, but per-edit proof must not rebuild unrelated existing widgets through UISpec just to prove unrelated slot families.

Lossy boundaries documented in [SPEC_MonolithUI.md](SPEC_MonolithUI.md) remain valid. Graph-bound native bindings, rich animation curve data, editor-only metadata, and unsupported future slot fields should be reported as limitations, not false failures.

### 4.4 Schema Confusion Guard

The schema-confusion guard is a small set of docs, fixtures, and helper responses that makes the correct UI payload shape hard to miss.

Required work:

| Item | Requirement |
|---|---|
| UI examples | Add examples for `build_ui_from_spec`, `build_menu_from_spec`, `apply_common_menu_transform_spec`, `workflow.ui_shipping_widget_blueprint(proof_profile=...)`, and `ui.verify_widget_visual_artifacts` that use exact envelope fields. |
| Builder/probe | Prefer existing registry/schema validation and focused schema discovery. Add a helper/probe action only if invocation logs still show recurring schema confusion after examples and fixtures land. |
| Analyzer fixture | Add a UI schema-confusion fixture for common mistakes such as object-shaped `screens` where an array is required. |
| Error recommendation | Schema findings should recommend either an alias/coercion or a clearer example. |
| Large discovery pressure | UI skills should prefer focused schema discovery over broad `monolith.discover` dumps. |

Known recent evidence: the local invocation analyzer reported repeated `ui.apply_common_menu_transform_spec` schema confusion where `screens` was passed as a non-array. The implementation should fix that through examples, fixtures, and optional additive input tolerance only if it does not weaken the public contract.

### 4.5 Optional Runtime Profile

Runtime proof must be separate from static `ui.validate_frontend_menu_flow`, but v1 should compose existing async editor primitives before adding a UI-owned runner. A UI-owned start/poll/stop action set should ship only after a concrete CommonUI fixture proves that the manifest/profile approach is too repetitive or too error-prone.

Initial runtime profile contract:

- `workflow.ui_shipping_widget_blueprint(proof_profile="runtime")` accepts a `runtime_flow` manifest.
- The workflow validates the static frontend contract first.
- The workflow starts or delegates to existing async PIE proof primitives, input injection, property reads, and capture actions.
- If the required primitive is missing or PIE cannot start, the result is `blocked` with concrete `next_actions`, not a static pass.

Deferred UI-owned actions:

Actions:

| Action | Purpose |
|---|---|
| `ui.start_frontend_flow_proof` | Start PIE or attach to an active PIE session, apply the flow script, and return `session_id`. |
| `ui.poll_frontend_flow_proof` | Poll until `running|complete|failed|stopped`; return accumulated assertions and captures. |
| `ui.stop_frontend_flow_proof` | Stop and clean up one proof session or all proof sessions. |

Flow step grammar:

```json
{
  "steps": [
    {"op": "push_screen", "screen": "/Game/UI/WBP_MainMenu"},
    {"op": "wait_widget", "query": {"name": "StartButton"}, "timeout_sec": 5},
    {"op": "focus", "widget": "StartButton"},
    {"op": "expect_focus_path", "contains": ["StartButton"]},
    {"op": "inject_input_action", "action": "/Game/Input/IA_Confirm"},
    {"op": "expect_stack_state", "top_screen": "/Game/UI/WBP_SaveSlots"},
    {"op": "expect_property", "object": "SaveSlotList", "property": "Visibility", "equals": "Visible"},
    {"op": "capture", "name": "after_confirm"}
  ]
}
```

Requirements:

- Runtime proof must be async. It must not pump the editor tick synchronously inside one handler.
- Runtime proof passes only when PIE actually ran, every required step executed, all assertions passed, and screenshot artifacts exist.
- Use CommonUI focus/input helper actions where available; report `commonui_unavailable` when the plugin surface is missing.
- UI overlay screenshots should use runtime/console/PIE capture paths when editor viewport capture cannot include overlays.
- The proof result must record before/after focus path, active input type, stack state, property reads, logs, captures, and failure code per step.
- `ui.validate_frontend_menu_flow` remains a static preflight and cannot satisfy runtime proof by itself.

## 5. Failure Codes

Proof implementations must use stable failure codes so agents can recover without guessing.

| Code | Meaning |
|---|---|
| `compile_failed` | WBP compile/read-back returned errors. |
| `layout_findings_failed` | `ui.audit_widget_layout` reported errors or warning-as-error findings. |
| `accessibility_findings_failed` | Accessibility audit failed required checks. |
| `round_trip_mismatch` | Dump/build/dump comparison changed supported fields unexpectedly. |
| `schema_envelope_invalid` | Top-level action payload used wrong or unknown envelope fields. |
| `schema_document_invalid` | Nested `FUISpecDocument` failed validation. |
| `frontend_contract_issues` | Static menu/frontend validation reported issues. |
| `runtime_identity_mismatch` | PIE proof controlled or captured the wrong widget/screen/world identity. |
| `runtime_assertion_failed` | Runtime interaction assertion failed. |
| `pie_compile_refused` | PIE did not start because errored Blueprints were present. |
| `capture_unavailable` | Rendering or capture source is unavailable. |
| `capture_deferred` | Capture could not be completed in the current runtime window. |
| `pixel_blank_or_uniform` | PNG exists but is blank, fully transparent, or near-uniform. |
| `all_frames_uniform_black` | Runtime capture produced only invalid dark frames. |
| `thumbnail_misused_as_viewport` | Thumbnail evidence was used where viewport/widget evidence was required. |
| `artifact_missing` | Manifest references a missing or empty artifact file. |
| `slate_feature_gated` | Requested Slate proof depends on a gated or unavailable Slate feature. |
| `analyzer_escape_hatch_regression` | Logs show new UI work falling back to generic escape hatches such as broad `editor.run_python` instead of typed actions. |

## 6. Implementation Slices

### Slice A - Spec and Skill Contract

- Add this spec.
- Update `Skills/unreal-ui/SKILL.md` to route agent UI completion through workflow proof profiles once actions exist.
- Add a short reference from `SPEC_MonolithUI.md`.

### Slice B - Visual Artifact Verifier

- Add `ui.verify_widget_visual_artifacts`.
- Compose `editor.capture_scene_preview(asset_type="widget")` and artifact verification from `workflow.ui_shipping_widget_blueprint(proof_profile="visual")`.
- Add generated-WBP automation fixtures for success, render-unavailable, blank PNG rejection, path/output_file normalization, and manifest shape.

### Slice C - Workflow Proof Profiles

- Extend `workflow.ui_shipping_widget_blueprint`.
- Reuse `workflow.ui_shipping_widget_blueprint` read-back logic where possible.
- Promote compile and visual proof from "next action" to executable checks only when `proof_profile="visual"` or `proof_profile="runtime"`.

### Slice D - Round-Trip and Schema Guard

- Add schema-shaped examples and focused docs.
- Add analyzer fixture for UI schema confusion.
- Add tests that reject or clearly report wrong envelope-vs-document payloads.
- Add limited round-trip proof for UISpec-owned or spec-representable edits.
- Strengthen supported-slot round-trip automation outside the per-edit gate.

### Slice E - Optional Runtime Profile

- Add a runtime manifest path to `workflow.ui_shipping_widget_blueprint(proof_profile="runtime")`.
- Compose existing async PIE, input, property-read, and capture primitives.
- Add CommonUI/navigation/focus/property assertions for one concrete fixture.
- Add null-RHI/headless unavailable tests.
- Promote `ui.start_frontend_flow_proof`, `ui.poll_frontend_flow_proof`, and `ui.stop_frontend_flow_proof` only after the manifest path proves repeated value.

## 7. Verification

Implementation PRs for this spec must report exactly which checks ran. The minimum verification set is:

```powershell
git diff --check
python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check
```

Editor/automation verification is required for action implementation slices:

- Registration/schema/result-shape automation for every new action or new workflow parameter.
- Generated-WBP visual artifact verification automation that does not depend on project sample assets.
- Headless/null-RHI unavailable-path automation for visual/runtime proof.
- Limited per-edit round-trip automation for spec-representable changes, plus broader slot-family round-trip automation outside the per-edit proof.
- Analyzer fixture tests for UI schema confusion and workflow-proof log classification.
- UBT build or explicit blocker text when the local UE build tools/editor are unavailable.

Runtime proof may be marked passed only when the proof session reached a terminal success state after real PIE execution. A planned runtime proof, static frontend validation result, or screenshot manifest alone is not runtime proof.

## 8. Agent Usage Rule

When a Monolith-using agent edits user-facing UMG UI, it must not finish with only "compiled" or "asset saved." The final answer must be backed by workflow proof with:

1. compile proof,
2. round-trip proof when the edit is spec-authored or clearly spec-representable,
3. layout/accessibility proof,
4. visual artifact proof for each requested profile,
5. runtime interaction proof when the requested behavior includes focus, navigation, input, click, modal, stack, or screen-flow behavior.

If any required proof is unavailable, the agent must say which proof is blocked and include the exact failure code from Section 5.
