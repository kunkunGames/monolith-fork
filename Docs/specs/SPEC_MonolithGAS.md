# Monolith — MonolithGAS Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)

---

## MonolithGAS

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, GameplayAbilities, GameplayTags
**Namespace:** `gas` (136 actions) + 4 cross-namespace aliases into `ui` | **Tool:** `gas_query(action, params)` | **Actions:** 136 (Phase J F8: +`grant_ability_to_pawn`; 2026-05-19: +`get_runtime_summary`)
**Conditional:** GBA (Blueprint Attributes) features wrapped in `#if WITH_GBA`. Core GAS engine modules (GameplayAbilities, GameplayTags, GameplayTasks) are always available. When GBA is absent, Blueprint AttributeSet creation is disabled but all 136 actions still register and compile cleanly. When `bEnableGAS` is disabled in settings, 0 actions registered.
**Settings toggle:** `bEnableGAS` (default: True)

MonolithGAS provides full MCP coverage of the Gameplay Ability System. It covers ability CRUD, attribute set management, gameplay effect authoring, ASC (Ability System Component) inspection and manipulation, gameplay tag operations, gameplay cue management, target data, input binding, runtime inspection, scaffolding of common GAS patterns, and Widget→Attribute binding via class-extension authoring.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Abilities | 28 | Create, edit, delete, list, grant, activate, cancel, query gameplay abilities. Includes spec handles, instancing policy, tags, costs, cooldowns |
| Attributes | 20 | Create/edit attribute sets, get/set attribute values, define derived attributes, attribute initialization, clamping, replication config |
| Effects | 26 | Create/edit gameplay effects, duration policies, modifiers, executions, stacking, conditional application, period, tags granted/removed |
| ASC | 14 | Inspect/configure Ability System Components, list granted abilities, active effects, attribute values, owned tags, replication mode |
| Tags | 10 | Query gameplay tag hierarchy, check tag matches, add/remove loose tags, tag containers, tag queries |
| Cues | 10 | Create/edit gameplay cue notifies (static and actor), cue tags, cue parameters, handler lookup, optional registered-tag coverage audit |
| Targets | 5 | Target data handles, target actor selection, target data confirmation, custom target data types |
| Input | 5 | Bind abilities to Enhanced Input actions, input tag mapping, activation on input |
| Inspect | 7 | Runtime inspection of active abilities, applied effects, attribute snapshots, ability task state, prediction keys, and PIE-safe summary status via `get_runtime_summary` |
| Scaffold | 7 | Scaffold common GAS setups: init_attribute_set, init_asc_actor, init_ability_set, init_damage_pipeline, init_cooldown_system, init_stacking_effect, **`grant_ability_to_pawn`** (Phase J F8 — author-time append to ASC startup-abilities array via reflection) |
| UI Binding | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. Authored via `UMonolithGASAttributeBindingClassExtension`. **Also registered as aliases in the `ui` namespace** (so `ui::bind_widget_to_attribute` and `gas::bind_widget_to_attribute` dispatch to the same handler — see `MonolithGASUIBindingActions.cpp:561-577`). The `ui::` aliases are documented in [SPEC_MonolithUI.md](SPEC_MonolithUI.md) "GAS Bridge Aliases" section |

**Total:** 28 + 20 + 26 + 14 + 10 + 10 + 5 + 5 + 7 + 7 + 4 = **136**.

Effect authoring follows the ParamGuard rule: optional scalar fields use defaults only
when absent. Present wrong-type values, including modifier `value`, stacking limits,
duration magnitudes, component `chance`, and copy flags, return invalid-param errors
instead of being silently ignored.

### GameplayCue Coverage Contract

`gas::validate_cue_coverage` is the read-only audit entrypoint for GameplayCue wiring. It scans GameplayEffect Blueprint cue references and GameplayCue Notify Blueprint handlers, then returns `missing_handlers`, `orphaned_cues`, counts, and `fully_covered`.

| Parameter | Type | Default | Contract |
|-----------|------|---------|----------|
| `path_filter` | string | empty | Limits the existing GameplayEffect/GameplayCue Notify coverage scan to the package path. When registered-tag audit is enabled, Notify handlers are still compared against the global project handler set to avoid false positives for handlers outside the filtered path. |
| `include_registered_tags_without_notifies` | boolean | `false` | When true, walks the registered `GameplayCue` tag subtree through `UGameplayTagsManager` and returns registered cue tags that have no matching GameplayCue Notify handler anywhere in the project. The audit is read-only and does not create tags, assets, or compile Blueprints. |

| Response field | Type | Notes |
|----------------|------|-------|
| `missing_handlers` | string[] | Cue tags referenced by GameplayEffects but not handled by any GameplayCue Notify asset in scope. |
| `orphaned_cues` | object[] | GameplayCue Notify handlers whose cue tags are not referenced by any GameplayEffect in scope. |
| `registered_cue_tag_count` | number | Present only when `include_registered_tags_without_notifies=true`; number of registered child tags under `GameplayCue`. |
| `registered_notify_handler_scope` | string | Present only when requested; `current_scan` without `path_filter`, `global_project` when a path filter is used. |
| `registered_tags_without_notifies` | string[] | Present only when requested; sorted registered `GameplayCue.*` tags with no matching Notify handler. |
| `registered_tags_without_notifies_count` | number | Present only when requested; count of `registered_tags_without_notifies`. |

This closes the UE 5.8 `GASToolsets::FindCueTagsWithoutNotifies` parity gap for UE 5.7 while preserving the existing Monolith action surface and backward-compatible default response.

### Phase J fixes touching this module

- **F2 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects unknown `owner_resolver` (`ParseOwner` no longer silently coerces to `OwningPlayerPawn`).
- **F3 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects malformed `format=format_string` templates (new `ValidateFormatStringPayload` helper enforces `{0}` slot, `{1}` when `max_attribute` bound).
- **F5 (2026-04-26)** — Response shape & error-message drift cleanup (`index` → `binding_index`, composite `attribute`/`max_attribute` strings, `widget_class`, `removed_binding_index`, enriched valid-options enumerations).
- **F6 (2026-04-26)** — J1 spec relaxed to match impl (`warnings` omitted-when-empty, AttributeSet enumeration dropped, full-valid-list replaces Levenshtein "did you mean").
- **F8 (2026-04-26)** — `gas::grant_ability_to_pawn` added (+1).
- **F9 logging (2026-04-26)** — Observability adds + `LogMonolithGASUIBinding` / `LogMonolithGASUIBindingExt` retired into parent `LogMonolithGAS` category.
- **Runtime summary (2026-05-19)** — `gas::get_runtime_summary` added (+1). The action is read-only and PIE-safe: outside PIE it returns `pie_active=false` with zero counts instead of an error, and during PIE it summarizes matching ASCs plus optional actor samples.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### Notes

> **Runtime actions (Inspect category) require PIE for actor-specific state.** `get_runtime_summary` is the lightweight exception: it can be called outside PIE and reports `pie_active=false`; actor-specific actions such as `snapshot_gas_state`, `get_tag_state`, `get_cooldown_state`, and `trace_ability_activation` still return errors without a Play-In-Editor session.
>
> **GBA conditional support:** The `WITH_GBA` define is set automatically by the module's `Build.cs` when GameplayAbilities is found. Projects without GAS get zero compile overhead — the entire module compiles to an empty stub.
>
> **UI Binding cooked-build caveat.** `UMonolithGASAttributeBindingClassExtension` is an editor-only class — content WBPs that reference it will fail to apply bindings in cooked Steam builds. See [COOKED_BUILD_TODO.md](../COOKED_BUILD_TODO.md) for the resolution path (Option A/B/C deferred to pre-Steam-launch checkpoint).
