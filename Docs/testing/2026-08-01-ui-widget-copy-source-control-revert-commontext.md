# Monolith UMG Authoring Support Verification

**Date:** 2026-08-01

**Scope:** Cross-class and same-Widget-Blueprint subtree copy, source-control revert safety, and Foundation CommonTextBlock property authoring

**Result:** Protected build and all focused regressions passed

---

## 1. Root Causes and Corrections

| Area | Confirmed Root Cause | Correction |
| --- | --- | --- |
| Destination ownership | `ui.copy_widget_subtree_with_class_remap` duplicated source-owned widget and slot references before the complete source-to-destination map existed, allowing source mutation and invalid destination ownership. | Preconstruct every destination widget and slot, seed the complete replacement map, duplicate state into destination-owned objects, and reassert ownership before assembling the destination hierarchy. |
| Cross-class remap crash | `StaticDuplicateObjectEx` requires compatible object memory layouts. Replacing `CommonRichTextBlock` with `CommonTextBlock` violated that contract and asserted because their class sizes differ. | Use seeded `StaticDuplicateObjectEx` only for the same class. Use `UEngine::CopyPropertiesForUnrelatedObjects` for different classes so compatible reflected properties are copied by contract rather than raw object layout. |
| Same-WBP slot loss | Collision deletion detached the source widget before the live tree traversal read its parent, child index, and slot properties. | Snapshot the source attachment graph before collision handling and rebuild from that snapshot, preserving parent, order, padding, alignment, size rule, and fill weight. |
| P4 revert behavior | Synchronous revert inherited Unreal's global `bPromptForCheckoutOnAssetModification`-adjacent source-control delete preference and could invoke `p4 revert -w`, which performed unintended workspace reconciliation and hung on generated assets. | Add the tolerant boolean `delete_new_files`, default it to `false`, and guard/restore the global preference for the duration of the action. Callers must explicitly opt into deleting workspace files. |
| Foundation text authoring | The safe UI property allowlist did not expose inherited `UTextBlock` mappings when the target was `UCommonTextBlock`, preventing safe `Text`, wrapping, justification, and related presentation edits. | Register inherited TextBlock mappings plus the supported CommonUI style and slot mappings for `CommonTextBlock`, while retaining the allowlist boundary. |

---

## 2. Regression Coverage

| Automation | Result | Contract Proved |
| --- | --- | --- |
| `Monolith.Registry.UI.CopyWidgetSubtreeWithClassRemapSchema` | `1/1` pass | The live catalog exposes the corrected remap action schema. |
| `Monolith.ParamGuard.UI.CopyWidgetSubtree` | `2/2` pass | Invalid or missing copy inputs fail at the parameter boundary. |
| `Monolith.UI.WidgetSubtreeCopy` | `1/1` pass | Source remains unchanged, destination objects are destination-owned, unrelated-class text copy succeeds, and same-WBP vertical-slot padding/alignment/fill survive replacement. |
| `MonolithUI.Allowlist.CommonTextBlockInheritedTextMappings` | `1/1` pass | Safe inherited TextBlock properties resolve for Foundation CommonTextBlock targets. |
| `Monolith.ParamValidation.MonolithSourceControl` | `2/2` pass | `delete_new_files` accepts tolerant booleans and defaults safely to `false`. |

Focused Monolith result: `7/7` passed with zero errors and zero warnings. The combined Replay Browser and supporting-tool run was `18/18`.

---

## 3. Build

The final verification used the protected project entry point:

```powershell
$env:P4_BUILD_CHANGELIST = "1407"
$env:SKIP_EDITOR_LAUNCH = "1"
& "Build\BatchFiles\BuildGameEditorAndRun.bat"
```

Result: `Succeeded` with exit code `0` after all copy, slot-preservation, source-control, allowlist, test, spec, and skill changes were present in the build inputs.

---

## 4. Production Use Evidence

The corrected actions authored `/SpeedCore/UI/Menu/WBP_QAReplayBrowser` and `/SpeedCore/UI/Menu/WBP_QAReplayEntry` from the local Speed Foundation widgets without modifying `/Game/UI/Settings/W_LyraSettingScreen` or `/Game/UI/Settings/Editors/W_SettingsListEntry_Action`. Both source reference assets were reverted and confirmed absent from the task changelist.

---

## 5. Screenshot Scope

No separate Monolith-tool screenshot or Discord upload is required for this editor/tooling regression record. Player-facing visual evidence for the UMG assets produced through these actions is recorded in `Docs\testing\2026-08-01-qa-replay-browser-umg-foundation.md` and `Saved\Screenshots\20260801\WBP_QAReplayBrowser_CL1407_PC1080p.png`.
