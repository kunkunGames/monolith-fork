## 2024-05-24 - Harden SearchSource FTS query limit bounds and bindings
**Query contract:** Unbounded limit parameter injection in `SearchSourceFTS`, `SearchSourceFTSFiltered` and `SearchSymbolsFTSFiltered`.
**Learning:** Raw string insertion via `FString::Printf` for integer bounds like `LIMIT %d` using unclamped user limits is dangerous and permits resource exhaustion or query malfunction.
**Prevention:** Always clamp index limits (e.g., `FMath::Clamp(Limit, 1, 1000)`) and use SQLite prepared statement bindings (`LIMIT ?`) for numeric arguments in Monolith database handlers.
## 2024-05-08 - Harden ProjectListGameplayTagsAction query contract
**Query contract:** Gameplay tag list query limit, offset and wildcard escaping
**Learning:** Unclamped limits, unhandled offsets and unescaped wildcards in prefix filters can cause unbounded results and inefficient table scans.
**Prevention:** Always clamp limits, add offset support, and escape `%`, `_`, and `\` before applying them to a LIKE prefix filter.

## 2026-05-10 - Escape User Input in SQL LIKE Queries
**Query contract:** SQLite `LIKE` queries accepting user-controlled strings (e.g. `PathFilter` or `Category`).
**Learning:** Raw user inputs mapped directly to `LIKE` queries allow unbounded wildcards (`%` or `_`) which can bypass limits, lead to performance-degrading full-table scans, or unintended matches.
**Prevention:** Always escape user-provided wildcards (`%`, `_`, and `\`) in C++ using `.Replace()` and append `ESCAPE '\'` to the parameterized `LIKE` query string.

## 2026-05-10 - Harden ProjectSearchGameplayTagsAction LIKE contract
**Query contract:** Gameplay tag search query substring escaping
**Learning:** Unescaped user string used in a LIKE substring match can result in unconstrained wildcards altering query performance and matching logic.
**Prevention:** Always escape SQL wildcards `%`, `_`, and `\` before passing a user string to a parameter bound to a `LIKE` query, and explicitly use `ESCAPE '\'`.
