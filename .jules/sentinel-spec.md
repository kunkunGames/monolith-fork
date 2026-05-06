
## 2026-05-03 - Mesh Action Count
**Drift pattern:** MonolithMesh module counts drifted. Source reality (241: 194 core + 47 town gen) did not match spec/docs (240: 195 core + 45 town gen), likely due to previous feature work or PR merges introducing procedural/experimental actions while deleting core actions, without simultaneously updating the global registry counts.
**Source of truth:** The definitive action count is established by a static code audit mapping all `RegisterAction` calls in `Source/MonolithMesh/Private/`.
**Prevention:** Future code changes adding, removing, or refactoring action registrations in Monolith modules must manually trigger an action-count audit across the core documentation (`README.md`, `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`) to keep counts consistent.
