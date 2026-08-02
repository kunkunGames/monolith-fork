# Monolith — MonolithGAS Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.22.0 (Beta)

---

## MonolithGAS

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, GameplayAbilities, GameplayTags, EnhancedInput
**Namespace:** `gas` (142 actions) + 4 cross-namespace aliases into `ui` | **Tool:** `gas_query(action, params)` | **Actions:** 142 (Phase J F8: +`grant_ability_to_pawn`; 2026-05-31 DataAsset GAS workflow P0/P3/P4: +DataAsset profile describe/validate/set and runtime event/cue probes)
**Conditional:** GBA (Blueprint Attributes) features wrapped in `#if WITH_GBA`. Core GAS engine modules (GameplayAbilities, GameplayTags, GameplayTasks) are always available. When GBA is absent, Blueprint AttributeSet creation is disabled but all 142 actions still register and compile cleanly. When `bEnableGAS` is disabled in settings, 0 `gas` actions registered.
**Settings toggle:** `bEnableGAS` (default: True)

MonolithGAS provides full MCP coverage of the Gameplay Ability System. It covers ability CRUD, attribute set management, gameplay effect authoring, ASC (Ability System Component) inspection and manipulation, gameplay tag operations, gameplay cue management, target data, input binding, runtime inspection, scaffolding of common GAS patterns, and Widget→Attribute binding via class-extension authoring.

Related follow-up: [SPEC_MonolithGAS_GoWorkflowImprovements.md](SPEC_MonolithGAS_GoWorkflowImprovements.md) captures DataAsset-driven skill, tag-based input, held/channel policy, runtime cue/event proof, and offline GAS validation improvements discovered during the Go GAS enhancement pass. The implementation adds DataAsset GAS profile describe/validate actions, manifest embedding, release-input ability validation, safe dry-run-first DataAsset profile writes, runtime event/cue probe actions, and readiness fields on `get_runtime_summary`. A dedicated offline `monolith_query.exe gas` namespace remains deferred to preserve CLI routing cohesion.

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
| Input | 5 | Bind abilities to Enhanced Input actions, input tag mapping, activation on input |
| DataAsset Profile | 3 | `describe_data_asset_gas_profile`, `validate_data_asset_gas_profile`, `set_data_asset_gas_fields` for DataAsset-driven GAS skill/profile inspection, validation, and transacted dry-run-first field writes |
| Inspect | 10 | Runtime inspection of active abilities, applied effects, attribute snapshots, ability task state, prediction keys, safe readiness summary, and bounded PIE event/cue probes |
| Scaffold | 7 | Scaffold common GAS setups: init_attribute_set, init_asc_actor, init_ability_set, init_damage_pipeline, init_cooldown_system, init_stacking_effect, **`grant_ability_to_pawn`** (Phase J F8 — author-time append to ASC startup-abilities array via reflection) |
| UI Binding | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. Authored via `UMonolithGASAttributeBindingClassExtension`. **Also registered as aliases in the `ui` namespace** (so `ui::bind_widget_to_attribute` and `gas::bind_widget_to_attribute` dispatch to the same handler — see `MonolithGASUIBindingActions.cpp:561-577`). The `ui::` aliases are documented in [SPEC_MonolithUI.md](SPEC_MonolithUI.md) "GAS Bridge Aliases" section |

**Total:** 28 + 20 + 26 + 14 + 10 + 10 + 5 + 5 + 3 + 10 + 7 + 4 = **142**.

### Phase J fixes touching this module

- **F2 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects unknown `owner_resolver` (`ParseOwner` no longer silently coerces to `OwningPlayerPawn`).
- **F3 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects malformed `format=format_string` templates (new `ValidateFormatStringPayload` helper enforces `{0}` slot, `{1}` when `max_attribute` bound).
- **F5 (2026-04-26)** — Response shape & error-message drift cleanup (`index` → `binding_index`, composite `attribute`/`max_attribute` strings, `widget_class`, `removed_binding_index`, enriched valid-options enumerations).
- **F6 (2026-04-26)** — J1 spec relaxed to match impl (`warnings` omitted-when-empty, AttributeSet enumeration dropped, full-valid-list replaces Levenshtein "did you mean").
- **F8 (2026-04-26)** — `gas::grant_ability_to_pawn` added (+1).
- **F9 logging (2026-04-26)** — Observability adds + `LogMonolithGASUIBinding` / `LogMonolithGASUIBindingExt` retired into parent `LogMonolithGAS` category.
- **DataAsset GAS workflow P0/P3/P4 (2026-05-31)** — `gas::describe_data_asset_gas_profile`, `gas::validate_data_asset_gas_profile`, `gas::set_data_asset_gas_fields`, `gas::start_event_cue_probe`, `gas::stop_event_cue_probe`, and `gas::expect_event_cue` added (+6). `gas::get_runtime_summary` now reports GAS namespace registration, action count, `WITH_GBA`, ProjectIndex availability, and read-only fallback guidance while remaining safe outside PIE.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### Notes

> **Runtime actions (Inspect category) require PIE.** These actions query live game state and return errors if called outside a Play-In-Editor session.
>
> **GBA conditional support:** GameplayAbilities is a required engine-plugin dependency because the module's core action surface directly links GAS modules. The module's `Build.cs` sets `WITH_GBA` only when the optional Blueprint Attributes plugin is enabled; without it, Blueprint AttributeSet creation is omitted while the core GAS actions still compile and register.
>
> **UI Binding cooked-build caveat.** `UMonolithGASAttributeBindingClassExtension` is an editor-only class — content WBPs that reference it will fail to apply bindings in cooked Steam builds. See [COOKED_BUILD_TODO.md](../COOKED_BUILD_TODO.md) for the resolution path (Option A/B/C deferred to pre-Steam-launch checkpoint).
>
> **Unity-safe file-local helpers (#68).** Internal-linkage helpers (anonymous-namespace functions/types, file-`static`s) must carry file-unique names or live in per-file named namespaces — matching the MonolithUI model — so they don't collide when adaptive/full unity concatenates same-module `.cpp`s into one translation unit.

---

### Enhanced Input Asset Namespace (`input`)

`MonolithGASInputAssetActions.cpp` registers ten Enhanced Input asset-authoring actions under the standalone `input` namespace: `list_input_actions`, `get_input_action`, `create_input_action`, `set_input_action_properties`, `list_input_mapping_contexts`, `get_input_mapping_context`, `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping`, and `validate_input_mappings`.

`input.add_input_mapping` is not a bare append helper. By default it searches the target `UInputMappingContext` for an existing action+key row and updates that row instead of duplicating it. It can clone `Modifiers` and `Triggers` from a source mapping (`source_context_path`, `source_action_path`, `source_key`) or replace them with explicit `modifier_classes` / `trigger_classes`; empty explicit arrays clear that side. The handler deep-compares the desired modifier/trigger objects against the current mapping before marking the context dirty, supports `dry_run`, and requires `allow_duplicate=true` before intentionally adding a second identical action+key row.

The same action owns the first-class per-row Enhanced Input remapping contract. `player_mappable=true` requires non-empty `mapping_name`, `display_name`, and `display_category`; `mapping_name` must also resolve to a non-`None` `FName`. The action optionally accepts a de-duplicated `supported_key_profile_ids` string array and writes an instanced `UPlayerMappableKeySettings` override on that exact action+key row. `player_mappable=false` explicitly opts the row out with `IgnoreSettings`; omitting `player_mappable` preserves the existing behavior and metadata. Metadata fields without `player_mappable=true`, malformed arrays, empty profile IDs, `None` mapping identities, and unsupported engine property layouts fail before an asset is mutated. The response exposes whether only player-mappable metadata changed and returns the authored mapping when the edit is executed.

`input.validate_input_mappings` follows the UE 5.8 Enhanced Input data contract instead of assuming that every repeated key is an error. A shared key bound to distinct actions is legal and is returned under `shared_keys` / `shared_key_groups` for inspection. A hard `duplicate_mapping_conflict` requires two or more rows with the same action, key, ordered modifier instances, and ordered trigger instances, including equivalent instanced-object property values. Missing actions remain hard errors. `EKeys::Invalid` / `None` rows are reported under `unbound_rows` / `unbound_mappings` but remain informational by default, matching the engine's default data-validation policy; callers that require every row to have a concrete key pass `fail_on_unbound=true`. The top-level `conflicts` field is retained for compatibility and now counts exact duplicate-mapping groups.

`input.create_input_mapping_context` accepts optional `registration_tracking_mode=Untracked|CountRegistrations` when creating or updating an IMC. It validates the enum before mutation, writes the reflected UE property, saves through the normal package path, and returns the applied mode in the standard IMC readback. Use `CountRegistrations` whenever independent systems can install the same context: every installer must then issue exactly one matching add/remove pair instead of inferring ownership from `HasMappingContext()`.

---

### Bulk Fill & Describe Surface (2026-05-11)

The `gas` namespace registers a `FMonolithBulkFillRegistry` adapter (`MonolithGASBulkFillAdapter.cpp`) routed from the central `bulk_fill_query("apply")` and `describe_query("schema")` dispatchers. Phase 2 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`, implementation plan `Docs/plans/2026-05-11-monolith-mcp-ergonomics.md`). This collapses the 20-attr × 10-level ≈ 200-call grind on AttributeInit DataTables into a single transacted call.

**Surface summary.** `bulk_fill_query("apply", target_namespace="gas", target="<asset_path>", tree={...}, dry_run=<bool>, strict=<bool>)` walks the JSON tree against the target asset's reflection schema and either commits atomically or fails with a per-row error map. `describe_query("schema", target_namespace="gas", target="<asset_path>")` returns the settable surface — for AttributeInit DataTables, the `FAttributeMetaData` row schema; for everything else, the modifier-magnitude tagged-union descriptor.

**fill_kind catalogue (1 — enumerated against `MonolithGASBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `AttributeInitDataTable` | `UCurveTable` / `UDataTable` set up for `FAttributeMetaData` | `rows:{}` keyed by `[GroupName].[Level]` (per the engine's `FAttributeSetInitterDiscreteLevels` convention at `AttributeSet.h:303-318`), values are per-attribute scalars or `{base, min, max}` objects |

**H5 stub-adapter invariant:** the adapter's `Register()` call runs unconditionally from `FMonolithGASModule::StartupModule` regardless of `WITH_GBA`. The adapter BODY switches on `WITH_GBA` — the dev build wires the real handlers; release builds without GAS return a clean `"GAS optional dep not available (WITH_GBA=0)"` error. This guarantees `monolith_discover("gas")` action surface is identical across dev + release builds.

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
- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASModule.cpp` — `Register()` + `Unregister()` call sites
