## 2024-05-10 - Harden Source DB query limit contract
**Query contract:** Unbounded limit parameter in `SearchSymbolsFTS`, `GetReferencesTo`, `GetReferencesFrom`, and `GetSymbolsInModule` methods within `MonolithSourceDatabase`.
**Learning:** Untrusted limit parameters can be passed directly to the SQLite query, potentially causing resource exhaustion or unbounded queries.
**Prevention:** Always explicitly clamp user-provided limit parameters using `FMath::Clamp(Limit, 1, 1000)` before calling `SetBindingValueByIndex` in SQLite prepared statements.
