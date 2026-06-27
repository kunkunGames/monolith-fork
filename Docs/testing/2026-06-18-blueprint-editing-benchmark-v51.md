# AssetEditing Benchmark v5.1 — Adversarial Hardening + Interface-Resolver Fix

| Field | Value |
|-------|-------|
| Date | 2026-06-18 |
| Author | Claude (Opus 4.8) |
| Changelist | 789 |
| Engine | `++UE5+Release-5.7-CL-51494982` (UE 5.7) |
| MCP | `0.20.2`, MonolithBlueprint DLL rebuilt |
| Project | `GO` (`D:\P4\game`) |

---

## 1. Scope

Adversarially audit the `Benchmarks/AssetEditing` benchmark + test set, maximize its practical
and anti-gaming value, run it against the live editor, and reflect the results into Monolith.

## 2. Method

1. **Adversarial audit** — a 5-dimension fan-out (scoring-engine gaming vectors, practicality gaps,
   re-run idempotency, catalog/param correctness, weighting) with an adversarial verify pass:
   35/36 findings confirmed against the live runner + handler source.
2. **Empirical baseline** — `setup_fixtures` + a 295-task run against the live editor
   (`baseline-v5-pre` = **0.967**). All 9 failures were benchmark/contract defects, not server gaps.
3. **Implementation** — scoring-engine hardening + practical-coverage expansion + reweight in
   `Scripts/asset_editing_benchmark.py`; the `ResolveInterfaceClass` handler fix in
   `Source/MonolithBlueprint`. 305 tasks, 10 categories.
4. **Build + live validation** — the project editor target rebuilt (UBT); the interface resolver and the
   `negative_compile` break mechanism validated with direct MCP calls; full scored run.

## 3. Results

| Run | Tasks | Score | Notes |
|-----|------:|------:|-------|
| `baseline-v5-pre` | 295 | 0.967 | pre-hardening; 9 benchmark-defect failures |
| `v5.1` | 305 | **1.000** | hardened suite, 0 transport errors, **all 10 dimensions 1.000** |

`compare/comparison.md`: score +0.033; `edit_execute` +0.058, `variable_read` +0.071,
`workflow_execute` +0.091. The score rose although the suite got strictly harder, because the
benchmark/contract defects that depressed the baseline were fixed.

### Anti-gaming validated
- No-edit stub ceiling recomputed ≈0.22 (down from v5 ≈0.31): a stub scores 0 on `edit_execute`
  (delete-first read-back), `workflow_execute`, `error_path` (identifier-specific), `duplicate_reject`,
  and `negative_compile`.
- `negative_compile` break mechanism verified live: `set_variable_type` to a non-existent struct →
  `compile_blueprint` `error_count=2, status=Error, "invalid type Structure"`. The audit's proposed
  dangling-VariableGet and duplicate-event breaks were verified **silently tolerated** by UE
  (`error_count=0`) and rejected.
- Interface-resolver fix verified live: both `BPI_TestInterface` (short name) and the full
  `/Game/Benchmarks/BPI_TestInterface` path resolve `get_interface_functions` to 7 functions
  (previously the short name returned "Interface class not found").

## 4. Reflected into Monolith

| Change | File(s) |
|--------|---------|
| `ResolveInterfaceClass` — resolve Blueprint Interface assets by path/short name for `implement_interface`, `get_interface_functions`, `remove_interface` | `Source/MonolithBlueprint/Private/{MonolithBlueprintInternal.h, MonolithBlueprintGraphActions.cpp, MonolithBlueprintActions.cpp}` |
| **`duplicate_component` guard** — reject an explicit `new_name` collision instead of silently suffixing (`Y`→`Y_1`); auto-`_Copy` default still suffixes (found + verified live) | `Source/MonolithBlueprint/Private/MonolithBlueprintComponentActions.cpp` |
| **`remove_event_dispatcher` / `set_event_dispatcher_params` `name` alias** — `dispatcher_name` made `Optional` + `name` registered as an alias (the schema-validation layer rejected the call before the handler's fallback ran; registration change required) | `Source/MonolithBlueprint/Private/MonolithBlueprintGraphActions.cpp` |
| Benchmark v5.1 (hardening, expansion, reweight, defect fixes) | `Scripts/asset_editing_benchmark.py`, `Benchmarks/AssetEditing/*` |
| Module spec sync | `Docs/specs/SPEC_MonolithBlueprint.md` |
| Deferred items + corrected claims | `Docs/TODO.md` |

### Self-audit correction (post-run)

A re-verification of the "reflect into Monolith" deliverables (prompted by "did this reflect well?")
found and corrected two issues, applying the verify-don't-assume rule to my own work:
1. The TODO had claimed `add_variable` silently no-ops on an inherited-native-name shadow (the
   alleged root cause of the `bIsActive` fixture problem). A live test **disproved** it —
   `add_variable bIsActive` on `BC_TestComponent` creates the variable successfully. There is no
   shadow-collision bug; the claim was an unverified assumption and was corrected. The
   `bComponentActive` rename stands as a harmless precaution.
2. Two genuinely real bugs that had been *deferred* to TODO were instead fixed at the root and
   verified live (the `duplicate_component` silent-suffix and the event-dispatcher param
   inconsistency, above) — rather than masked at the benchmark level.

## 5. Blocked / process notes

- The headless `-nullrhi` editor crashes intermittently under heavy mutation (known engine bug). The
  scored run is gated on **zero transport errors** — a mid-run crash is rejected and retried on a
  fresh boot. `setup_fixtures` now `save_asset`s each fixture so new entities survive a reboot.
- `create_blueprint` auto-`p4 add`s and persists assets immediately; a corrupt scratch asset (e.g. a
  variable with an invalid struct type) destabilizes the next boot's asset scan, so `negative_compile`
  self-cleans (repairs the break + saves) and break-test leftovers were reverted.
- Relinking a Monolith module DLL requires `p4 edit` on the tracked `binary+l`
  `Binaries/Win64/UnrealEditor-<Module>.dll` first, else `LNK1104 Access denied`.

## 6. Verification gate

`python Scripts/asset_editing_benchmark.py run --label v5.1 --skip-preflight` after a fresh boot +
`setup_fixtures` → 305 tasks, score 1.000, 0 transport errors, 0 failures. Re-run on a fresh editor
boot (or after fixture reset) for an idempotent number.
