# Jules Agent Cross-Domain Coordination Map

This file establishes ownership boundaries and coordination rules for Jules scheduled agents. It prevents cross-domain overlaps where agents race to touch the same files. Updated 2026-06-10 to a module-ownership model after repeated Task-Agent-vs-Domain-Smith collisions (e.g., the same CreatePackage validation merged twice as #1042 and #1043).

## 1. Module Keepers (single owner per module)

A module keeper owns ALL routine maintenance inside its module: parameter hardening, input validation, domain correctness, focused tests under `Source/<Module>/Private/Tests/`, and parity of its module spec and skill docs. Other agents must not start work inside a keeper-owned module except for the explicitly shared concerns in section 2.

| Module | Keeper |
|---|---|
| Source/MonolithCore | CoreRegistry |
| Source/MonolithBlueprint | BlueprintSmith |
| Source/MonolithMaterial | MaterialSmith |
| Source/MonolithNiagara | NiagaraSmith |
| Source/MonolithAnimation | AnimWeaver |
| Source/MonolithGAS | GASWarden |
| Source/MonolithMesh | MeshCartographer |
| Source/MonolithUI | UISmith |
| Source/MonolithAudio, Source/MonolithAudioRuntime | AudioConductor |
| Source/MonolithAI | AIDirector |
| Source/MonolithLogicDriver | LogicDriverKeeper |
| Source/MonolithComboGraph | ComboGraphKeeper |
| Source/MonolithLevelSequence | SequenceDirector |
| Source/MonolithSource | SourceIndexer |
| Source/MonolithIndex | ProjectIndexer |
| Source/MonolithConfig | ConfigKeeper |
| Source/MonolithEditor | EditorCrashLens |
| Source/MonolithBABridge | BABridgeKeeper |

Keeper-less modules (WorldGen, Scene, LevelDesign, ReflectionIntel, ModelGen, ImageGen, Asset, Sprite, Paper2D, PCG, Water, ChaosFracture, Interchange, GameFeatures, WorldConditions, NDisplay, Slate, SourceControl, Dataflow) are serviced by the concern agents in section 2.

## 2. Concern Agents (cross-module, single concern)

| Concern | Agent | Operating area |
|---|---|---|
| Malformed-JSON / parameter-access hardening | ParamGuard | Keeper-less modules ONLY |
| Resource-boundary / input-size clamps | LimitGuard | Keeper-less modules ONLY |
| Narrow correctness fixes | Sentinel | Keeper-less modules ONLY |
| Focused regression tests | Sentinel Test | Keeper-less modules ONLY |
| FullyLoad data-loss and CreatePackage path validation | Crashguard | Repo-wide, one site per run, file-disjoint check required |
| Behavior-preserving performance (Reserve, allocation, lookup caching) | Bolt | Repo-wide within the module group named in its schedule slot |
| Reuse / dedup refactors (e.g., FMonolithAssetUtils adoption) | Sentinel Refactor | Repo-wide except MonolithCore, file-disjoint check required |
| Action-registry / handler / schema parity (code side) | RegistryGuard | Repo-wide registration contract surfaces |
| SQLite / FTS query contract safety | IndexGuard | DB/query layers of MonolithIndex and MonolithSource |

Rules for concern agents operating inside keeper-owned modules (Crashguard, Bolt, Sentinel Refactor, RegistryGuard, IndexGuard):
- Run the cross-agent branch check first; if the keeper (or any agent) has an open branch touching the intended files, stop without PR.
- Keep the diff strictly inside the named concern; do not bundle keeper-style domain work.

Rules for keepers:
- Yield the shared concern themes (performance micro-optimizations, FullyLoad/CreatePackage cleanup, pure reuse refactors) to the concern agents above; do not duplicate their patterns inside your module on the same surfaces they are actively mining.
- Everything else inside your module is yours, including parameter hardening and tests.

## 3. Documentation / Specification Agents (disjoint aspects)

- **SkillDocSmith:** `Skills/*/SKILL.md` accuracy versus live actions. No action counts, no `Docs/API_REFERENCE.md` edits.
- **ActionCountKeeper:** the ONLY agent allowed to change action/module counts, and only across all count-bearing docs in one PR. Stop if any count PR is open.
- **Sentinel Spec:** missing namespace/action coverage in `Docs/API_REFERENCE.md` and structural spec parity.
- **MCPContractAuditor:** fixes doc rows that contradict live action schemas (one action per run). If the code side is wrong, report and stop; the fix belongs to the module keeper.
- **ReleaseNotesScribe:** `CHANGELOG.md` entries and release notes content.
- **Harbor:** release/packaging scripts, updater preserve lists, version-header parity.

Docs agents must NOT modify `Source/` files. Code agents must not edit count-bearing doc rows.

## 4. Orchestration / Hygiene Agents

- **Marshal:** `AGENTS.md`, `.jules/`, `.github/` coordination rules — substantive, incident-driven changes only.
- **Curator:** `.gitignore`, generated-artifact hygiene, attribution/licensing consistency.
- **Forge:** `*.Build.cs` and `.uplugin` correctness, module-boundary/include hygiene.
- **OptionalDependencyScout:** optional-plugin detection guards in `Monolith.uplugin`/`Build.cs`. Forge owns non-optional build correctness; cross-check each other's branches before touching shared files.
- **StaticCIKeeper:** `.github/monolith-static-ci.json`, `Scripts/ci_static_checks.py`, `Scripts/lint_agent_tools.py`.
- **ProxyKeeper:** proxy scripts, `MCP/` templates, install/proxy README sections.

Only orchestration agents may update `AGENTS.md` and repository-wide coordination policies. Do NOT modify production code (`Source/`) from this group unless implementing a tiny, explicitly justified agent-infra helper.

## 5. Overlap Prevention Rules

- **Single owner first:** before selecting work, identify the owner of the target file via sections 1-4. If you are not the owner and the work is not your named concern, stop without PR.
- **Cross-agent branch check:** scan the full `git branch -r` list for semantic substring matches of your target module/area (3- and 4-segment branch names), plus `gh pr diff <PR> --name-only` for related open PRs. Same intended files anywhere = stop.
- **Forbid branch name evasion and template-echo titles:** never append numeric task IDs, UUIDs, or timestamp suffixes to branch names (e.g., `-17624609949312622604`). Never echo prompt templates in PR titles (e.g., '<Agent>: concise <domain> improvement.'). If your chosen branch name is taken or an overlapping PR exists, stop without PR instead of renaming the branch to bypass collision checks.
- **Micro-edits on shared files:** shared coordination files (`AGENTS.md`, release scripts, count-bearing docs) are high-collision zones. NO-OP unless the edit is your specific duty and substantive.
- **No-op is success:** when ownership is elsewhere, the queue already covers it, or no safe candidate exists, stop without creating a branch or PR. Do not push any branch (e.g., `no-op-<id>`) or create a PR to announce a no-op. Report your findings in the task log using the `done` tool instead.
