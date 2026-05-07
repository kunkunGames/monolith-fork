## 2024-05-24 - Harden SearchSource FTS query limit bounds and bindings
**Query contract:** Unbounded limit parameter injection in `SearchSourceFTS`, `SearchSourceFTSFiltered` and `SearchSymbolsFTSFiltered`.
**Learning:** Raw string insertion via `FString::Printf` for integer bounds like `LIMIT %d` using unclamped user limits is dangerous and permits resource exhaustion or query malfunction.
**Prevention:** Always clamp index limits (e.g., `FMath::Clamp(Limit, 1, 1000)`) and use SQLite prepared statement bindings (`LIMIT ?`) for numeric arguments in Monolith database handlers.
