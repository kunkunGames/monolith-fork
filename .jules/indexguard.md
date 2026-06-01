2026-05-14 - Add Regression Test for Empty Search Query

Query contract: ProjectSearchAction handling of empty query parameters.
Learning: Ensure boundary cases such as an empty string `query` parameter are safely and explicitly rejected before passing to the backend indexing search system.
Prevention: Future actions must explicitly validate query constraints, e.g., using `!Query.IsEmpty()`, and we should add test coverage in `MonolithIndexQueryTests.cpp` for such malformed input.
2026-05-31 - Regression tests added for empty Source queries
Query contract: SourceSearchAction and SearchCrgGraph handling of empty query parameters.
Learning: Ensure boundary cases such as an empty string query parameter are safely rejected in MonolithSource actions before querying the index.
Prevention: Added test coverage in MonolithSourceQueryTests.cpp for malformed input to avoid regression.
