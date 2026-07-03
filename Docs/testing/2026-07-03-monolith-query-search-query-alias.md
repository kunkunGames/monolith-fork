# Monolith Query Search Query-Alias Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-03 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Offline `monolith_query.exe` search argument compatibility for `project.search` and `source.search_source` |

---

## 1. Results

| Check | Result |
| --- | --- |
| `cmd /c Plugins\Monolith\Tools\MonolithQuery\build.bat` | Passed. Built source hash `206ab0997e64f154` and copied `monolith_query.exe` to `Plugins\Monolith\Binaries`. |
| `project search --query=Health --limit=1` | Passed, exit 0, returned one compact result. |
| `project search --query Health --limit=1` | Passed, exit 0, returned one compact result. |
| `project search --q=Health --limit=1` | Passed, exit 0, returned one compact result. |
| `project search --q Health --limit=1` | Passed, exit 0, returned one compact result. |
| `project search Health --limit=1` | Passed, exit 0, preserved positional behavior. |
| `project search --limit=1` | Passed as negative case, exit 1, error says `search requires a query argument (positional or --query=<text>)`. |
| `source search_source --query=UObject --limit=1` | Passed, exit 0, returned symbol and source-line matches. |
| `source search_source --query UObject --limit=1` | Passed, exit 0, returned symbol and source-line matches. |
| `source search_source --q=UObject --limit=1` | Passed, exit 0, returned symbol and source-line matches. |
| `source search_source --q UObject --limit=1` | Passed, exit 0, returned symbol and source-line matches. |
| `source search_source UObject --limit=1` | Passed, exit 0, preserved positional behavior. |
| `source search_source --limit=1` | Passed as negative case, exit 1, error says `search_source requires a query argument (positional or --query=<text>)`. |
| `Plugins\Monolith\Binaries\monolith_query.exe --catalog-self-check` | Passed, `success=true` and `errors={}`. |

## 2. Notes

The ROI trigger was `Saved\Monolith\LogAnalysis\roi-20260703`, which showed fresh schema-confusion regressions where agents naturally supplied search text through `--query`. The fix is additive: existing positional calls still resolve first, while option-based calls now work for both high-traffic offline search actions.
