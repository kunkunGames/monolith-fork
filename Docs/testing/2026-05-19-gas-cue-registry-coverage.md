# GAS GameplayCue Registry Coverage Verification

**Date:** 2026-05-19
**Branch:** `codex/gas-cue-registry-clean`
**Scope:** `gas.validate_cue_coverage`
**Engine:** Unreal Engine 5.7 resolved from `D:\P4\game\GO.uproject`
**Result:** PASS

---

## 1. Summary

`gas.validate_cue_coverage` now accepts `include_registered_tags_without_notifies=false|true`. When enabled, it walks the registered `GameplayCue` tag subtree through `UGameplayTagsManager` and reports sorted `registered_tags_without_notifies` entries for cue tags that have no matching GameplayCue Notify handler. If `path_filter` is present, existing missing/orphaned scans remain path-limited while the registered-tag audit compares against global project Notify handlers to avoid false positives.

The implementation is read-only. It reuses the existing GameplayEffect and GameplayCue Notify scan, compares against registered tag names, and does not create tags, create assets, compile Blueprints, save packages, call transactions, or mark packages dirty.

---

## 2. Verification Results

| Gate | Command | Result |
|------|---------|--------|
| Diff hygiene | `git diff --check origin/master...HEAD` | PASS |
| Static CI | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS: 0 blocking findings, 1 pre-existing `.claude/agents` advisory |
| UE 5.7 module build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Plugin="D:\P4\monolith-prs\gas-cue-registry-clean\Monolith.uplugin" -Module=MonolithGAS -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` | PASS |
| UE 5.7 release guard | `MONOLITH_RELEASE_BUILD=1` with the same UBT module build | PASS |

---

## 3. Deferred Checks

| Item | Reason |
|------|--------|
| Functional editor fixture | No local GAS project fixture with intentionally registered-but-unimplemented `GameplayCue.*` tags was exercised in this pass. Source-level parity with UE 5.8 `GASToolsets::FindCueTagsWithoutNotifies`, static CI, and UE 5.7 compile coverage verify the API contract and dependency compatibility. |
