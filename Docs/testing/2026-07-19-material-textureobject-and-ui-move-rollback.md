# Material TextureObject And UI Move Rollback Verification

| Metadata | Value |
|----------|-------|
| Date | 2026-07-19 |
| Scope | Correct material texture-source validation and make cross-parent UMG moves rollback-safe |
| Modules | `MonolithMaterial`, `MonolithUI` |
| Changelist | CL `1182` |
| Status | Verified: protected editor build succeeded, both focused tests passed, and the targeted source-control audit is clean |

---

## 1. Root Causes And Contracts

`material.validate_material` previously treated every `UMaterialExpressionTextureBase` with an empty direct `Texture` field as broken. That is incorrect for `UMaterialExpressionTextureSample`: a connected `TextureObject` input is the sample's source and overrides the direct field. Validation now recognizes the connected sample as sourced while still validating the upstream texture expression independently, so an upstream parameter with no default texture remains a real error.

`ui.move_widget` previously removed a cross-parent child before proving that target insertion and requested sibling placement succeeded. A target-side failure could therefore leave the Widget Blueprint tree partially mutated. Cross-parent moves now run through one reusable transaction helper that validates the target result and, on failure, restores the source parent, original child index, and compatible slot state before returning a structured mutation error.

---

## 2. Verification Gates

| Gate | Required evidence | Result |
|------|-------------------|--------|
| Protected editor build | `P4_BUILD_CHANGELIST=1182` through `Build\BatchFiles\BuildGameEditorAndRun.bat` | **Pass** — UBT reported `Result: Succeeded`; the protected wrapper completed with exit code `0` in client `123369lee_speed_cl1172_submit` |
| Material focused automation | `Monolith.Material.Validation.TextureObjectOverride` | **Pass** — 1/1 passed in 0.118 s, errors `0`, warnings `0` (`automation-20260718T185812Z-C71A6FB7`) |
| UI rollback focused automation | `Monolith.UI.MoveWidget.RollsBackTargetAddFailure` | **Pass** — 1/1 passed in 0.117 s, errors `0`, warnings `0` (`automation-20260718T185840Z-F343B908`) |
| Source-control membership | Every implementation, test, spec, generated build output, and this record in CL `1182`; default changelist empty | **Pass** — 31 files in CL `1182`, default `0`, unchanged edits `0`, missing opened files `0`, and targeted reconcile preview `0` |

---

## 3. Protected Build Lifecycle Observation

The protected build compiled and linked the editor successfully. During its source-control cleanup, however, the wrapper could not open `Plugins\GameFeatures\SpeedBox\Binaries\Win64\UnrealEditor-SpeedBoxRuntime.dll` because another client held that `+l` file, yet the wrapper still returned exit code `0`. This is a fail-open lifecycle behavior in the build wrapper rather than a compiler failure. The external checkout's local copy was restored exactly to depot head, a targeted reconcile preview confirmed no unowned binary/source/doc changes, and the launch preflight then passed for 66 project DLLs and 16 manifests.

---

## 4. Visual And Discord Evidence

These changes affect editor-only validation semantics and failure rollback behavior. They do not alter runtime UI presentation, material rendering, gameplay, VFX, animation, or level content, so screenshot verification and Discord screenshot upload are not applicable.
