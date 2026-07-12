# Monolith Source CRG Native-Reference Prune

| Metadata | Value |
|----------|-------|
| Date | 2026-07-12 |
| Scope | Keep incremental project-source pruning and derived CRG edge parity identical |
| Module | `MonolithSource` |
| Changelist | `1138` |
| Status | Passed — build, focused automation, clean full rebuild, incremental project reindex, and final deep health verified |

---

## 1. Root Cause And Contract

After a successful full `source.repair_crg_cache`, the automatic project-only source reindex could delete a `references` row because its `file_id` belonged to the pruned project slice while preserving both endpoint symbols. The prune removed CRG edges only by pruned endpoint node ids, so that file-owned reference left a derived edge whose `native_id` no longer belonged to the current valid-reference set. A subsequent deep `source.health` repeatedly recommended the same full repair.

`FMonolithSourceDatabase::PruneIndexedFilesUnderRoots` now snapshots the complete doomed reference-id set before any source rows are deleted. It deletes derived reference edges by those exact native ids and adds both endpoints to the pending refresh set so surviving risk metrics are recomputed. There is no fallback or count suppression: source rows and derived rows converge through the same deletion set.

---

## 2. Verification Gates

| Gate | Required evidence | Result |
|------|-------------------|--------|
| Build | Resolver-derived `SpeedEditor Win64 Development` build | Passed — `Saved\BuildSpeedEditor-speed-gamefeature-split-final-crg-prune-20260712_retry2.log`, `Result: Succeeded` |
| Focused automation | `Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice` | Passed — `automation-20260712T105226Z-F4276077`, 1/1 passed, 0 warnings, 0 errors |
| Regression fixture | A project-file-owned reference between two retained engine symbols leaves neither a native source row nor derived CRG edge after prune | Passed — the fixture keeps both endpoint symbols, prunes the owning project file, verifies all `3` affected symbols, and finds neither the native reference nor its CRG edge |
| Clean bootstrap | `source.trigger_reindex`, followed by full source/CRG completion | Passed — `89,640` files, `1,325,692` symbols, `5,598,624` references, `0` errors; the DB reopened and the full CRG projection/cache completed |
| Live incremental reindex | `source.trigger_project_reindex`, followed by indexing completion | Passed — pruned `1,896` project files; reindexed `1,898` files and `14,882` symbols; scoped CRG refreshed `28,332` affected symbols; completion rebuilt the projection/cache; `0` errors |
| Source health | `status=ok`, valid-reference plus inheritance edge parity, no repair recommendation | Passed — symbols/nodes/metrics `1,325,688/1,325,688/1,325,688`; valid reference-plus-inheritance edges/CRG edges `90,833/90,833`; orphan edges `0`; warnings `0`; every maintenance flag `false` |

The pre-verification database had already been damaged by an earlier overlapping full-repair and background incremental-reindex attempt, so it was not reused as proof. The live verification reset and rebuilt `EngineSource.db` through `source.trigger_reindex`, then exercised the project-only prune path once on that clean database. Final `source.health(include_counts=true)` reports `status=ok`, deep-health reason `deep_health_clean`, and no FTS, CRG, override-edge, or reindex maintenance requirement.

---

## 3. Visual And Discord Evidence

This is an internal source-index/derived-cache consistency change with no runtime gameplay, UI, level, VFX, animation, material, or asset presentation. Screenshot verification and Discord upload are not applicable.
