# Monolith SQLite Optimization: C++ Implementation Enhancements

**Status:** Implemented in SQLite implementation pass
**Date:** 2026-04-24
**Scope:** Application-layer (C++) enhancements supplementing `SQLite_Optimization_Spec.md`

This document details two critical application-layer (C++) enhancements that complement the structural and policy changes defined in the canonical `SQLite_Optimization_Spec.md`. These enhancements address edge cases in Unreal Engine's specific usage patterns and runtime environment without violating the original specification's Priority Lock. The CamelCase/PascalCase search improvement uses an additive `search_tokens` schema migration so stored display fields remain unchanged.

## Implementation Notes

- CamelCase/PascalCase expansion is centralized in `BuildMonolithSQLiteSearchText` under `MonolithCore`.
- Project and source indexes store supplemental `search_tokens` in the FTS payload instead of adding query-time wildcard scans.
- Dynamic memory awareness is implemented by passing `FPlatformMemory::GetStats().AvailablePhysical` into the pragma preset selector and capping requested cache/mmap/temp-store settings.

## 1. CamelCase & PascalCase Tokenization (Data-Level Normalization)

### 1.1. The Problem
The canonical spec standardizes the FTS5 tokenizer to `unicode61 "remove_diacritics=2"`. While this correctly handles standard delimiters (spaces, underscores) and removes the detrimental effects of the Porter stemmer on identifiers, it possesses a blind spot for CamelCase/PascalCase naming conventions heavily used in Unreal Engine (e.g., `MyPlayerCharacter`, `WBP_InventoryItem`).

The `unicode61` tokenizer treats `MyPlayerCharacter` as a single contiguous token. A user searching for "Player" or "Character" will not match this asset unless they search for the exact prefix "MyPlay...".

### 1.2. The C++ Implementation Solution
Modifying the SQLite tokenizer engine or utilizing complex tri-gram tokenizers violates the spec's non-goals (no engine modification, preserve performance). The solution exists in the C++ layer during data ingestion and writes supplemental tokens to an additive FTS payload column.

When indexing an asset or symbol, the C++ indexer must perform **Data-Level Normalization** before binding the parameters for the `INSERT` or `UPDATE` statement:

1. **Detect CamelCase:** Identify boundaries between lowercase and uppercase letters (e.g., via regex or a simple string traversal algorithm).
2. **Expand:** Transform `MyPlayerCharacter` into a supplementary string: `MyPlayerCharacter My Player Character`.
3. **Store:** Write this expanded string into `search_tokens`, which is indexed by FTS but kept separate from user-visible display fields such as `asset_name` and `qualified_name`.

### 1.3. Impact
- **Performance:** Negligible overhead during the background indexing phase (write path). Zero overhead during read queries.
- **UX:** Users can search for middle-words within CamelCase identifiers (e.g., searching "Inventory" finds `WBP_InventoryItem`), drastically improving search intuition.

---

## 2. Dynamic Memory Awareness for PRAGMA Tuning

### 2.1. The Problem
Section `9.3. Proposed memory tiers` of the canonical spec defines a static ladder for RAM tuning (e.g., `32GB+ RAM: 1GB mmap, 256MB cache`). 

While this ladder is a massive improvement over untuned connections, a static check against total installed physical memory (`FPlatformMemory::GetConstants().TotalPhysical`) is risky in a modern development environment. A developer with 32GB of RAM might be running the Unreal Editor, Rider/Visual Studio, multiple browser tabs, and a local server, leaving only 2GB of *available* RAM. Blindly allocating a large SQLite cache in this state will force the OS into heavy paging, severely degrading overall system performance.

### 2.2. The C++ Implementation Solution
The `MonolithSQLitePragmaPolicy::SelectPragmaPreset` helper (defined in Phase 1 of the spec) must evaluate **Available Physical Memory** dynamically at the moment the connection is opened, rather than relying solely on Total Physical Memory.

1. **Query Available RAM:** Utilize `FPlatformMemory::GetStats().AvailablePhysical`.
2. **Apply Safety Margins:** Ensure the requested `mmap_size` and `cache_size` never exceed a safe threshold of the *currently available* memory (e.g., maximum 10-15% of available RAM).
3. **Fallback:** If available RAM is critically low (e.g., < 2GB available), gracefully degrade to the `< 8GB` or `32-bit` conservative tier regardless of the total installed RAM.

### 2.3. Impact
- **Stability:** Prevents the Monolith plugin from causing out-of-memory (OOM) crashes or severe OS paging during heavy multitasking.
- **Compliance:** Perfectly adheres to the canonical spec's directive that "mmap_size must be treated as a request, not a guarantee" and ensures the plugin remains a good citizen within the Editor environment.
