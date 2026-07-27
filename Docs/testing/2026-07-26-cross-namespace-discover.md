# Cross-Namespace Discover Verification

**Date:** 2026-07-26 (re-verified 2026-07-27 after scope trim)
**Scope:** Minimal no-namespace `discover` filtering and pagination
**Hosts:** Disposable UE 5.8 and UE 5.7 projects outside the plugin checkout

---

## 1. Purpose

Verify that no-namespace `discover` can return a bounded filtered action list
while preserving the existing namespace-specific behavior and default namespace
inventory. The implementation must reuse existing filter, pagination, and terse
description semantics without adding a ranking engine, a new tool, a new
advertised schema surface, proxy dispatch behavior, or a second schema model.

---

## 2. Verification Results

All rows below were produced from the current head, after the scope trim that
removed the proxy-seed synchronization and the five response-shaping schema
properties.

The current head adds the `matched_namespaces` rollup described in §3.11–3.13.
All rows below were re-run at that head.

| Gate | Result | Evidence |
|---|---|---|
| `git diff --check` | PASS | No whitespace errors |
| UE 5.8 editor build | PASS | `D:\P4\MonolithPR112ProtectedHost` (UE 5.8): recompiled `MonolithCoreTools.cpp` + `MonolithDiscoverTerseTest.cpp`, relinked `UnrealEditor-MonolithCore.dll`, 5/5 actions, `Result: Succeeded` |
| UE 5.8 `Monolith.Discover` automation | PASS, 9/9 | `Saved\Automation\PR112TrimVerify\index.json`: `succeeded=9 failed=0 notRun=0`, process exit 0 |
| UE 5.7 editor build | PASS | `D:\P4\MonolithPR112FinalUE57Host` (UE 5.7): same two translation units recompiled, `UnrealEditor-MonolithCore.dll` relinked, 5/5 actions, `Result: Succeeded` |
| UE 5.7 `Monolith.Discover` automation | PASS, 9/9 | `Saved\Automation\PR112TrimVerifyUE57\index.json` under `-RenderOffscreen`: `succeeded=9 failed=0 notRun=0`, process exit 0 |
| Proxy files untouched | PASS | `Scripts/monolith_proxy.py` and `Tools/MonolithProxy/monolith_proxy.cpp` are byte-identical to `tumourlove/master` |
| Screenshot / Discord upload | N/A | Tool-discovery response shaping has no visual or asset-presentation change |

---

## 3. Covered Contracts

The `Monolith.Discover` test prefix verifies:

1. default no-namespace calls still return the namespace inventory;
2. a non-empty `filter` opts into the flat cross-namespace action view;
3. registry order remains deterministic;
4. the existing substring predicate is reused;
5. `offset` and `limit` paginate the filtered list;
6. terse descriptions are bounded by the shared one-line helper;
7. detailed parameter output remains opt-in;
8. namespace-specific filtering and pagination remain unchanged;
9. unknown namespaces still fail through the existing contract;
10. `limit` without `filter` and whitespace-only `filter` values preserve the
    namespace inventory instead of exposing the global action catalog;
11. the registered `discover` schema still advertises exactly the reused
    `filter` / `offset` / `limit` parameters, so the advertised contract cannot
    drift from the handler;
12. a filtered cross-namespace call returns `matched_namespaces`, every row is a
    distinct namespace with a positive `match_count`, and those counts sum to the
    pre-pagination `total` even when `limit=1` — so the namespace-selection view
    describes the whole filtered set, not the page;
13. the no-argument namespace inventory does not carry `matched_namespaces`.

Both the UE 5.8 and UE 5.7 reports contain nine succeeded tests and zero failed
tests.

---

## 4. Deliberate Exclusions

**Editor-offline proxy seeds are not touched.** `Scripts/monolith_proxy.py` and
`Tools/MonolithProxy/monolith_proxy.cpp` seed a `monolith_discover` descriptor
for use when the Editor is unreachable. That seed already lags the live
registration on `master` — it predates `filter`, `offset`, `limit`, `detail`,
and `verbose` — so a cold-start schema-driven client cannot construct the
cross-namespace call until the first live `tools/list`. This branch neither
introduces nor widens that drift, and refreshing the seeds (plus the stale
`tools/list` cache path behind it) is a separate change against a separate
surface.

**No new advertised schema surface.** The universal `_fields` / `_omit` /
`_row_fields` / `_path_fields` / `_compact_json` response-shaping parameters
continue to be accepted by `FMonolithParamSchema` without being advertised on
`discover`, matching every other registered action. `matched_namespaces` is a
response field, not a request parameter, so it adds no input surface.

**No ranking, scoring, or alias data.** `match_count` is the size of the filtered
set within a namespace. Namespaces are emitted in registry order and are never
sorted by it, there is no distance function, weight, tier, or curated word list
anywhere in the change, and the whole branch touches one source file plus the
shared `MonolithToolText` helper.
