# find_overrides / search_functions List-Projection Verification (PR D)

| Field | Value |
| --- | --- |
| Date | 2026-07-04 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Additive `offset`/`cursor`/`fields` list-projection contract on live `source.find_overrides`, offline `monolith_query.exe source find_overrides`, and live `blueprint.search_functions` |

---

## 1. Contract Summary

Both actions keep every legacy response field and add: `total`, `returned`, `next_cursor`
(next offset as a numeric string, present while more rows remain), `projection` echo
(`detail`, `offset`, `fields`), optional `offset` (integer) and `cursor` (string, from a
prior `next_cursor`, takes precedence, parsed by the shared
`FMonolithProjectionUtils::ReadCursorOffset`), and optional `fields` (array or
comma-separated) row projection with unknown-name warnings.

`find_overrides` semantics: `total` counts rows emitted by the current fetch window
(`offset + max_results`), not the full population; absence of `next_cursor` is the
completion signal; the fetch window truncation also emits a cursor so pages can chain
past a small first window; caps are 2000 rows live / 1000 rows offline with an explicit
warning at the cap. Live rows project over
`id,name,qualified_name,kind,file,path_status,line,depth`; offline rows over
`id,name,qualified_name,kind,file,path_status,line_start,line_end,signature,depth`.

`search_functions` semantics: `total` equals the full `matched_count` (session function
cache), `truncated` is now `matched_count > offset + returned` (identical to the legacy
meaning at `offset=0`), and `limits` reports `default_limit=50` / `max_limit=1000`.

## 2. Results

| Check | Result |
| --- | --- |
| `cmd /c Plugins\Monolith\Tools\MonolithQuery\build.bat` | Passed; rebuilt and copied `Binaries\monolith_query.exe`. |
| Offline page 1: `source find_overrides Tick --direction=both --max-results=5` | Passed: `total=5`, `returned=5`, `truncated=true` (fetch window), `next_cursor="5"`. |
| Offline page 2: `... --offset=5 --fields=name,file,line_start` | Passed: `returned=5`, `next_cursor="10"`, row keys exactly `[file, line_start, name]`. |
| Offline unknown field: `... --fields=name,bogus` | Passed: warning `Unknown fields ignored: bogus. Valid fields: id, name, qualified_name, kind, file, path_status, line_start, line_end, signature, depth`. |
| `Monolith.IndexGuard.Source.FindOverridesTraversesDepth` | Passed (legacy behavior unchanged). |
| `Monolith.IndexGuard.Source.FindOverridesHandlesDiamondDepth` | Passed (legacy behavior unchanged). |
| `Monolith.IndexGuard.Source.FindOverridesPagesAndProjects` | Passed: full-window totals, page1/page2 non-overlap via `next_cursor`, tail page without cursor, `fields=name,line` projection, unknown-field warning. |
| `Monolith.LimitGuard.Blueprint.SearchFunctionsClampsLimit` | Passed (legacy limit clamp + minimal/standard detail unchanged). |
| `Monolith.LimitGuard.Blueprint.SearchFunctionsPagesAndProjects` | Passed: `total`/`returned`/`next_cursor` chain, page non-overlap, `cursor="1"` equivalence with `offset=1`, fields projection + unknown-field warning, legacy `matched_count`/`returned_count`/`limits`/`projection` presence. |
| Primary `SpeedEditor Win64 Development` UBT build | Passed. The headless MCP editor (NullRHI, `Saved\HeadlessMcp`) was stopped before each link because it held `UnrealEditor-MonolithSource.dll`/`UnrealEditor-MonolithBlueprint.dll`; the watchdog relaunched it afterward. |
| Live MCP `source.find_overrides` / `blueprint.search_functions` paging | See §3 (run after the final rebuild through `http://localhost:9316/mcp`). |

## 3. Live MCP Verification

Recorded 2026-07-04 after the final UBT rebuild and watchdog relaunch (`/health` PID
`61088` at `http://localhost:9316/mcp`).

| Live call | Result |
| --- | --- |
| `source.find_overrides {symbol:Tick, direction:both, max_results:5, fields:[name,file,line]}` | Passed: `overrides[]` rows carry exactly `name`/`file`/`line`; `total=5`, `returned=5`, `next_cursor="5"` emitted from the fetch-window truncation; `projection` echoes `{detail:minimal, offset:0, max_results:5, fields:[name,file,line]}`. |
| `blueprint.search_functions {query:Get, limit:2, cursor:"1", fields:"function_name,class_name"}` | Passed: `cursor` honored (`projection.offset=1`), rows carry exactly `function_name`/`class_name`, `total=6192` (full matched count), `returned=2`, `next_cursor="3"`, `limits={default_limit:50, max_limit:1000}`, legacy `matched_count`/`returned_count`/`cache_size` intact. |

## 4. Notes

The ROI trigger was the 2026-07-04 remaining-high-ROI plan (`PR D — large-result
projection`), whose headline sizes were re-measured before implementation. A 46-day log
sweep (`Logs\20260520..20260704`) showed the `>=209KB` evidence was stale or synthetic:
the 276KB offline `find_overrides` rows predate the 2026-06-08 `detail_level=minimal`
default, the 259/194KB `search_functions` rows were `limit=1000000` limit-guard
automation fixtures, and all four §3.3 actions had zero live calls in the
20260629–20260704 window. The projection contract was still implemented as an additive
correctness/ergonomics closure, and the corrected evidence plus fresher large-result
candidates (`monolith.find`, `editor.list_automation_tests`,
`monolith.get_action_metadata_coverage`) were recorded in `Docs\TODO.md`.
