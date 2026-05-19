# Monolith - MonolithWorldConditions Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)
**Status:** Spec-first implementation, 2026-05-19

---

## MonolithWorldConditions

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities
**Optional dependencies:** WorldConditions (`WITH_MONOLITH_WORLDCONDITIONS`), SmartObjects (`WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS`), GameplayTags when SmartObjects is present
**Namespace:** `world_conditions` | **Tool:** `world_conditions_query(action, params)` | **Actions:** 4
**Settings toggle:** `bEnableWorldConditionsInspection` (default: False)
**Feature class:** read-only inspection

MonolithWorldConditions provides bounded, read-only inspection for Unreal `FWorldConditionQueryDefinition` data. The first slice targets Smart Object definition preconditions because the GO project uses SmartObjects/WorldConditions through `AIK_SmartObjects`, and the UnrealMCP `WorldConditionsToolset` evidence shows this as a compact introspection gap rather than an authoring workflow.

---

## 1. Scope

| In scope | Out of scope |
|----------|--------------|
| Report WorldConditions/SmartObjects availability and feature flag state. | Mutating or creating WorldCondition queries. |
| List SmartObjectDefinition assets that can own WorldCondition queries. | Loading every project asset class to search arbitrary nested structs. |
| Describe object-level SmartObject preconditions. | Evaluating conditions at runtime. |
| Describe slot-level SmartObject selection preconditions. | ToolsetRegistry, external MCP import, or AIAssistant integration. |
| List loaded condition struct types and safe property summaries. | Serializing raw object pointers, addresses, or unbounded nested data. |

---

## 2. Actions

| Action | Params | Result contract |
|--------|--------|-----------------|
| `get_status` | none | Returns setting state, compile guards, module availability, and candidate owner count when AssetRegistry is available. |
| `list_query_owners` | `path_filter?`, `limit?` | Lists SmartObjectDefinition assets that can contain object/slot WorldCondition preconditions. |
| `describe_query` | `asset_path`, `query?`, `slot_index?` | Describes `preconditions` or `slot_selection_preconditions` as condition rows, schema, description, unsupported property count, and truncation state. |
| `describe_condition_types` | `limit?` | Lists loaded `FWorldConditionBase`-derived script structs with reflected property metadata. |

`query` defaults to `preconditions`. `slot_index` is required for `slot_selection_preconditions`.

---

## 3. Serialization Rules

| Data shape | Behavior |
|------------|----------|
| Primitive, enum, name, string, text | Serialized directly or as display strings. |
| Object/class references | Serialized as safe object paths or class paths only. |
| Structs such as GameplayTag/GameplayTagContainer | Exported as bounded text. |
| Arrays, maps, sets, delegates, raw pointers | Counted as unsupported unless explicitly handled later. |
| Large text values | Truncated and annotated. |

Condition rows include index, struct name, display name, operator, invert flag, expression depth, description, safe properties, unsupported property count, and truncation state.

---

## 4. Dependency Contract

| Case | Behavior |
|------|----------|
| `bEnableWorldConditionsInspection=false` | Actions remain discoverable, but inspection actions return `enabled=false` and no mutation occurs. |
| WorldConditions headers unavailable or release build guard active | `get_status` reports unavailable and inspection actions return `dependency_state=unavailable`. |
| SmartObjects unavailable | SmartObject owner listing and query description return `dependency_state=unavailable`; generic condition type summaries still work if WorldConditions is compiled in. |
| No candidate owners | Return success with empty arrays and explicit counts. |

The module must not hard-link optional dependencies in `MONOLITH_RELEASE_BUILD=1`.

---

## 5. Verification Gates

| Gate | Required check |
|------|----------------|
| Static docs/code hygiene | `git diff --check` |
| Monolith static CI | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` |
| UE 5.7 compile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Plugin="D:\P4\monolith-prs\worldconditions-readonly\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` |
