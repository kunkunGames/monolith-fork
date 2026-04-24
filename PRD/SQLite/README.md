# SQLite PRD

This folder contains SQLite-related product and technical specification documents for Monolith.

## Document Rules

- Keep SQLite connection-policy, maintenance-policy, and benchmark guidance under this folder.
- Prefer one canonical spec per SQLite workstream. Split only when a document grows enough that implementation and review quality start to degrade.
- When adding supporting docs, link them from this README and mark which document is canonical.

## Canonical Spec

- [SQLite_Optimization_Spec.md](./SQLite_Optimization_Spec.md)

## Status

- Current canonical document status: `Implementation pass applied`
- Verification entry point: `python Plugins/Monolith/PRD/SQLite/verify_sqlite_prd.py`
- The current folder is still intentionally single-spec plus one supporting enhancement note. Split only when benchmark results or follow-up task tracking becomes large enough to distract from the design spec.

## Likely Follow-On Documents

- `SQLite_Benchmark_Protocol.md` — if benchmark procedure and results become large enough to distract from the design spec
- `SQLite_Implementation_Task_List.md` — if the implementation is broken into multiple tracked work packages

## Documents

- [SQLite_Optimization_Spec.md](./SQLite_Optimization_Spec.md) — read/write open-mode strategy, pragma policy, maintenance policy, statement-cache policy, and benchmarking guidance for Monolith SQLite databases

- [SQLite_Optimization_Enhancements.md](./SQLite_Optimization_Enhancements.md) — C++ application-layer improvements for CamelCase tokenization and dynamic memory awareness
- [verify_sqlite_prd.py](./verify_sqlite_prd.py) — static and SQLite smoke verification for the implemented PRD requirements
