# IndexGuard PR Guidance

## PR Intent
IndexGuard PRs keep project/source indexing safe, bounded, and consistent with the SQLite/FTS source of truth.

## Code Work Improvements
- Preserve index query result shapes, path normalization, and FTS behavior unless the PR explicitly documents a contract fix.
- Harden untrusted query params with type checks and bounds before database work begins.
- Keep index specs/API docs synchronized only for real public action or result changes.
- **Param validation**: Use explicit `HasField` and `TryGet*Field` checks for optional query parameters (e.g., `limit`, `offset`, `ref_kind`), returning standard JSON-RPC `-32602` Invalid params errors when type expectations are violated, rather than silently defaulting.

## Review Gate
- Inspect active ProjectIndexer, SourceIndexer, and ParamGuard PRs before editing index actions or tests.
- Verify SQL/FTS changes with focused static inspection or tests where available.
- Do not update global action counts from index work unless registrations actually changed.

## Journal
2026-05-14 - [Harden source.find_references query contract]
Query contract: [Hardened limit and ref_kind parameters in HandleFindReferences by replacing unsafe value fetches with TryGetNumberField/TryGetStringField type checks.] Learning: [Silent parameter defaults hide integration errors from MCP callers; enforcing type correctness ensures predictable DB behavior.] Prevention: [Future index actions should always validate optional parameter types before claiming defaults.]
