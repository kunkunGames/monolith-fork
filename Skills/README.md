# Monolith Skills

Agent-facing skills that route Unreal Engine editor work through the **Monolith MCP** server.
Each subfolder is one skill (`<name>/<name>.md`) with YAML frontmatter (`name`, `description`)
and an action reference for one Monolith namespace.

## Start here

The catalog is **runtime-discovered** (~1,600 actions across ~40 namespaces) and changes
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

### Code & project
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-cpp` | `source` | 27 | C++ symbol/text search, references, hierarchy, risk/review/impact |
| `unreal-project-search` | `project` | ~20 | Asset search, references, type filter, gameplay tags, review/snapshots |
| `unreal-bridge` | `bridge` | 5 | Map assets/Blueprint nodes to their backing C++ symbols |
| `unreal-build` | `editor` | 57* | Build, hot reload, compile errors |
| `unreal-debugging` | `editor` | 57* | Logs, crash context, output log |
| `unreal-performance` | `editor`/`mesh` | — | Profiling, triangle/draw-call/shadow budgets |

### Gameplay
| Skill | Namespace | ~Actions | Use for |
|-------|-----------|---------:|---------|
| `unreal-ai` | `ai` | 243 | Behavior Tree, StateTree, Blackboard, EQS, navigation, Smart Objects, Mass/ZoneGraph, perception, scaffolding |
| `unreal-gas` | `gas` | 136 | Gameplay Ability System: abilities, effects, attributes, cues, tags |
| `unreal-blueprints` | `blueprint` | 100 | Blueprint graphs, nodes, variables, functions, compile |
| `unreal-logicdriver` | `logicdriver` | 66 | LogicDriver state machines |
| `unreal-combograph` | `combograph` | 13 | ComboGraph combo/attack authoring |
| `unreal-input` | `input` | 10 | Enhanced Input: actions, mapping contexts, modifiers, triggers |
| `unreal-gamefeatures` | `gamefeatures` | 5 | Game Feature plugins, modular gameplay |
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
| `unreal-ui` | `ui` | 119 | UMG widgets, bindings, slots, styling, templates, accessibility |
| `unreal-niagara` | `niagara` | 109 | Niagara VFX systems, emitters, modules |
| `unreal-animation` | `animation` | 135 | AnimBP, montages, sequences, blendspaces, layout |
| `unreal-audio` | `audio` | 98 | Sound assets, cues, MetaSounds, attenuation, mixing |
| `unreal-materials` | `material`/`asset` | 63 / 8 | Material graphs, instances, functions; asset ingest/inspect/validate |
| `unreal-metahuman` | `metahuman` | 2 | MetaHuman setup & layout |
| `unreal-slate` | `slate` | 6 | Editor Slate / Editor Utility Widgets |
| `unreal-paper2d` | `paper2d` | 3 | Sprites, flipbooks, tile maps |
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
| `unreal-config` | `config` | 10 | `.ini` config, sections, console variables |
| `unreal-source-control` | `source_control` | 9 | Perforce/Git status, checkout/add/delete, revert |
| `unreal-collection` | `collection` | 13 | Editor asset Collections |
| `unreal-localization` | `localization` | 10 | Localization targets, string tables, cultures |

\* `editor` actions are split across `unreal-build` (build/hot reload), `unreal-debugging` (logs/crash), and `unreal-performance` (profiling).

## Reference companions

- `material-reference/` — material-expression reference for `unreal-materials`.
- `niagara-reference/` — Niagara module/data-interface reference for `unreal-niagara`.

## Conventions

- Asset paths use UE object paths (no `.uasset`): `/Game/...`, `/PluginName/...`, engine `/PluginName/...`.
- `query_*` = live queries; `get_*` = stored reads. Most editor actions work without a PIE session.
- After C++ changes: `editor_query("get_build_errors")` then the project build; after indexing, CRG caches rebuild automatically.
- When an action seems missing or renamed, re-run `monolith_discover` — never guess action names or parameters.
