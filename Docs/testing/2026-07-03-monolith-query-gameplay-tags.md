# Monolith Query Gameplay Tag Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-03 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Offline `monolith_query.exe` parity for `project.list_gameplay_tags` and `project.search_gameplay_tags` |

---

## 1. Results

| Check | Result |
| --- | --- |
| `cmd /c Plugins\Monolith\Tools\MonolithQuery\build.bat` | Passed. Built source hash `02dd86871932f26f` and copied `monolith_query.exe` to `Plugins\Monolith\Binaries`. |
| `Plugins\Monolith\Binaries\monolith_query.exe --catalog-self-check --no-log` | Passed, `success=true` and `errors={}`. |
| `Plugins\Monolith\Binaries\monolith_query.exe project get_stats --no-log` | Passed, reported `tags=335` and `tag_references=0` in `ProjectIndex.db`. |
| `Plugins\Monolith\Binaries\monolith_query.exe project list_gameplay_tags --limit=3 --no-log` | Passed, exit 0, returned three `tags[]` rows and `next_cursor=3`. |
| `Plugins\Monolith\Binaries\monolith_query.exe project search_gameplay_tags Game --limit=3 --no-log` | Passed, exit 0, returned three matching `GamePhase*` tags and empty `referencing_assets[]` arrays, matching the current `tag_references=0` database state. |
| `Plugins\Monolith\Binaries\monolith_query.exe project list_gameplay_tags Ability --limit=2 --no-log` | Passed, exit 0, accepted positional prefix text and returned two `Ability*` tags. |
| `Plugins\Monolith\Binaries\monolith_query.exe project search_gameplay_tags --query Game --limit=2 --no-log` | Passed, exit 0, accepted space-separated `--query` text and returned two `GamePhase*` tags. |
| `Plugins\Monolith\Binaries\monolith_query.exe project search_gameplay_tags --limit=1 --no-log` | Passed as negative case, exit 1, error says `search_gameplay_tags requires a query argument (positional or --query=<text>)`. |
| `Plugins\Monolith\Binaries\monolith_query.exe project list_gameplay_tags --project-db <empty.db> --no-log` | Passed, exit 0, returned `status=schema_missing`, `missing_tables=["tags"]`, and no fake empty success. |
| `Plugins\Monolith\Binaries\monolith_query.exe project search_gameplay_tags Game --project-db <empty.db> --no-log` | Passed, exit 0, returned `status=schema_missing`, `missing_tables=["tags","tag_references","assets"]`, and no fake empty success. |
| `project list_gameplay_tags --help` / `project search_gameplay_tags --help` | Passed, both actions are documented by the offline help catalog. |

## 2. Notes

The ROI trigger was `Saved\Monolith\LogAnalysis\roi-20260703`, which showed `query:project.search_gameplay_tags` failing as an unknown offline action. The fix is additive: the live MCP actions already existed, and the offline CLI now reads the same ProjectIndex tables with explicit `schema_missing` JSON when required tables are absent.
