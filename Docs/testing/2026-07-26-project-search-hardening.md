# Project Search Hardening and FTS Repair Verification

**Date:** 2026-07-26 (review-fix reruns: 2026-07-27)
**Scope:** Existing `fts_assets` / `fts_nodes` search contract, recursive FTS5 projection, provenance, bounded output, failure classification, and focused `repair_fts`
**Hosts:** Disposable UE 5.8 and UE 5.7 projects outside the plugin checkout

---

## 1. Purpose

Verify the maintainer-requested high-ROI slice without introducing a new index schema:

- preserve the existing asset and graph-node FTS tables;
- return deterministic match provenance and bounded text;
- derive each context preview from the field reported as the match;
- fail closed on malformed queries or SQLite failures;
- preserve compatible branches in top-level and nested asset/node-spanning boolean expressions;
- recognize bare, quoted, grouped, negative, and mixed-table column filters;
- preserve non-ASCII FTS5 barewords at decoded Unicode boundaries;
- distinguish invalid caller input from index/storage failures at the JSON-RPC boundary;
- support dry-run-first repair of only the affected FTS table;
- keep the native executable and Python offline query paths behaviorally aligned;
- compile on both Unreal Engine 5.8 and 5.7.

---

## 2. Verification Results

| Gate | Result | Evidence |
|---|---|---|
| Python syntax | PASS | `python -m py_compile Scripts\monolith_offline.py Scripts\check_offline_exe_fresh.py` |
| Native offline query build | PASS | `Tools\MonolithQuery\build.bat` from the Visual Studio x64 developer environment |
| Native executable freshness | PASS | `python Scripts\check_offline_exe_fresh.py`, ordered source-manifest hash `db8c4142cf4471cd` covers `monolith_query.cpp` and `ProjectSearchQueryProjectionCore.h` |
| Native/Python fixture parity | PASS, 25/25 | Deep-equal result payloads plus explicit asset-name/node-class matched-context assertions; also covers quoted/grouped/mixed filters, nested `AND`/`OR`, repeated `NOT`, precedence, long flat boolean chains, `éORé`, Unicode whitespace, anchors, valid zero results, malformed structure, and unknown columns |
| SQLite FTS5 grammar acceptance | PASS, 45/45 | Shared projector output prepared successfully against SQLite for the supported phrase, prefix, anchor, `NEAR(...)`, column-filter, grouping, and boolean forms |
| Generated semantic differential | PASS, 250/250 | Projected table-specific queries preserved both result membership and ranking against equivalent per-table reference queries |
| Static policy differential | PASS | Newer Speed static policy reported 36 blockers on both clean upstream and this branch; this branch introduced zero blockers |
| UE 5.8 editor build | PASS | Protected `ActivationHostEditor Win64 Development` build in `D:\P4\MonolithPR113ReviewUE58Host`; exact review source recompiled/linked `UnrealEditor-MonolithIndex.dll`, exit 0 |
| UE 5.8 focused automation | PASS, 1/1 | Review-fix rerun under `-RenderOffscreen`; report: `D:\P4\MonolithPR113ReviewUE58Host\Saved\Automation\PR113NearDistanceFinalUE58\index.json`; log: `D:\P4\MonolithPR113ReviewUE58Host\Saved\Logs\PR113NearDistanceFinalUE58.log`; one succeeded test, zero test warnings/errors, process exit 0 |
| UE 5.7 editor build | PASS | Detached exact production/test source snapshot; protected build recompiled/linked the affected `MonolithIndex` sources and `UnrealEditor-MonolithIndex.dll`, exit 0; isolated UBT log: `D:\P4\MonolithPR113FinalUE57Host\Saved\Logs\PR113NearDistanceBuildUE57-UBT.log` |
| UE 5.7 focused automation | PASS, 1/1 | `-RenderOffscreen`; report: `D:\P4\MonolithPR113FinalUE57Host\Saved\Automation\PR113NearDistanceFinalUE57\index.json`; log: `D:\P4\MonolithPR113FinalUE57Host\Saved\Logs\PR113NearDistanceFinalUE57.log`; one succeeded test, zero test warnings/errors, process exit 0 |
| Screenshot / Discord upload | N/A | Search/index infrastructure has no visual or asset-presentation change |

---

## 3. Focused Automation Contract

`Monolith.Index.ProjectSearch.HardeningAndRepair` creates an isolated temporary SQLite database and verifies:

1. asset and graph-node searches expose the existing table/field/object-path provenance;
2. asset-name and node-class hits derive context from those matched fields rather
   than unrelated non-empty description or node-name columns;
3. returned context and matched values are bounded by Unicode code points;
4. graph-node field qualifiers route to the node FTS table;
5. bare, quoted, grouped, and mixed-table column specifications route only to
   compatible columns;
6. `asset_name:... OR node_name:...` preserves both compatible branches, and
   an unqualified alternative remains searchable on both tables;
7. `Common AND (asset_name:... OR node_name:...)` recursively preserves the
   compatible nested branch for each table;
8. decoded UTF-8 boundaries in `éORé` do not split the embedded `OR`, and
   non-ASCII whitespace such as NBSP remains part of an FTS5 bareword instead
   of being normalized as ASCII syntax whitespace;
9. compatible boolean chains are rendered with only precedence-required
   parentheses, so an 81-branch cross-table disjunction executes without
   artificial nesting growth;
10. a syntactically valid cross-table conjunction that no single table can
   satisfy succeeds with zero results and no error;
11. malformed syntax and unknown columns—including an unknown qualifier behind
   an otherwise inapplicable conjunction and a non-numeric `NEAR` distance—return
   `InvalidQuery` with no partial result set;
12. a deliberately missing FTS table returns `InternalError`, with the expected
   SQLite diagnostic accounted for and no partial results;
13. `repair_fts` defaults to dry-run and reports the selected table;
14. explicit execution repairs only the selected FTS table.

The final UE 5.8 and UE 5.7 reports each contain one succeeded test, zero
warnings, and zero errors.

The current upstream revision predates the repository's newer hosted static
checker and configuration. The accepted check therefore applies the exact same
newer checker/configuration to clean upstream and this branch, with only the
incompatible binary-freshness execution gate disabled in memory, then compares
full finding identities. The 2026-07-27 maintenance rerun reports the same 36
blockers and 802 Windows-checkout advisories on both sides; this branch
introduces and resolves zero findings.

---

## 4. Cross-Version Runtime Verification

The accepted UE 5.8 and UE 5.7 runs both use `-RenderOffscreen`, retaining
unattended execution while providing the rendering surface Slate requires.
Each controller found exactly one matching test, completed it successfully with
zero test warnings/errors, exported a JSON report, and exited with status 0:

```text
Found 1 automation tests based on 'Monolith.Index.ProjectSearch.HardeningAndRepair'
Test Completed. Result={Success} Name={HardeningAndRepair}
...Automation Test Queue Empty 1 tests performed.
FPlatformMisc::RequestExitWithStatus(1, 0, FEngineLoop::Tick.GScopedTestExit)
```

Both runs use the same `-ReportExportPath` contract and exercise newly linked
binaries from the exact reviewed source tree; neither relies on a stale editor
module. The UE 5.8 host logged a
pre-test bind failure for port 9316 because an existing local MCP listener
already owned that unrelated service port. The search automation does not use
the HTTP listener, subsequently ran to completion, and reported no test event
errors; the startup diagnostic is recorded here rather than silently omitted.

---

## 5. Build Isolation Note

UE-generated plugin `Binaries` and `Intermediate` outputs are engine-version
specific. The UE 5.8 host uses the production review worktree, while the UE 5.7
host uses a detached clean checkout of the exact same commit. Each host owns
its own generated project intermediates and links against its matching engine.
No source fallback, alternate plugin checkout, or cross-version binary reuse is
part of the accepted evidence.
