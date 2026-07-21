# UI Visual Diff Scanline and Work-Budget Verification

**Date:** 2026-07-19
**Status:** Source, focused automation, API, and spec implemented; protected build and focused execution pending
**Changelist:** 1200

---

## 1. Scope

Harden `ui.verify_widget_visual_artifacts` after exclusion rectangles were added to capture and region comparisons. The verifier must retain exact exclusion semantics while preventing a maximum `128 regions * 32 exclusions` request from degrading into a pixel-by-rectangle scan. This changelist changes Monolith UI source, focused automation, public API/spec text, and this verification record only; it does not change Widget Blueprints, runtime UI assets, or gameplay presentation.

---

## 2. Implementation contract

| Gate | Result | Evidence |
|------|--------|----------|
| Shared membership algorithm | IMPLEMENTED | Global metrics, named-region metrics, and heatmap generation call one row-delta scanline visitor. Membership work is `O(height * (width + rectangle_count))`; no pixel performs a linear scan of every rectangle. |
| Exclusion ownership | IMPLEMENTED | Capture exclusions affect the global metric and appear as deterministic dim-blue heatmap pixels. Region exclusions affect only the owning named-region metric; they do not remove pixels from the global metric or heatmap. |
| Action-wide preflight | IMPLEMENTED | Every baseline comparison reserves global, region, and heatmap scanline work against one cumulative action budget of `134,217,728` units (`2 * MaxVisualArtifactPixels`). The model is `height * (width + exclusion_count)` for each scanline pass. |
| Overflow safety | IMPLEMENTED | Reservation compares `units_per_multiplier > remaining / multiplier` before multiplication. Requested-unit evidence uses a second `MAX_int64` division guard before forming the exact product. |
| Fail-closed evidence | IMPLEMENTED | An over-budget comparison returns `visual_diff_work_budget_exceeded` before metric or heatmap work and reports `work_units_requested`, `work_units_reserved`, `work_units_limit`, and `work_unit_model`. Successful diffs and the top-level manifest report the same reservation model. |
| Existing fully-excluded gates | PRESERVED | Capture-wide and region-wide masks continue to fail with `comparison_fully_excluded` and `region_fully_excluded`; no zero-denominator comparison auto-passes. |

---

## 3. Focused automation coverage

`Monolith.UI.VisualArtifacts.VerifierContract` now includes these regression cases:

| Case | Expected evidence |
|------|-------------------|
| Capture-only exclusion heatmap ownership | With no region mask present, the changed pixel is absent from the global metric and decodes from the heatmap as BGRA `[96,32,0,255]`. |
| Region-only exclusion ownership | The named region passes locally, the global changed-pixel count remains `1`, and the same heatmap pixel remains red instead of dim blue. |
| Multi-capture accumulation | Two identical `2x2` comparisons report cumulative requested work of `8` and `16` units, and the action reports `16` reserved units. |
| Maximum-cardinality bounded success | A `64x64` identical comparison with `128 * 32 = 4096` region exclusions passes, emits all 128 region rows and a heatmap, and reports exactly `794,624` requested units. |
| Work-budget failure | A `32x16384` identical fixture with 128 regions and 32 exclusions per region crosses the cap during preflight, returns `visual_diff_work_budget_exceeded`, reports requested units above `134,217,728`, reserves `0` action units, exposes no computed metrics, and writes no heatmap. |

The focused automation source is implemented but has not been compiled or executed in this subtask. The integration owner must run the protected build with `P4_BUILD_CHANGELIST=1200`, then execute the exact test `Monolith.UI.VisualArtifacts.VerifierContract` through the project's editor automation path.

---

## 4. Pending verification

| Gate | Result | Evidence / required action |
|------|--------|----------------------------|
| Protected editor build | PENDING | Run `Build\BatchFiles\BuildGameEditorAndRun.bat` with `P4_BUILD_CHANGELIST=1200` and `SKIP_EDITOR_LAUNCH=1`. |
| Focused automation | PENDING | Execute `Monolith.UI.VisualArtifacts.VerifierContract`; require zero errors and every ownership/work-budget assertion above. |
| Public contract review | STATIC PASS | `Plugins\Monolith\Docs\API_REFERENCE.md` and `Plugins\Monolith\Docs\specs\SPEC_MonolithUI.md` describe the scanline model, exclusion ownership boundary, cumulative cap, fields, and failure code. |
| Screenshot / Discord | N/A | This is source/schema/test/documentation hardening only. No Widget Blueprint, runtime UI, visual asset, gameplay, or presentation state changed, so PC 1920x1080 screenshot proof and Discord upload are not applicable. |
