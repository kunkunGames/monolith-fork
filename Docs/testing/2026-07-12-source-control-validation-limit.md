# Monolith Source-Control Validation Limit Alignment

| Metadata | Value |
|----------|-------|
| Date | 2026-07-12 |
| Scope | Align `source_control.list_opened` and depot-path batching with the existing editor changeset-validation limit |
| Module | `MonolithSourceControl` |
| Changelist | CL `1138` |
| Status | Passed — build, focused automation, and live 5,000-limit validation verified |

---

## 1. Root Cause And Contract

`editor.plan_content_validation_changeset` and `editor.validate_changeset_assets` publicly accept `limit` through 5,000, but their required `source_control.list_opened` dependency previously rejected every value above 2,000. A live CL `1135` validation call with `limit=5000` therefore failed before target planning even though the editor action schema declared the request valid.

The source-control boundary now shares the existing 5,000-row editor contract. `list_opened` remains bounded by `p4 -ztag opened -m (limit + 1)` and therefore observes at most 5,001 tagged records including one sentinel. Depot/client mapping accepts at most 5,000 raw and unique paths while preserving the existing per-command limits of 128 paths and 24,000 encoded characters; only the aggregate command cap expands from 16 to 40 so a full 5,000-path request remains bounded.

---

## 2. Verification Gates

| Gate | Required evidence | Result |
|------|-------------------|--------|
| Focused build | Resolver-derived UBT build including `MonolithSourceControl` | Passed — `Saved\BuildSpeedEditor-speed-gamefeature-split-final-crg-prune-20260712_retry2.log`, `Result: Succeeded` |
| Opened bounds automation | Maximum 5,000 builds `-m 5001`; 5,001 is rejected | Passed — `Monolith.SourceControl.P4WhereBatch`, `automation-20260712T101958Z-9EFCCAE7`, 6/6 passed |
| Batch scale automation | 5,000 unique paths resolve in at most 40 batches of at most 128 paths | Passed — `P4WhereBatch.Scale` passed in `automation-20260712T101958Z-9EFCCAE7` |
| Param guards | `list_opened(limit=5001)` and 5,001 raw map paths fail before P4 execution | Passed — `Monolith.ParamValidation.MonolithSourceControl`, `automation-20260712T102001Z-F17B664A`, 2/2 passed |
| Live changeset validation | `editor.validate_changeset_assets(changelist="1135", limit=5000)` reaches Data Validation instead of dependency rejection | Passed — opened `562` exact rows and Data Validation completed with requested/checked/valid `281/281/281`, invalid/skipped/unable/warnings `0` |

The final live `source_control.list_opened(changelist="1135", limit=5000)` result reported `count=observed_count=returned_count=562`, `count_semantics="exact"`, `backend_record_limit=5001`, `has_more=false`, and `truncated=false`. Mapping reported raw/requested/unique/resolved `562/562/562/562`, failed `0`, in `5` bounded commands.

---

## 3. Visual And Discord Evidence

This change affects read-only Perforce query bounds and JSON contracts only. It has no runtime, UI, VFX, material, animation, level, or asset-presentation output, so screenshot verification and Discord screenshot upload are not applicable.
