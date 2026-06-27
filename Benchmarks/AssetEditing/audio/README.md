# AssetEditing: audio

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 29 |
| `edit` | 29 |
| `save` | 29 |
| `readback_verify` | 29 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 29 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `routing` | 1 | `asset_authoring.audio.routing` | `testcases\routing.json` |
| `template_batch` | 1 | `asset_authoring.audio.template_batch` | `testcases\template_batch.json` |
| `metasound_explicit_connection` | 1 | `asset_authoring.audio.metasound_explicit_connection` | `testcases\metasound_explicit_connection.json` |
| `metasound_interactive_crossfade` | 1 | `asset_authoring.audio.metasound_interactive_crossfade` | `testcases\metasound_interactive_crossfade.json` |
| `metasound_looping_ambient_preset` | 1 | `asset_authoring.audio.metasound_looping_ambient_preset` | `testcases\metasound_looping_ambient_preset.json` |
| `metasound_oneshot_sfx` | 1 | `asset_authoring.audio.metasound_oneshot_sfx` | `testcases\metasound_oneshot_sfx.json` |
| `metasound_patch_io` | 1 | `asset_authoring.audio.metasound_patch_io` | `testcases\metasound_patch_io.json` |
| `metasound_source_graph` | 1 | `asset_authoring.audio.metasound_source_graph` | `testcases\metasound_source_graph.json` |
| `metasound_spec_node_edit` | 1 | `asset_authoring.audio.metasound_spec_node_edit` | `testcases\metasound_spec_node_edit.json` |
| `metasound_synth_tone` | 1 | `asset_authoring.audio.metasound_synth_tone` | `testcases\metasound_synth_tone.json` |
| `metasound_variable_state` | 1 | `asset_authoring.audio.metasound_variable_state` | `testcases\metasound_variable_state.json` |
| `sound_class_mix_ducking` | 1 | `asset_authoring.audio.sound_class_mix_ducking` | `testcases\sound_class_mix_ducking.json` |
| `sound_concurrency` | 1 | `asset_authoring.audio.sound_concurrency` | `testcases\sound_concurrency.json` |
| `sound_cue` | 1 | `asset_authoring.audio.sound_cue` | `testcases\sound_cue.json` |
| `sound_cue_delete` | 1 | `asset_authoring.audio.sound_cue_delete` | `testcases\sound_cue_delete.json` |
| `sound_cue_distance_crossfade` | 1 | `asset_authoring.audio.sound_cue_distance_crossfade` | `testcases\sound_cue_distance_crossfade.json` |
| `sound_cue_graph_primitives` | 1 | `asset_authoring.audio.sound_cue_graph_primitives` | `testcases\sound_cue_graph_primitives.json` |
| `sound_cue_layered_template` | 1 | `asset_authoring.audio.sound_cue_layered_template` | `testcases\sound_cue_layered_template.json` |
| `sound_cue_looping_ambient` | 1 | `asset_authoring.audio.sound_cue_looping_ambient` | `testcases\sound_cue_looping_ambient.json` |
| `sound_cue_random_template` | 1 | `asset_authoring.audio.sound_cue_random_template` | `testcases\sound_cue_random_template.json` |
| `sound_cue_spec` | 1 | `asset_authoring.audio.sound_cue_spec` | `testcases\sound_cue_spec.json` |
| `sound_cue_switch_duplicate` | 1 | `asset_authoring.audio.sound_cue_switch_duplicate` | `testcases\sound_cue_switch_duplicate.json` |
| `sound_perception_binding` | 1 | `asset_authoring.audio.sound_perception_binding` | `testcases\sound_perception_binding.json` |
| `sound_perception_unbind` | 1 | `asset_authoring.audio.sound_perception_unbind` | `testcases\sound_perception_unbind.json` |
| `sound_wave_batch_maintenance` | 1 | `asset_authoring.audio.sound_wave_batch_maintenance` | `testcases\sound_wave_batch_maintenance.json` |
| `sound_wave_batch_properties` | 1 | `asset_authoring.audio.sound_wave_batch_properties` | `testcases\sound_wave_batch_properties.json` |
| `sound_wave_batch_routing` | 1 | `asset_authoring.audio.sound_wave_batch_routing` | `testcases\sound_wave_batch_routing.json` |
| `sound_wave_virtualization_looping` | 1 | `asset_authoring.audio.sound_wave_virtualization_looping` | `testcases\sound_wave_virtualization_looping.json` |
| `submix_hierarchy` | 1 | `asset_authoring.audio.submix_hierarchy` | `testcases\submix_hierarchy.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.audio
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
