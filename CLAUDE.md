# Monolith Claude Instructions

Claude Code should follow `AGENTS.md` in this directory as the canonical Monolith coordination file. This file exists so Claude can discover the same local rules when it loads `CLAUDE.md`.

## Project Search Contract

- Live `project.search` and offline `Binaries\monolith_query.exe project search` default to content-inclusive search.
- Default search covers `fts_assets`, `fts_nodes`, `fts_variables`, `fts_parameters`, `fts_datatable_rows`, `fts_actors`, and `fts_asset_search_values`.
- Search results expose `match_source`, `match_table`, `match_field`, `match_object_path`, and `match_value`; use these fields before treating a result as an asset identity match.
- Use `include_content=false` / `--include-content=false` for bridge/source context, asset identity matching, or noisy name/type lookup.
- `project repair_fts --target=all` covers all seven project FTS tables. Prefer dry-run first on a live editor DB; use `--execute` only when repair is intended and the DB is writable, or verify on a copied DB.

```powershell
Binaries\monolith_query.exe project search Health --limit=10 --include-content=true
Binaries\monolith_query.exe project search Health --limit=10 --include-content=false
Binaries\monolith_query.exe project health --include-counts=true
Binaries\monolith_query.exe project repair_fts --target=all
```

Do not duplicate `EngineSource.db` source symbols or `graph.db` nodes into `ProjectIndex.db`; use `source` and `bridge` actions for source relationships.
