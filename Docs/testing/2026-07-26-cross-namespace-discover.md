# Cross-Namespace Discover Verification

**Date:** 2026-07-26
**Scope:** Minimal no-namespace `discover` filtering and pagination
**Hosts:** Disposable UE 5.8 and UE 5.7 projects outside the plugin checkout

---

## 1. Purpose

Verify that no-namespace `discover` can return a bounded filtered action list
while preserving the existing namespace-specific behavior and default namespace
inventory. The implementation must reuse existing filter, pagination, and terse
description semantics without adding a ranking engine, a new tool, proxy
dispatch behavior, or a second schema model. Editor-offline proxy seeds must
advertise the same request parameters as the live action.

---

## 2. Verification Results

| Gate | Result | Evidence |
|---|---|---|
| `git diff --check` | PASS | No whitespace errors |
| Static policy differential | PASS | Newer Speed static policy reported 36 blockers on both clean upstream and this branch; this branch introduced zero blockers |
| UE 5.8 editor build | PASS | Isolated protected host completed the original 435/435 build; the final nested-shaping review sync recompiled `MonolithCoreTools.cpp`, relinked `UnrealEditor-MonolithCore.dll`, and completed all 4/4 affected actions |
| UE 5.8 `Monolith.Discover` automation | PASS, 9/9 | `D:\P4\MonolithPR112ProtectedHost\Saved\Logs\PR112ReviewFinal2.log` and `Saved\Automation\PR112ReviewFinal2\index.json` record nine succeeded tests, zero warnings/errors, and process exit 0 |
| UE 5.7 editor build | PASS | Clean isolated source worktree completed the original 429/429 build; the exact final commit recompiled the schema and test sources, relinked `UnrealEditor-MonolithCore.dll`, and completed all 5/5 affected actions |
| UE 5.7 `Monolith.Discover` automation | PASS, 9/9 | `D:\P4\MonolithPR112FinalUE57Host\Saved\Logs\PR112ReviewFinalUE57.log` and `Saved\Automation\PR112ReviewFinalUE57\index.json` record nine succeeded tests, zero warnings/errors, and process exit 0 |
| Python proxy seed | PASS | `python -m py_compile Scripts\monolith_proxy.py`; editor-offline `tools/list` exposes all seven live discovery parameters plus `_fields`, `_omit`, `_row_fields`, `_path_fields`, and `_compact_json` |
| Native proxy seed | PASS | `monolith_proxy.cpp` compiled with the installed Visual Studio x64 C++17 toolchain; editor-offline `tools/list` has deep description/schema equality with the Python seed, including nested response shaping |
| Legacy proxy cache upgrade | PASS | Both proxies overlaid current seed fields onto an old cached `monolith_discover`, preserving its live `annotations`/`title`, deduplicating stale entries, and keeping unrelated cached tools byte-for-byte |
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
11. both native and Python proxy seeds expose
    `namespace`, `category`, `detail`, `verbose`, `filter`, `offset`, and
    `limit` for cold-start schema parity;
12. live, Python, and native discovery schemas expose `_row_fields` and
    `_path_fields`, allowing schema-driven clients to project the flat
    `actions` list and nested response leaves;
13. an older cached `monolith_discover` entry receives the current seed's
    description and schema in both proxies while live `annotations`/`title`
    survive, duplicates collapse to one, and unrelated cached Editor tools
    remain unchanged.

The current UE 5.8 report contains nine succeeded tests and zero failed tests.
The assertions cover all twelve contracts above; several compatibility assertions
share the existing focused automation cases.

---

## 4. Static-Policy Compatibility Note

The current upstream revision predates the repository's newer hosted static
checker and configuration. A same-config comparison against clean upstream
found no branch-introduced blocker. Its only introduced advisory was a CRLF
observation on unchanged `Scripts/monolith_proxy.sh`, caused by the comparison
worktree's checkout representation rather than this diff.
