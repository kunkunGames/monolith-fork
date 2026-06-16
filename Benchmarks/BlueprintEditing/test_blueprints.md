# BlueprintEditing Benchmark — Test Blueprint Fixtures

This document specifies the 7 fixture Blueprints that must exist under `/Game/Benchmarks/` for the BlueprintEditing benchmark to produce rich, representative scores. Without these fixtures, read and edit tasks fall back to discovering empty or non-existent assets, which reduces score variance and makes capability gaps harder to distinguish.

The asset paths here match `tasks.jsonl` exactly — do not rename them.

## Why Fixtures Matter

- **Graph reads** score higher signal when the graph contains real nodes and pin connections.
- **Variable reads** require at least one variable per Blueprint to distinguish "empty list" from "action not working".
- **Edit execute tasks** (`edit_execute` category) call real mutations against these fixtures; they are the highest-weighted category (0.25) and require the fixtures to compile cleanly.
- **Workflow tasks** chain multiple steps; a fixture that already compiles cleanly isolates agent errors from asset errors.

---

## 1. BPB_TestActor

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/BPB_TestActor` |
| Parent class | `AActor` |
| Domain | General gameplay actor |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `Health` | `float` | `100.0` |
| `MaxHealth` | `float` | `100.0` |
| `ActorTag` | `FName` | `"BenchActor"` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `TakeDamage_Bench` | `Amount: float` | `RemainingHealth: float` |
| `Heal_Bench` | `Amount: float` | _(none)_ |

**Creation steps (blueprint_query MCP calls):**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/BPB_TestActor parent_class=AActor
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestActor variable_name=Health variable_type=float
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestActor variable_name=MaxHealth variable_type=float
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestActor variable_name=ActorTag variable_type=FName
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPB_TestActor function_name=TakeDamage_Bench
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPB_TestActor function_name=Heal_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/BPB_TestActor
```

---

## 2. BPB_TestCharacter

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/BPB_TestCharacter` |
| Parent class | `ACharacter` |
| Domain | Player/AI character |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `MoveSpeed` | `float` | `600.0` |
| `bIsSprinting` | `bool` | `false` |
| `CharacterName` | `FString` | `"BenchChar"` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `StartSprint_Bench` | _(none)_ | _(none)_ |
| `StopSprint_Bench` | _(none)_ | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/BPB_TestCharacter parent_class=ACharacter
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestCharacter variable_name=MoveSpeed variable_type=float
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestCharacter variable_name=bIsSprinting variable_type=bool
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BPB_TestCharacter variable_name=CharacterName variable_type=FString
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPB_TestCharacter function_name=StartSprint_Bench
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPB_TestCharacter function_name=StopSprint_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/BPB_TestCharacter
```

---

## 3. WBP_TestWidget

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/WBP_TestWidget` |
| Parent class | `UUserWidget` |
| Domain | UMG UI widget |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `DisplayText` | `FText` | `"Bench"` |
| `bIsVisible` | `bool` | `true` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `UpdateDisplay_Bench` | `NewText: FText` | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/WBP_TestWidget parent_class=UUserWidget
blueprint_query action=add_variable asset_path=/Game/Benchmarks/WBP_TestWidget variable_name=DisplayText variable_type=FText
blueprint_query action=add_variable asset_path=/Game/Benchmarks/WBP_TestWidget variable_name=bIsVisible variable_type=bool
blueprint_query action=add_function asset_path=/Game/Benchmarks/WBP_TestWidget function_name=UpdateDisplay_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/WBP_TestWidget
```

---

## 4. ABP_TestAnim

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/ABP_TestAnim` |
| Parent class | `UAnimInstance` |
| Domain | Animation state machine |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `Speed` | `float` | `0.0` |
| `bIsInAir` | `bool` | `false` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `UpdateLocomotion_Bench` | `CurrentSpeed: float` | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/ABP_TestAnim parent_class=UAnimInstance
blueprint_query action=add_variable asset_path=/Game/Benchmarks/ABP_TestAnim variable_name=Speed variable_type=float
blueprint_query action=add_variable asset_path=/Game/Benchmarks/ABP_TestAnim variable_name=bIsInAir variable_type=bool
blueprint_query action=add_function asset_path=/Game/Benchmarks/ABP_TestAnim function_name=UpdateLocomotion_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/ABP_TestAnim
```

---

## 5. GA_TestAbility

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/GA_TestAbility` |
| Parent class | `UGameplayAbility` |
| Domain | GAS ability |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `AbilityCooldown` | `float` | `1.0` |
| `AbilityCost` | `float` | `10.0` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `OnAbilityActivated_Bench` | _(none)_ | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/GA_TestAbility parent_class=UGameplayAbility
blueprint_query action=add_variable asset_path=/Game/Benchmarks/GA_TestAbility variable_name=AbilityCooldown variable_type=float
blueprint_query action=add_variable asset_path=/Game/Benchmarks/GA_TestAbility variable_name=AbilityCost variable_type=float
blueprint_query action=add_function asset_path=/Game/Benchmarks/GA_TestAbility function_name=OnAbilityActivated_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/GA_TestAbility
```

---

## 6. BC_TestComponent

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/BC_TestComponent` |
| Parent class | `UActorComponent` |
| Domain | Reusable actor component |

**Variables to create:**

| Name | Type | Default |
|------|------|---------|
| `ComponentID` | `int32` | `0` |
| `bIsActive` | `bool` | `true` |

**Functions to create:**

| Name | Inputs | Outputs |
|------|--------|---------|
| `Initialize_Bench` | `ID: int32` | _(none)_ |
| `Deactivate_Bench` | _(none)_ | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/BC_TestComponent parent_class=UActorComponent
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BC_TestComponent variable_name=ComponentID variable_type=int32
blueprint_query action=add_variable asset_path=/Game/Benchmarks/BC_TestComponent variable_name=bIsActive variable_type=bool
blueprint_query action=add_function asset_path=/Game/Benchmarks/BC_TestComponent function_name=Initialize_Bench
blueprint_query action=add_function asset_path=/Game/Benchmarks/BC_TestComponent function_name=Deactivate_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/BC_TestComponent
```

---

## 7. BPI_TestInterface

| Field | Value |
|-------|-------|
| Asset path | `/Game/Benchmarks/BPI_TestInterface` |
| Parent class | `UInterface` |
| Domain | Blueprint interface |

**Variables to create:**

_(Interfaces do not carry member variables; this fixture intentionally has none to validate empty-variable-list handling in `variable_read` tasks. The second `variable_read` slot for Interface calls `list_functions` instead.)_

**Functions to create (interface stubs):**

| Name | Inputs | Outputs |
|------|--------|---------|
| `GetDisplayName_Bench` | _(none)_ | `Name: FName` |
| `OnInteract_Bench` | `Instigator: AActor*` | _(none)_ |

**Creation steps:**

```
blueprint_query action=create_blueprint asset_path=/Game/Benchmarks/BPI_TestInterface parent_class=UInterface
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPI_TestInterface function_name=GetDisplayName_Bench
blueprint_query action=add_function asset_path=/Game/Benchmarks/BPI_TestInterface function_name=OnInteract_Bench
blueprint_query action=compile_blueprint asset_path=/Game/Benchmarks/BPI_TestInterface
```

---

## Fixture Verification

After creating all 7 fixtures, run the benchmark in read-only mode to verify server handling:

```powershell
python Scripts\blueprint_editing_benchmark.py run `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --label fixture-verify `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\fixture-verify
```

Expected: `graph_read_rate` and `variable_read_rate` ≥ 0.9 (fixtures exist and server handles requests), `edit_execute_rate` ≥ 0.8 (fixture assets are writable and the server can process edit calls).
