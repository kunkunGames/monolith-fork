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

## 2026-06-27 - Forbid numeric branch evasion and no-op branches
**Coordination issue:** IndexGuard generated multiple branches with large numeric suffixes (e.g., `-6302586439543451557`, `-17166109155410785489`) and pushed a branch to announce a no-op (e.g., `noop-4215052388254789000`).
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes and no-op branches are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken, overlapping work exists, or no safe non-overlapping candidate exists, stop without creating a branch or PR. Never push a branch or open a PR to announce a no-op.
**Avoid:** Generating branches with `-<number>` suffixes or creating branches for no-op runs.
