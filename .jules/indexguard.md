## 2024-05-24 - Harden SearchSource FTS query limit bounds and bindings
**Query contract:** Unbounded limit parameter injection in `SearchSourceFTS`, `SearchSourceFTSFiltered` and `SearchSymbolsFTSFiltered`.
**Learning:** Raw string insertion via `FString::Printf` for integer bounds like `LIMIT %d` using unclamped user limits is dangerous and permits resource exhaustion or query malfunction.
**Prevention:** Always clamp index limits (e.g., `FMath::Clamp(Limit, 1, 1000)`) and use SQLite prepared statement bindings (`LIMIT ?`) for numeric arguments in Monolith database handlers.
## 2024-05-08 - Harden ProjectListGameplayTagsAction query contract
**Query contract:** Gameplay tag list query limit, offset and wildcard escaping
**Learning:** Unclamped limits, unhandled offsets and unescaped wildcards in prefix filters can cause unbounded results and inefficient table scans.
**Prevention:** Always clamp limits, add offset support, and escape `%`, `_`, and `\` before applying them to a LIKE prefix filter.
