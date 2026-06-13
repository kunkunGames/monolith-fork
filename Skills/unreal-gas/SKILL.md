---
name: unreal-gas
description: Use when working with Unreal Engine Gameplay Ability System (GAS) via Monolith MCP (gas namespace) — creating and editing GameplayAbilities, AttributeSets, GameplayEffects, ASC setup, gameplay tags, gameplay cues, targeting, ability input binding, runtime PIE inspection, and project scaffolding. For the AI that decides WHEN to fire an ability use unreal-ai; for an ability fired as a ComboGraph chain step use unreal-combograph; to edit the montage an ability plays use unreal-animation; for the actor/component Blueprint graph around an ability use unreal-blueprints; to bind health/mana to a HUD widget use unreal-ui. Triggers on GAS, ability, GameplayAbility, GA, attribute set, AttributeSet, gameplay effect, GameplayEffect, GE, gameplay tag, gameplay cue, GCN, ASC, ability system component, cooldown, cost, modifier, stacking, ability task, damage pipeline, buff, debuff, DOT.
---

# Unreal GAS Workflows

Drives the **`gas`** namespace via `gas_query()` to create and edit GameplayAbilities, AttributeSets, GameplayEffects, ASC setup, gameplay tags/cues, targeting, ability input binding, runtime PIE inspection, and project scaffolding. **130+ GAS actions across 10 categories** — the tables below are a curated snapshot, not the full live catalog.

## Discovery

Always confirm the live action set and an action's exact parameter schema before calling — the registry is the source of truth, not these tables:

```
monolith_discover({ namespace: "gas" })                                  // list live actions
gas_query({ action: "create_gameplay_effect", mode: "schema" })          // exact params for one action
describe_query("action_schema", { namespace: "gas", action: "add_modifier" })  // equivalent schema form
monolith_find("create a damage gameplay effect")                         // jump straight to the right action
```

## When to use / Use a different skill for

Use **unreal-gas** for the GAS assets themselves — GameplayAbility/GameplayEffect/AttributeSet authoring, ASC setup, tags/cues, targeting, ability input binding, and runtime PIE inspection.

- **unreal-ai** — the AI that DECIDES when to activate an ability (Behavior Tree / StateTree / EQS / perception driving the pawn) rather than the ability/effect/attribute assets.
- **unreal-combograph** — the ability is fired as a step in a ComboGraph attack chain; author the chain there and have it call into GAS.
- **unreal-animation** — editing the montage or anim asset an ability plays (the `PlayMontageAndWait` target), as opposed to the ability/task wiring.
- **unreal-blueprints** — the actor/component Blueprint graph wiring around an ability rather than the GameplayAbility/GameplayEffect asset.
- **unreal-ui** — binding an attribute (health/mana) to a UMG HUD widget; the attribute-binding lives on the UI side.

## Key Parameters

Param names below are the real catalog names — note `attribute_set`/`actor`/`name`/`effect_class` are NOT `asset`/`actor_path`/`attribute_name`/`effect_path` from older drafts.

- `asset_path` — author-time GA/GE/cue/ASC-Blueprint asset path (most edit actions)
- `attribute_set` — attribute set asset path (attribute actions, NOT `asset_path`)
- `actor` / `actor_path` — runtime/PIE actor identity (`actor` for ASC/attribute/effect runtime ops, `actor_path` for ASC default-setup ops)
- `effect_class` / `cooldown_effect_class` / `cost_effect_class` / `ability_class` — GE/GA class refs (NOT `effect_path`)
- `name` — attribute name on an attribute set (NOT `attribute_name`)
- `cue_tag` / `tag` — gameplay tag string (e.g., `"Ability.Combat.Attack"`)
- `template` / `preset` — preset name (from `list_*` / scaffold presets)

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with mode `schema` (or `gas_query({ action, mode: "schema" })`). The Discovery block above stays the authority.

Full per-action parameter signatures — grouped by the 12 categories (Abilities 28, Attributes 20, Effects 26, ASC Setup 14, Tags 10, Cues 10, Targeting 5, Input Binding 5, Inspect/Debug 10, Scaffold/Bootstrap 7, Data Asset GAS Profile 3, Widget Attribute Binding 5) — live in [`references/actions.md`](references/actions.md). Common-name traps to remember:

- `attribute_set` is the attribute set asset path on attribute actions (NOT `asset_path`); `name` is the attribute name on a set (NOT `attribute_name`).
- Runtime/PIE ops use `actor`; ASC default-setup ops use `actor_path`.
- GE/GA class refs are `effect_class` / `cooldown_effect_class` / `cost_effect_class` / `ability_class` (NOT `effect_path`).
- Create/scaffold actions use `save_path` for the new asset path.
- `set_data_asset_gas_fields` is `dry_run=true` by default — pass `dry_run=false save=true` to actually write.

## Templates

### Survival Horror (this project)
- `horror_attributes` — Health, Stamina, Sanity, Horror, PainThreshold
- `horror_effects` — Bleeding, Infection, Exhaustion, Panic, Adrenaline
- `horror_abilities` — Sprint, HeavyAttack, Heal, BarricadeDoor, Flashlight
- `horror_asc_player` — Full player ASC with accessibility GEs
- `horror_asc_ai` — AI ASC: minimal attributes, aggro abilities
- `horror_tags` — Full tag hierarchy (State, Ability, Effect, Damage, Status)

### Generic
- `basic_attributes` — Health, MaxHealth, Mana, MaxMana, AttackPower, Defense
- `basic_damage_effect` / `basic_heal_effect` / `basic_dot_effect` / `basic_buff_effect`
- `basic_melee_ability` / `basic_projectile_ability`
- `basic_asc_player` (on PlayerState) / `basic_asc_ai` (on Actor)

## Technical Notes

1. **UK2Node_LatentAbilityCall** — AT nodes use this, NOT UK2Node_CallFunction. `add_ability_task_node` handles it; if using `blueprint_query("add_node")` directly, specify correct type.
2. **GE Component Model (5.3+)** — Use `add_ge_component`/`set_ge_component`/`remove_ge_component` instead of legacy field setters.
3. **IAbilitySystemInterface is C++ only** — `setup_ability_system_interface` guides to C++ implementation.
4. **ComboGraph globals** — Check `DefaultGame.ini` for `GlobalAbilityList` entries that auto-grant abilities.
5. **GBA plugin** — Allows Blueprint-only attribute sets. Check before recommending C++-only workflows.
6. **Hospice accessibility** — Scaffold infinite-duration GEs for accessibility modes. Horror templates include these by default.

## Common Workflows

### Full Bootstrap
```
gas_query({ action: "bootstrap_gas_foundation", params: { project_name: "Horror" } })
// bootstrap_gas_foundation takes only project_name (optional). For a specific
// template / damage pipeline, use the scaffold_* actions in the Scaffolding table.
```

### Create GE with Modifiers
```
gas_query({ action: "create_gameplay_effect", params: {
  save_path: "/Game/GAS/Effects/GE_Damage_Bleed", duration_policy: "duration"
}})
gas_query({ action: "add_modifier", params: {
  asset_path: "/Game/GAS/Effects/GE_Damage_Bleed",
  attribute: "Health", operation: "Additive", magnitude: { type: "ScalableFloat", value: -5.0 }
}})
gas_query({ action: "set_period", params: {
  asset_path: "/Game/GAS/Effects/GE_Damage_Bleed", period: 1.0, execute_on_application: true
}})
```

## Validation Catches

- Missing InitAbilityActorInfo → `validate_asc_setup`
- Cost GE with wrong duration (must be Instant) → `validate_effect`
- Cooldown GE without duration → `validate_effect`
- Infinite stacking without limit → `validate_effect`
- Meta attribute without execution calc → `validate_attribute_set`
- Orphaned cue tags (no GCN asset) → `validate_cue_coverage`
- Tag typos (used but not registered) → `validate_tag_consistency`
- Unbound abilities → `validate_gas_setup`
- Runtime actions (`get/set_attribute_value`, `apply/remove_effect`, `grant/revoke_ability`) only work during PIE
