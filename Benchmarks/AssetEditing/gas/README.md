# AssetEditing: gas

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 16 |
| `edit` | 16 |
| `save` | 16 |
| `readback_verify` | 16 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 16 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `ability_input_binding` | 1 | `asset_authoring.gas.ability_input_binding` | `testcases\ability_input_binding.json` |
| `ability_spec_builder` | 1 | `asset_authoring.gas.ability_spec_builder` | `testcases\ability_spec_builder.json` |
| `ability_tags_cost_triggers` | 1 | `asset_authoring.gas.ability_tags_cost_triggers` | `testcases\ability_tags_cost_triggers.json` |
| `asc_replication_mode` | 1 | `asset_authoring.gas.asc_replication_mode` | `testcases\asc_replication_mode.json` |
| `attribute_init_datatable` | 1 | `asset_authoring.gas.attribute_init_datatable` | `testcases\attribute_init_datatable.json` |
| `effect_modifier_crud` | 1 | `asset_authoring.gas.effect_modifier_crud` | `testcases\effect_modifier_crud.json` |
| `gameplay_ability` | 1 | `asset_authoring.gas.gameplay_ability` | `testcases\gameplay_ability.json` |
| `gameplay_ability_flags` | 1 | `asset_authoring.gas.gameplay_ability_flags` | `testcases\gameplay_ability_flags.json` |
| `gameplay_cue_effect_link` | 1 | `asset_authoring.gas.gameplay_cue_effect_link` | `testcases\gameplay_cue_effect_link.json` |
| `gameplay_cue_notify` | 1 | `asset_authoring.gas.gameplay_cue_notify` | `testcases\gameplay_cue_notify.json` |
| `gameplay_cue_unlink_lifecycle` | 1 | `asset_authoring.gas.gameplay_cue_unlink_lifecycle` | `testcases\gameplay_cue_unlink_lifecycle.json` |
| `gameplay_effect` | 1 | `asset_authoring.gas.gameplay_effect` | `testcases\gameplay_effect.json` |
| `gameplay_effect_spec_builder` | 1 | `asset_authoring.gas.gameplay_effect_spec_builder` | `testcases\gameplay_effect_spec_builder.json` |
| `gameplay_effect_template` | 1 | `asset_authoring.gas.gameplay_effect_template` | `testcases\gameplay_effect_template.json` |
| `target_actor` | 1 | `asset_authoring.gas.target_actor` | `testcases\target_actor.json` |
| `widget_attribute_binding` | 1 | `asset_authoring.gas.widget_attribute_binding` | `testcases\widget_attribute_binding.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.gas
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
