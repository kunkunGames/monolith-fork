## 2024-05-04 - Parameterize FTS FullTextSearch Limit Queries
**Query contract:** MonolithIndex project search limit handling
**Learning:** Appending user-controlled or unbounded limit parameters directly via FString::Printf is unsafe for memory exhaustion and breaks SQLite prepare caching rules.
**Prevention:** All database limit/offset queries should use prepared statement bindings (LIMIT ?) and input limits should be clamped explicitly using FMath::Clamp before evaluation.
