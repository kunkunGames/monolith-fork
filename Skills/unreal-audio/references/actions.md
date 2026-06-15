# unreal-audio — Action Reference

These per-action signatures are a snapshot of the live `audio` catalog (98 actions). Confirm the live action set and an action's exact parameter schema before calling — `monolith_discover({ namespace: "audio" })` and `describe_query("action_schema", { namespace: "audio", action: "<name>" })` (mode schema) are the authority, never call from this snapshot alone.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates the project (transaction-wrapped).

### Asset CRUD (15)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `[w] create_sound_attenuation` | `asset_path*`, `settings?` (object) | Create USoundAttenuation with optional settings |
| `get_attenuation_settings` | `asset_path*` | Read all FSoundAttenuationSettings fields |
| `[w] set_attenuation_settings` | `asset_path*`, `settings*` (object) | Partial update any attenuation fields |
| `[w] create_sound_class` | `asset_path*`, `parent_class?`, `properties?` (object) | Create USoundClass with hierarchy |
| `get_sound_class_properties` | `asset_path*` | Read FSoundClassProperties + parent + children |
| `[w] set_sound_class_properties` | `asset_path*`, `properties?` (object), `parent_class?` (empty to clear) | Update class properties |
| `[w] create_sound_mix` | `asset_path*`, `eq_settings?` (object), `class_effects?` (array of FSoundClassAdjuster), `initial_delay?`, `fade_in_time?`, `duration?`, `fade_out_time?` | Create USoundMix with EQ + adjusters |
| `get_sound_mix_settings` | `asset_path*` | Read EQ bands, class adjusters, timing |
| `[w] set_sound_mix_settings` | `asset_path*`, `eq_settings?` (object), `class_effects?` (array, replaces), `initial_delay?`, `fade_in_time?`, `duration?`, `fade_out_time?` | Update mix |
| `[w] create_sound_concurrency` | `asset_path*`, `settings?` (object) | Create USoundConcurrency |
| `get_concurrency_settings` | `asset_path*` | Read MaxCount, ResolutionRule, ducking |
| `[w] set_concurrency_settings` | `asset_path*`, `settings*` (object) | Update concurrency |
| `[w] create_sound_submix` | `asset_path*`, `parent_submix?` | Create USoundSubmix (no effect_chain param — set via set_submix_properties) |
| `get_submix_properties` | `asset_path*` | Read effect chain, volume, hierarchy |
| `[w] set_submix_properties` | `asset_path*`, `properties*` (object) | Update submix |
| `[w] create_test_wave` | `asset_path*` (alias `path`), `frequency_hz=440.0` (20-20000), `duration_seconds=0.5` (0.05-5.0), `sample_rate=44100` (22050/44100/48000), `amplitude=0.5` ((0,1]) | Synthesize a 16-bit mono sine USoundWave for tests |

### Query & Search (10)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `list_audio_assets` | `type*` (SoundWave/SoundCue/MetaSoundSource/SoundClass/SoundAttenuation/SoundSubmix/SoundConcurrency/SoundMix/All), `path_filter?`, `limit=100` | List by type |
| `search_audio_assets` | `query*`, `type?`, `limit=50` | Name-substring search (de-dup by path — see issue #5) |
| `get_sound_wave_info` | `asset_path*` | Duration, channels, sample rate, compression, class, attenuation |
| `get_sound_class_hierarchy` | `root_class?` (omit = all roots) | Recursive tree traversal |
| `get_submix_hierarchy` | `root_submix?` (omit = all roots) | Routing tree with effect chains |
| `find_audio_references` | `asset_path*` | Bidirectional reference scan |
| `find_unused_audio` | `type=All`, `path_filter?`, `limit=100` | Zero-reference audio assets |
| `find_sounds_without_class` | `path_filter?`, `limit=100` | Unassigned SoundBases |
| `find_unattenuated_sounds` | `path_filter?`, `limit=100` | Missing attenuation |
| `get_audio_stats` | _(none)_ | Counts by type, sizes, compression breakdown |

### Batch Operations (10)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `batch_assign_sound_class` | `asset_paths[]`, `sound_class` | Set class on N assets |
| `batch_assign_attenuation` | `asset_paths[]`, `attenuation` | Set attenuation on N assets |
| `batch_set_compression` | `asset_paths[]`, `quality?`, `type?` | Set compression on N SoundWaves |
| `batch_set_submix` | `asset_paths[]`, `submix` | Set submix on N assets |
| `batch_set_concurrency` | `asset_paths[]`, `concurrency` | Set concurrency on N assets |
| `batch_set_looping` | `asset_paths[]`, `looping` | Set looping flag |
| `batch_set_virtualization` | `asset_paths[]`, `mode` | Restart/PlayWhenSilent/Disabled |
| `batch_rename_audio` | `asset_paths[]`, `prefix?`, `suffix?`, `find?`, `replace?` | Rename with patterns |
| `batch_set_sound_wave_properties` | `asset_paths[]`, `properties` | Multi-property reflection set |
| `apply_audio_template` | `asset_paths[]`, `template` | Apply class+attenuation+compression+submix+concurrency in one pass |

### Sound Cue Graph (21)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_sound_cue` | `asset_path`, `sound_waves?[]` | Create cue, auto-Random if multiple waves |
| `get_sound_cue_graph` | `asset_path` | JSON: nodes[], connections[], first_node |
| `add_sound_cue_node` | `asset_path`, `node_type`, `node_id`, `properties?` | Add any of 22 node types |
| `remove_sound_cue_node` | `asset_path`, `node_id` | Remove node |
| `connect_sound_cue_nodes` | `asset_path`, `from_node_id`, `to_node_id`, `child_index?` | Wire nodes |
| `set_sound_cue_first_node` | `asset_path`, `node_id` | Set root output |
| `set_sound_cue_node_property` | `asset_path`, `node_id`, `property_name`, `value` | Set any node property |
| `list_sound_cue_node_types` | — | All 22 types with max_children |
| `find_sound_waves_in_cue` | `asset_path` | All WavePlayer references |
| `validate_sound_cue` | `asset_path` | Check for issues |
| `build_sound_cue_from_spec` | `asset_path`, `spec` | **Crown jewel** — declarative JSON graph |
| `create_random_sound_cue` | `asset_path`, `sound_waves[]`, `weights?[]` | Template: Random node |
| `create_layered_sound_cue` | `asset_path`, `sound_waves[]`, `volumes?[]` | Template: Mixer node |
| `create_looping_ambient_cue` | `asset_path`, `sound_waves[]`, `delay_min?`, `delay_max?` | Template: Loop+Random+Delay |
| `create_distance_crossfade_cue` | `asset_path`, `bands[]` | Template: Distance crossfade |
| `create_switch_sound_cue` | `asset_path`, `parameter_name`, `sound_waves[]` | Template: Switch node |
| `duplicate_sound_cue` | `source_path`, `dest_path` | Clone |
| `delete_audio_asset` | `asset_path` | Delete any audio asset |
| `preview_sound` | `asset_path` | Editor playback |
| `stop_preview` | — | Stop preview |
| `get_sound_cue_duration` | `asset_path` | Cached duration |

### MetaSound Graph (25, requires WITH_METASOUND)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_metasound_source` | `asset_path`, `format?`, `one_shot?` | Create via Builder API |
| `create_metasound_patch` | `asset_path` | Create reusable subgraph |
| `add_metasound_node` | `asset_path`, `node_class` [NS, Name, Variant] | Add from 76+ node types |
| `remove_metasound_node` | `asset_path`, `node_id_or_handle` | Remove node |
| `connect_metasound_nodes` | `asset_path`, `from_node`, `from_output`, `to_node`, `to_input` | Wire by name |
| `disconnect_metasound_nodes` | `asset_path`, `from_node`, `from_output`, `to_node`, `to_input` | Disconnect |
| `add_metasound_input` | `asset_path`, `name`, `data_type`, `default_value?` | Graph-level input |
| `add_metasound_output` | `asset_path`, `name`, `data_type` | Graph-level output |
| `set_metasound_input_default` | `asset_path`, `input_name`, `value` | Set default |
| `add_metasound_interface` | `asset_path`, `interface_name` | UE.Source, UE.OutputFormat.Mono, etc |
| `get_metasound_graph` | `asset_path` | JSON: nodes, edges, inputs, outputs |
| `list_metasound_connections` | `asset_path` | All edges |
| `list_available_metasound_nodes` | `filter?` | Enumerate registered node classes |
| `get_metasound_node_info` | `node_class` | Inputs/outputs/types for a class |
| `find_metasound_node_inputs` | `asset_path`, `node_id_or_handle` | Node inputs |
| `find_metasound_node_outputs` | `asset_path`, `node_id_or_handle` | Node outputs |
| `get_metasound_input_names` | `asset_path` | Graph inputs with types/defaults |
| `build_metasound_from_spec` | `asset_path`, `spec` | **Crown jewel** — declarative JSON graph |
| `create_metasound_preset` | `asset_path`, `reference_metasound` | Preset from existing |
| `create_oneshot_sfx` | `asset_path`, `sound_wave` | Template: WavePlayer -> Out |
| `create_looping_ambient_metasound` | `asset_path`, `sound_wave` | Template: LFO modulated |
| `create_synthesized_tone` | `asset_path`, `oscillator_type?` | Template: Osc->Filter->ADSR |
| `create_interactive_metasound` | `asset_path`, `sound_waves[]`, `parameter_name` | Template: Crossfade/Switch |
| `add_metasound_variable` | `asset_path`, `name`, `data_type`, `default_value?` | Graph variable |
| `set_metasound_node_location` | `asset_path`, `node_id_or_handle`, `x`, `y` | Editor layout |
