
## 2026-05-03 - Mesh Action Count
**Drift pattern:** MonolithMesh module counts drifted. Source reality (241: 194 core + 47 town gen) did not match spec/docs (240: 195 core + 45 town gen), likely due to previous feature work or PR merges introducing procedural/experimental actions while deleting core actions, without simultaneously updating the global registry counts.
**Source of truth:** The definitive action count is established by a static code audit mapping all `RegisterAction` calls in `Source/MonolithMesh/Private/`.
**Prevention:** Future code changes adding, removing, or refactoring action registrations in Monolith modules must manually trigger an action-count audit across the core documentation (`README.md`, `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`) to keep counts consistent.
## 2026-05-07 - MonolithEditor Action Count
**Drift pattern:** MonolithEditor module counts drifted. Source reality (34 actions) did not match spec/docs (24 actions in README, 26 in uplugin, 22 in module spec), due to 12 actions added in previous phases (CrashRecovery, SelectionActions, F8 updates) without simultaneously updating the module spec list or the global registry counts.
**Source of truth:** The definitive action count is established by a static code audit mapping all `RegisterAction` calls in `Source/MonolithEditor/Private/`.
**Prevention:** Future code changes adding, removing, or refactoring action registrations in Monolith modules must manually trigger an action-count audit across the core documentation (`README.md`, `Docs/SPEC_CORE.md`, `Docs/specs/SPEC_<Module>.md`, `Monolith.uplugin`) to keep counts consistent.

## 2026-05-08 - 🛡️ Sentinel: Spec — MonolithLogicDriver fix scaffold actions mismatch
**Drift pattern:** The scaffolding templates section in `Docs/specs/SPEC_MonolithLogicDriver.md` listed hypothetical or old actions instead of the actual `scaffold_weapon_sm`, `scaffold_game_flow_sm`, and `scaffold_interactable_sm` registered in source.
**Source of truth:** `Source/MonolithLogicDriver/Private/MonolithLogicDriverScaffoldActions.cpp` and actual runtime action registrations.
**Prevention:** Future scaffolding documentation additions or modifications must directly match the explicitly registered string identifiers in the source code.
