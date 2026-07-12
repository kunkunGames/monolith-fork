# Monolith Data Validation Progress-Modal Classification

| Metadata | Value |
|----------|-------|
| Date | 2026-07-12 |
| Scope | Prevent auto-dismiss validation progress windows from becoming per-asset warning results |
| Module | `MonolithEditor` |
| Changelist | CL `1138` |
| Status | Passed — build, focused automation, and fresh-editor Data Validation verified |

---

## 1. Root Cause And Contract

`editor.validate_changeset_assets` validated all 237 CL `1135` assets as valid in a fresh headless editor, but returned 171 warning rows and `ok=false`. Every warning was the same Monolith-owned modal diagnostic: `MODAL_OPEN ... Validating Assets | 0%`. Unreal's asset-load log gatherer attributed that warning to each subsequently loaded asset.

The modal watcher now uses only Unreal's authoritative `FModalWindowContext::bIsSlowTaskWindow` classification. A Slate subtree bounded to depth `12`, `256` visited widgets, and `4096` text characters supplies diagnostic text and progress-indicator evidence but never substitutes for missing engine data. Every bounded-traversal stop is explicit through the structured `truncated` field, and overlong text also ends with `...[truncated]`. An unset classification therefore fails closed to `MODAL_OPEN`, and a missing context-window identifier remains empty with explicit `context_valid=false` rather than borrowing an unrelated active window. This prevents a real confirmation/error modal that happens to contain a progress indicator from being downgraded. Confirmed slow-task windows emit `MODAL_PROGRESS` at log severity and retain timestamp/title/text diagnostics without entering Data Validation warning capture. Blocking or unclassified modals continue to emit the existing `MODAL_OPEN` warning and recovery context.

---

## 2. Verification Gates

| Gate | Required evidence | Result |
|------|-------------------|--------|
| Build | Resolver-derived `SpeedEditor Win64 Development` build | Passed — `Saved\BuildSpeedEditor-speed-gamefeature-split-final-crg-prune-20260712_retry2.log`, `Result: Succeeded` |
| Focused automation | `Monolith.Editor.ModalDiagnostics.ProgressClassification` | Passed — `automation-20260712T101941Z-C32DE0A4`, 1/1 passed, 0 warnings, 0 errors |
| Fresh-editor Data Validation | CL `1135`, `limit=5000`, `capture_logs=true`, `silent=true` | Passed — `ok=true`, requested/checked/valid `281/281/281`, `asset_limit=false` |
| Asset result | All resolved assets valid; no synthetic modal warnings | Passed — invalid `0`, skipped `0`, unable `0`, warnings `0` |
| Blocking-modal contract | Engine `false` overrides any progress widget; engine `true` identifies a slow task; unset context/classification data is explicit and fails closed to `MODAL_OPEN` | Passed — focused automation covers authoritative `true`, authoritative `false`, unset fail-closed, bounded traversal, and explicit text truncation |

---

## 3. Visual And Discord Evidence

This is editor diagnostics/result-classification behavior with no runtime, gameplay, UI presentation, VFX, material, animation, or level output. Screenshot verification and Discord screenshot upload are not applicable.
