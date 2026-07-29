# Monolith — MonolithGAS Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithGAS

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, GameplayAbilities, GameplayTags, GameplayTasks, EnhancedInput, InputCore
**Namespaces:** `gas` (135 actions) + `input` (10 actions) + 4 `gas` aliases into `ui` | **Tools:** `gas_query(action, params)`, `input_query(action, params)`
**Conditional:** `input` always registers. `gas` registration follows `bEnableGAS`; GBA (Blueprint Attributes) features alone are wrapped in `#if WITH_GBA`. Core GAS and Enhanced Input engine modules are regular module dependencies.
**Settings toggle:** `bEnableGAS` (default: True; affects `gas`, not `input`)

MonolithGAS provides full MCP coverage of the Gameplay Ability System plus an independent Enhanced Input asset surface. The `gas` namespace covers ability CRUD, attribute set management, gameplay effect authoring, ASC (Ability System Component) inspection and manipulation, gameplay tag operations, gameplay cue management, target data, ability-input binding, runtime inspection, scaffolding, and Widget→Attribute binding. The `input` namespace directly inspects and authors `UInputAction` and `UInputMappingContext` assets.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Abilities | 28 | Create, edit, delete, list, grant, activate, cancel, query gameplay abilities. Includes spec handles, instancing policy, tags, costs, cooldowns |
| Attributes | 20 | Create/edit attribute sets, get/set attribute values, define derived attributes, attribute initialization, clamping, replication config |
| Effects | 26 | Create/edit gameplay effects, duration policies, modifiers, executions, stacking, conditional application, period, tags granted/removed |
| ASC | 14 | Inspect/configure Ability System Components, list granted abilities, active effects, attribute values, owned tags, replication mode |
| Tags | 10 | Query gameplay tag hierarchy, check tag matches, add/remove loose tags, tag containers, tag queries |
| Cues | 10 | Create/edit gameplay cue notifies (static and actor), cue tags, cue parameters, handler lookup |
| Targets | 5 | Target data handles, target actor selection, target data confirmation, custom target data types |
| GAS ability input | 5 | Bind abilities to Enhanced Input actions, input tag mapping, activation on input |
| Inspect | 6 | Runtime inspection of active abilities, applied effects, attribute snapshots, ability task state, prediction keys |
| Scaffold | 7 | Scaffold common GAS setups: init_attribute_set, init_asc_actor, init_ability_set, init_damage_pipeline, init_cooldown_system, init_stacking_effect, **`grant_ability_to_pawn`** (Phase J F8 — author-time append to ASC startup-abilities array via reflection) |
| UI Binding | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. Authored via `UMonolithGASAttributeBindingClassExtension`. **Also registered as aliases in the `ui` namespace** (so `ui::bind_widget_to_attribute` and `gas::bind_widget_to_attribute` dispatch to the same handler — see `MonolithGASUIBindingActions.cpp:561-577`). The `ui::` aliases are documented in [SPEC_MonolithUI.md](SPEC_MonolithUI.md) "GAS Bridge Aliases" section |

**Total:** 28 + 20 + 26 + 14 + 10 + 10 + 5 + 5 + 6 + 7 + 4 = **135**.

### Enhanced Input Asset Surface (`input` namespace)

The 10 `input` actions are distinct from the five `gas` ability-input bindings above. They remain registered when `bEnableGAS=false` because `FMonolithGASInputAssetActions::RegisterActions` runs before the GAS settings guard.

| Category | Actions | Contract |
|----------|---------|----------|
| Input Action read | `list_input_actions`, `get_input_action` | Enumerate or inspect `UInputAction` assets under `/Game`. |
| Input Action write | `create_input_action`, `set_input_action_properties` | Guarded creation/update of value type, descriptions, input consumption, paused/reserved behavior, and accumulation. |
| Mapping Context read | `list_input_mapping_contexts`, `get_input_mapping_context` | Enumerate or inspect `UInputMappingContext` assets and their mappings. |
| Mapping Context write | `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping` | Guarded context creation plus deterministic action/key mapping edits, modifier/trigger cloning, and removal previews. |
| Validation | `validate_input_mappings` | Validate explicit `context_paths`, or all contexts under an optional `/Game` root, for missing actions and duplicate-key conflicts. |

Mutation and validation invariants:

1. Every writer requires either `dry_run=true` or `confirm=true`; `save` defaults to `false`.
2. Every dry-run reports `preview_state="proposed"` and returns before object/package creation, modifier/trigger package/class/CDO loading or UObject construction, mutation, dirtying, or save. Soft class-path syntax and already-loaded class compatibility are still validated; full class resolution is deferred until confirmation.
3. Asset paths and list roots must be valid `/Game` package paths. Omitted list roots default to `/Game`; no unscoped Engine/plugin scan is permitted. Explicit object names must match the package asset name. Required/optional booleans, strings, class arrays, and `context_paths` retain strict JSON types.
4. Semantic no-ops do not call `Modify()`, dirty a package, or save it.
5. `add_input_mapping` reuses an existing action+key mapping unless `allow_duplicate=true`. Cloning requires the complete `source_context_path` + `source_action_path` + `source_key` selector. Explicit `modifier_classes` or `trigger_classes` replace the cloned/existing arrays; an explicit empty array clears one.
6. `remove_input_mapping` reports `would_remove_count`; an absent mapping is a clean successful no-op.
7. Omitted `context_paths` selects contexts from optional `path`; explicit `context_paths: []` selects none and never falls back to a global scan.
8. Unsaved confirmed asset creation uses a package-stable custom transaction change. Undo removes the asset from its package path and the Asset Registry while the change retains it through GC; Redo restores the same object and authored values.
9. `save=true` is the explicit disk-persistence boundary. A post-mutation save failure returns structured `error.data` with the action result and explicit `mutation_committed`, `partial_mutation`, `save_failed`, and `retry_safe=false` state.

### Phase J fixes touching this module

- **F2 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects unknown `owner_resolver` (`ParseOwner` no longer silently coerces to `OwningPlayerPawn`).
- **F3 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects malformed `format=format_string` templates (new `ValidateFormatStringPayload` helper enforces `{0}` slot, `{1}` when `max_attribute` bound).
- **F5 (2026-04-26)** — Response shape & error-message drift cleanup (`index` → `binding_index`, composite `attribute`/`max_attribute` strings, `widget_class`, `removed_binding_index`, enriched valid-options enumerations).
- **F6 (2026-04-26)** — J1 spec relaxed to match impl (`warnings` omitted-when-empty, AttributeSet enumeration dropped, full-valid-list replaces Levenshtein "did you mean").
- **F8 (2026-04-26)** — `gas::grant_ability_to_pawn` added (+1).
- **F9 logging (2026-04-26)** — Observability adds + `LogMonolithGASUIBinding` / `LogMonolithGASUIBindingExt` retired into parent `LogMonolithGAS` category.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### Notes

> **Runtime actions (Inspect category) require PIE.** These actions query live game state and return errors if called outside a Play-In-Editor session.
>
> **GBA conditional support:** `WITH_GBA` is set by `MonolithGAS.Build.cs` only when the optional Blueprint Attributes plugin is found. It does not gate core GAS engine modules or the `input` namespace. `bEnableGAS=false` suppresses `gas` registration while leaving all 10 Enhanced Input asset actions available.
>
> **UI Binding cooked-build caveat.** `UMonolithGASAttributeBindingClassExtension` is an editor-only class — content WBPs that reference it will fail to apply bindings in cooked Steam builds. See [COOKED_BUILD_TODO.md](../COOKED_BUILD_TODO.md) for the resolution path (Option A/B/C deferred to pre-Steam-launch checkpoint).
>
> **Unity-safe file-local helpers (#68).** Internal-linkage helpers (anonymous-namespace functions/types, file-`static`s) must carry file-unique names or live in per-file named namespaces — matching the MonolithUI model — so they don't collide when adaptive/full unity concatenates same-module `.cpp`s into one translation unit.

---

### Bulk Fill & Describe Surface (2026-05-11)

The `gas` namespace registers a `FMonolithBulkFillRegistry` adapter (`MonolithGASBulkFillAdapter.cpp`) routed from the central `bulk_fill_query("apply")` and `describe_query("schema")` dispatchers. Phase 2 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`, implementation plan `Docs/plans/2026-05-11-monolith-mcp-ergonomics.md`). This collapses the 20-attr × 10-level ≈ 200-call grind on AttributeInit DataTables into a single transacted call.

**Surface summary.** `bulk_fill_query("apply", target_namespace="gas", target="<asset_path>", tree={...}, dry_run=<bool>, strict=<bool>)` walks the JSON tree against the target asset's reflection schema and either commits atomically or fails with a per-row error map. `describe_query("schema", target_namespace="gas", target="<asset_path>")` returns the settable surface — for AttributeInit DataTables, the `FAttributeMetaData` row schema; for everything else, the modifier-magnitude tagged-union descriptor.

**fill_kind catalogue (1 — enumerated against `MonolithGASBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `AttributeInitDataTable` | `UCurveTable` / `UDataTable` set up for `FAttributeMetaData` | `rows:{}` keyed by `[GroupName].[Level]` (per the engine's `FAttributeSetInitterDiscreteLevels` convention at `AttributeSet.h:303-318`), values are per-attribute scalars or `{base, min, max}` objects |

**H5 stub-adapter invariant:** when `bEnableGAS=true`, the adapter's `Register()` call runs regardless of `WITH_GBA`. The adapter body switches on `WITH_GBA` — the dev build wires the optional handlers; release builds without GBA return a clean `"GAS optional dep not available (WITH_GBA=0)"` error. This keeps the enabled `gas` discovery surface identical across dev + release builds. The `input` surface does not use this adapter or `WITH_GBA`.

#### `bulk_fill_query("apply", target_namespace="gas", target_asset=..., tree=...)`

Supported `fill_kind` (v1): **`AttributeInitDataTable`**.

Tree shape:

```json
{
  "fill_kind": "AttributeInitDataTable",
  "attribute_set": "UMyProjectAttributeSet",
  "rows": {
    "Player.1": { "MaxHealth": 100.0, "HealthRegenRate": 1.0, "AttackRating": 10 },
    "Player.2": { "MaxHealth": 200.0, "HealthRegenRate": 1.0, "AttackRating": 12 },
    "Player.10": { "MaxHealth": 999.0, "HealthRegenRate": 1.0, "AttackRating": 30 }
  }
}
```

- `attribute_set` accepts either a C++ class name (e.g. `"UMyProjectAttributeSet"` / `"MyProjectAttributeSet"`) or a Blueprint asset path (`"/Game/.../BP_VitalsSet"`).
- Each cell may be a bare number (sets `BaseValue` only) OR an object `{ "base": N, "min": N, "max": N }` (sets all three on `FAttributeMetaData`).
- Row names are stored as `[GroupName].[AttributeSetName].[Attribute]` per the engine's `FAttributeSetInitterDiscreteLevels` convention (`AttributeSet.h:303-318`).
- Pre-commit, every column-name in `rows[].*` is resolved against the `attribute_set` class. **A miss surfaces as a `SilentDrops` entry** with a "possible rename hazard" warning — this is the `FGameplayAttribute`-rename-invalidates-GEs quirk from the design's Cross-Cutting Engine Quirks table.
- `dry_run: true` returns the full FieldWrites report without touching the asset.
- `strict: true` rejects the whole batch and cancels the transaction if any cell errors.

#### `describe_query("schema", target_namespace="gas", target_asset=...)`

Returns:

- **`target_asset` is an AttributeInit DataTable** → returns the `FAttributeMetaData` row schema (`BaseValue:float`, `MinValue:float`, `MaxValue:float`, `DerivedAttributeInfo:FString`, `bCanStack:bool`).
- **`target_asset` is anything else** → returns the modifier-magnitude **tagged-union descriptor** (`ScalableFloat` / `AttributeBased` / `SetByCaller` / `CustomCalculationClass`) with per-variant `ConditionalOn` discriminators and ImportText sample forms — the GE describe surface from design Cross-Cutting Engine Quirks row.
- **`target_asset` is empty** → returns both shapes as children of a `gas` root descriptor so callers can introspect the namespace's full surface.

### Files

- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASBulkFillAdapter.h` / `.cpp` — the adapter
- `Plugins/Monolith/Source/MonolithGAS/Public/MonolithGASInputAssetActions.h` — Enhanced Input handler surface
- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASInputAssetActions.cpp` — 10 `input` action schemas and handlers
- `Plugins/Monolith/Source/MonolithGAS/Private/Tests/MonolithGASInputAssetActionsTests.cpp` — registration, guard, allocation-free dry-run, strict type/path handling, creation Undo/Redo, lifecycle/clone, and no-op automation
- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASModule.cpp` — namespace registration and shutdown
- `Plugins/Monolith/Skills/unreal-input/SKILL.md` — schema-first Enhanced Input workflow

