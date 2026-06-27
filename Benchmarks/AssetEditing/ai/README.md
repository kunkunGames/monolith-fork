# AssetEditing: ai

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 14 |
| `edit` | 14 |
| `save` | 14 |
| `readback_verify` | 14 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 14 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `controller_perception` | 1 | `asset_authoring.ai.controller_perception` | `testcases\controller_perception.json` |
| `behavior_tree` | 1 | `asset_authoring.ai.behavior_tree` | `testcases\behavior_tree.json` |
| `behavior_tree_granular_edit` | 1 | `asset_authoring.ai.behavior_tree_granular_edit` | `testcases\behavior_tree_granular_edit.json` |
| `behavior_tree_node_blueprints` | 1 | `asset_authoring.ai.behavior_tree_node_blueprints` | `testcases\behavior_tree_node_blueprints.json` |
| `behavior_tree_spec` | 1 | `asset_authoring.ai.behavior_tree_spec` | `testcases\behavior_tree_spec.json` |
| `blackboard_inheritance_duplicate` | 1 | `asset_authoring.ai.blackboard_inheritance_duplicate` | `testcases\blackboard_inheritance_duplicate.json` |
| `blackboard_set_parent` | 1 | `asset_authoring.ai.blackboard_set_parent` | `testcases\blackboard_set_parent.json` |
| `eqs_query` | 1 | `asset_authoring.ai.eqs_query` | `testcases\eqs_query.json` |
| `eqs_query_mutation` | 1 | `asset_authoring.ai.eqs_query_mutation` | `testcases\eqs_query_mutation.json` |
| `eqs_spec_builder` | 1 | `asset_authoring.ai.eqs_spec_builder` | `testcases\eqs_spec_builder.json` |
| `mass_entity_config` | 1 | `asset_authoring.ai.mass_entity_config` | `testcases\mass_entity_config.json` |
| `smart_object_behavior_duplicate` | 1 | `asset_authoring.ai.smart_object_behavior_duplicate` | `testcases\smart_object_behavior_duplicate.json` |
| `smart_object_definition` | 1 | `asset_authoring.ai.smart_object_definition` | `testcases\smart_object_definition.json` |
| `state_tree` | 1 | `asset_authoring.ai.state_tree` | `testcases\state_tree.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.ai
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
