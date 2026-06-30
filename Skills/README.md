# Monolith Skills

Agent-facing skills that route Unreal Engine editor work through the **Monolith MCP** server.
Each subfolder is one skill (`<name>/SKILL.md`) with YAML frontmatter (`name`, `description`)
and an action reference for one or more Monolith namespaces. Link these directories into
Codex or Claude global skill roots with `Scripts/install_monolith_skills.ps1`; see
`Templates/Onboarding/Onboarding.md` for setup.

## Start here

The catalog is **runtime-discovered** (~1,900 actions across ~60 namespaces) and changes
between versions. Don't trust a hard-coded action list — confirm against the live catalog:

```
monolith_find("<task in plain words>")                                  # which namespace/action?
monolith_discover({ namespace: "<ns>" })                                 # actions in a namespace
monolith_discover({ namespace: "<ns>", action: "<action>", mode: "schema" })  # exact params
```

Then call the namespace tool: `{namespace}_query("<action>", { ...params })`.
See **monolith-mcp** for the discovery/admin surface and the offline `monolith_query.exe` CLI.

> The action counts below are a snapshot to aid routing. The registry is the source of truth.

## Skills by domain

### Discovery / meta
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `monolith-mcp` | `monolith` | 39 | Discover & route Monolith, server status, profiles, audit, readiness |
| `monolith-schema` | `describe`/`bulk_fill` | — | Schema-first discovery and validated reflected batch writes |

### Code & project
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-cpp` | `source` | 27 | C++ symbol/text search, references, hierarchy, risk/review/impact |
| `unreal-project-search` | `project` | ~20 | Asset search, references, type filter, gameplay tags, review/snapshots |
| `unreal-bridge` | `bridge` | 5 | Map assets/Blueprint nodes to their backing C++ symbols |
| `unreal-console` | `console` | 6 | Console object registry snapshots, cvars, console commands, hints, and guarded execution |
| `unreal-asset` | `asset` | 18 | Generic asset ingest, save/delete, inspection, live find, naming, batch rename, guarded content mount registration, package graph copy/remap, and dependency validation |
| `unreal-reflection-intel` | `cppreflect`/`network`/`decision`/`risk`/`reflect` | 20+ | Reflection Intelligence, replication audit, decision records, risk signals |
| `unreal-build` | `editor` | 59* | Build, hot reload, compile errors, changeset validation planning |
| `unreal-debugging` | `editor` | 59* | Logs, crash context, output log |
| `unreal-performance` | `config`/`material`/`niagara` | — | Cross-domain perf analysis — INI/CVar tuning, material shader stats, Niagara complexity |

### Gameplay
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-ai` | `ai` | 243 | Behavior Tree, StateTree, Blackboard, EQS, navigation, Smart Objects, Mass/ZoneGraph, perception, scaffolding |
| `unreal-gas` | `gas` | 136 | Gameplay Ability System: abilities, effects, attributes, cues, tags |
| `unreal-blueprints` | `blueprint` | 100 | Blueprint graphs, nodes, variables, functions, compile |
| `unreal-logicdriver` | `logicdriver` | 66 | LogicDriver state machines |
| `unreal-combograph` | `combograph` | 13 | ComboGraph combo/attack authoring |
| `unreal-input` | `input` | 10 | Enhanced Input: actions, mapping contexts, modifiers, triggers |
| `unreal-gamefeatures` | `gamefeatures` | 14 | Game Feature plugins, modular gameplay, instanced action authoring |
| `unreal-lyra` | `lyra` | 10 | Lyra Experience graphs/defaults, component-entry cleanup, bundle validation, UserFacingExperience hosting metadata, gameplay tag domains, and GamePhase flow diagnostics |
| `unreal-online` | `online` | 8 | EOS/OSSv2, CommonSession flow/schema, UserFacingSession, CommonUser initialization/privilege, and AccountPortal log diagnostics with credential redaction |
| `unreal-modular` | `modular` | 4 | ModularGameplay receiver lifecycle, AddComponentRequest actor/component target, and static extension-event source diagnostics |
| `unreal-gameplay-message` | `gameplay_message` | 4 | GameplayMessageRouter channel, match-type, and payload UScriptStruct diagnostics |
| `unreal-game-settings` | `settings` | 6 | GameSettings registry/screen/setting, dynamic path, visual-data, and player-mappable input diagnostics |
| `unreal-loading` | `loading` | 4 | CommonLoadingScreen manager reason, processor candidate, settings/CVar, and Lyra handoff diagnostics |
| `unreal-world-conditions` | `world_conditions` | 4 | World Conditions / world state |

### Spatial & level
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-scene` | `scene` | 76 | Live scene actors, spatial queries, volumes, lighting, decals, debug views, spatial registry |
| `unreal-mesh` | `mesh` | 70 | Mesh asset inspect/edit, GeometryScript, procedural geo, tech art, validation |
| `unreal-worldgen` | `worldgen` | 63 | Blockout + procedural town/building generation |
| `unreal-leveldesign` | `leveldesign` | 43 | Horror/accessibility/audio level analysis, encounter design |
| `unreal-level-instance` | `level_instance` | 16 | Level Instances & packed level actors |
| `unreal-hlod` | `hlod` | 12 | Hierarchical LOD / World Partition HLOD |
| `unreal-pcg` | `pcg` | 4 | Procedural Content Generation graphs |
| `unreal-water` | `water` | 2 | Water bodies & zones |

### Content
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-ui` | `ui` | 146 | UMG widgets, Lyra Common plugin diagnostics, bindings, slots, styling, templates, accessibility |
| `unreal-niagara` | `niagara` | 123 | Niagara VFX systems, emitters, modules |
| `unreal-animation` | `animation` | 135 | AnimBP, montages, sequences, blendspaces, layout |
| `unreal-audio` | `audio` | 98 | Sound assets, cues, MetaSounds, attenuation, mixing |
| `unreal-materials` | `material`/`asset` | 66 / 18 | Material graphs, instances, functions; asset ingest/inspect/validate |
| `unreal-metahuman` | `metahuman` | 2 | MetaHuman setup & layout |
| `unreal-slate` | `slate` | 6 | Editor Slate / Editor Utility Widgets |
| `unreal-paper2d` | `paper2d` | 3 | Sprites, flipbooks, tile maps |
| `unreal-sprite` | `sprite` | — | Sprite production contracts, sheets, icon atlases, metadata, handoff |
| `unreal-chaos-fracture` | `chaos_fracture` | 3 | Chaos destruction, Geometry Collections |
| `unreal-cloth` | `cloth` | 2 | Chaos Cloth simulation |
| `unreal-dataflow` | `dataflow` | 8 | Dataflow node graphs |
| `unreal-chooser` | `chooser` | 6 | Chooser tables (data-driven selection) |
| `unreal-interchange` | `interchange` | 16 | Interchange import/export pipelines |
| `unreal-modelgen` | `modelgen` | 7 | AI/procedural model generation jobs |
| `unreal-imagegen` | `imagegen` | 6 | AI image/texture generation |
| `unreal-ndisplay` | `ndisplay` | 2 | nDisplay / LED-wall virtual production |

### Sequencing
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-level-sequences` | `level_sequence` | 13 | Sequencer: tracks, sections, bindings |

### Project ops
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-config` | `config` | 10 | `.ini` config, sections, focused console-variable lookup |
| `unreal-source-control` | `source_control` | 11 | Perforce/Git status, checkout/add/delete, revert, opened/path mapping |
| `unreal-collection` | `collection` | 13 | Editor asset Collections |
| `unreal-localization` | `localization` | 10 | Localization targets, string tables, cultures |

\* `editor` actions are split across `unreal-build` (build/hot reload) and `unreal-debugging` (logs/crash). `unreal-performance` is cross-domain (`config`/`material`/`niagara`) profiling, not an `editor`-namespace skill; per-mesh triangle/draw-call/shadow budgeting lives in `unreal-mesh`.

## Reference companions

These two skills carry **no MCP actions** — they are read-only knowledge cards with on-demand
`references/` docs (paths relative to each skill folder, so they survive the per-skill junction/symlink
install). Each card holds a Quick Reference plus a table of reference documents. For any action on an
actual asset, route to the paired live-namespace skill.

- `material-reference/` — passive material/shader authoring knowledge for `unreal-materials`. Quick
  Reference (PBR base values, sampler/instruction budgets, GPU cycle costs, CPD-vs-DMI) plus
  `references/` docs: HLSL Custom-node guide, PBR values, material patterns, performance budgets,
  material systems/architecture, and anti-tiling. For create/edit/inspect/validate/optimize on a real
  material asset, use `unreal-materials`.
- `niagara-reference/` — passive Niagara VFX knowledge for `unreal-niagara`. Quick Reference (CPU-vs-GPU
  thresholds, fixed bounds, module order, GPU limits) plus `references/` docs: architecture, performance
  budgets, gotchas, material integration, and synthesized effect recipes (seeds — verify in-editor).
  Custom HLSL rules are not duplicated here; the authoritative guide is `Docs/NIAGARA_HLSL_GUIDE.md`. For
  any action on a real Niagara system, use `unreal-niagara`.

## Conventions

- Asset paths use UE object paths (no `.uasset`): `/Game/...`, `/PluginName/...`, engine `/PluginName/...`.
- `query_*` = live queries; `get_*` = stored reads. Most editor actions work without a PIE session.
- After C++ changes: `editor_query("get_build_errors")` then the project build; after indexing, CRG caches rebuild automatically.
- When an action seems missing or renamed, re-run `monolith_discover` — never guess action names or parameters.
