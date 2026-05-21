# Project Search Content FTS and Agent Surface Sync

**Date:** 2026-05-22
**Scope:** ProjectIndex content-inclusive FTS, offline `monolith_query.exe` parity, API/docs/skills/agent instruction sync

---

## Verification Matrix

| Check | Command / Surface | Result |
| --- | --- | --- |
| Native query build | `cmd /c Tools\MonolithQuery\build.bat` | PASS. Built and copied `Binaries\monolith_query.exe`; MSVC emitted existing C4819 encoding warnings only. |
| Offline project search default parity | `Binaries\monolith_query.exe project search GoInteractableInterface --limit=10 --include-content=true` | PASS. Returned 3 hits: one `node` hit and two `supplemental_value` hits from `asset_search_values`, with match provenance fields populated. |
| Offline project search escape hatch | `Binaries\monolith_query.exe project search GoInteractableInterface --limit=10 --include-content=false` | PASS. Returned only the asset/node-compatible node hit. |
| Offline project health | `Binaries\monolith_query.exe project health --include-counts=true` | PASS. `status=ok`; row counts included assets 597, nodes 3173, variables 169, parameters 420, datatable_rows 11, actors 42, asset_search_values 32, dependencies 528. |
| Offline project stats | `Binaries\monolith_query.exe project get_stats` | PASS. Included `asset_search_values=32` and `meta=4`. |
| Offline FTS dry-run | `Binaries\monolith_query.exe project repair_fts --target=all` | PASS. Planned 7 FTS rebuilds. |
| Offline FTS execute on copied DB | Copy `Saved\ProjectIndex.db` to `%TEMP%`, then `Binaries\monolith_query.exe project repair_fts --target=all --execute=true --db <copy>` | PASS. Rebuilt 7 FTS tables; after counts matched assets 597, nodes 3173, variables 169, parameters 420, datatable_rows 11, actors 42, asset_search_values 32. |
| Live DB execute note | `Binaries\monolith_query.exe project repair_fts --target=all --execute=true --db Saved\ProjectIndex.db` while editor was running | BLOCKED. SQLite returned `attempt to write a readonly database`. Dry-run and copied-DB execute passed; use live MCP `project.repair_fts execute=true` or a copied DB for offline write verification while the editor owns the live DB. |
| CLI help | `Binaries\monolith_query.exe` | PASS. Help documents `project search <query> [--limit=N] [--include-content=true\|false]` and all seven `repair_fts` targets. |
| Agent-facing docs and skills | `README.md`, `Docs/API_REFERENCE.md`, `Docs/specs/SPEC_MonolithIndex.md`, repo skills, Codex skills, Claude skills, `AGENTS.md`, `CLAUDE.md` | PASS. Surfaces now describe default `include_content=true`, `include_content=false`, all seven project FTS tables, and match provenance fields. |

## Notes

- `project.search` should be used in default content-inclusive mode for discovery.
- Identity-sensitive consumers should use `include_content=false` / `--include-content=false`.
- Do not duplicate `EngineSource.db` source symbols or `graph.db` nodes into `ProjectIndex.db`; use source and bridge actions for source relationships.
