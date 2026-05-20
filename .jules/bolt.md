2026-05-14 - Pre-allocate JSON arrays in PCG actions
Boundary: TArray<TSharedPtr<FJsonValue>> Reserve
Learning: When building large JSON responses in action handlers, use Reserve() before loops to prevent repeated dynamic memory allocations. Apply FMath::Min(SourceCount, Limit) or add upper bounds checking before reserving to prevent INT_MAX over-allocations when responding to client-provided limits.
Prevention: Future arrays populated by iteration should apply TArray::Reserve when the count is known via UE_ARRAY_COUNT, fixed sizes, or bounded inputs.
