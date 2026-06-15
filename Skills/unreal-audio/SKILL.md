---
name: unreal-audio
description: Use when creating, editing, or inspecting Unreal Engine audio ASSETS via the Monolith MCP audio namespace. Covers Sound Cues, MetaSound graphs, attenuation, sound classes, sound mixes, submixes, concurrency, audio buses, batch ops, and templates. For analyzing acoustics/reverb/RT60/footstep propagation as a level-design pass use unreal-leveldesign; this skill authors the audio ASSET. Triggers on audio, sound, SFX, music, sound wave, metasound, MetaSound graph, sound cue, audio component, attenuation, spatialization, occlusion, submix, audio bus, sound mix, sound class, concurrency, ambient sound, reverb.
---

# Unreal Audio Workflows

Authors and inspects Unreal audio **assets** through the Monolith `audio` namespace via `audio_query()`. The live namespace has 98 actions; the curated set below is a snapshot — discover the live catalog and schemas first, never call from memory.

```
monolith_discover({ namespace: "audio" })
describe_query("action_schema", { namespace: "audio", action: "build_metasound_from_spec" })
```

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates the project (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with mode schema (`describe_query("action_schema", {namespace:"audio", action:"<name>"})`).

## When to use / Use a different skill for

- **Use this skill** to create or edit audio assets — Sound Cues, MetaSounds, attenuation, sound classes, sound mixes, submixes, concurrency, and to batch-configure imported `SoundWave` assets.
- **Use unreal-leveldesign instead** when the task is analyzing acoustics, reverb/RT60, sound propagation, or footstep/stealth audio as a level-design pass rather than authoring an audio asset.
- **Use unreal-niagara instead** when the "effect" is a visual VFX particle effect rather than a sound.

## Key Parameters

- `asset_path` -- audio asset path (e.g. `/Game/Audio/SA_MyAttenuation`)
- `asset_paths` -- array of paths for batch operations
- `spec` -- JSON graph spec for `build_sound_cue_from_spec` / `build_metasound_from_spec`

## Action Reference

Full per-action parameter signatures — grouped by the 5 categories (Asset CRUD 15, Query & Search 10, Batch Operations 10, Sound Cue Graph 21, MetaSound Graph 25) — live in [`references/actions.md`](references/actions.md). The Discovery block above stays the authority: confirm the live catalog and an action's exact schema via `monolith_discover` with mode schema before calling.

## Common Workflows

### Create randomized footstep Sound Cue
```
audio_query("create_random_sound_cue", {
  "asset_path": "/Game/Audio/SC_Footstep_Concrete",
  "sound_waves": ["/Game/Audio/Waves/Concrete_01", "/Game/Audio/Waves/Concrete_02", "/Game/Audio/Waves/Concrete_03"],
  "weights": [1.0, 1.0, 0.5]
})
```

### Build MetaSound from spec
```
audio_query("build_metasound_from_spec", {
  "asset_path": "/Game/Audio/MS_Wind",
  "spec": {
    "type": "Source", "format": "Mono", "one_shot": false,
    "interfaces": ["UE.Source", "UE.OutputFormat.Mono"],
    "inputs": [{"name": "Intensity", "type": "Float", "default": 0.5}],
    "nodes": [
      {"id": "player", "class": ["UE", "Wave Player", "Mono"]},
      {"id": "gain", "class": ["UE", "Multiply", "Audio Float"]}
    ],
    "connections": [
      {"from": "player", "output": "Audio", "to": "gain", "input": "Audio"}
    ],
    "graph_input_connections": [
      {"input": "Intensity", "to_node": "gain", "to_pin": "Float"}
    ]
  }
})
```

### Batch configure imported sounds
```
audio_query("apply_audio_template", {
  "asset_paths": ["/Game/Audio/Waves/Gun_01", "/Game/Audio/Waves/Gun_02"],
  "template": {
    "sound_class": "/Game/Audio/Classes/SC_SFX",
    "attenuation": "/Game/Audio/Attenuation/SA_Medium",
    "compression": {"quality": 60, "type": "BinkAudio"}
  }
})
```

## Gotchas

- `USoundWave::CompressionQuality` and `SoundAssetCompressionType` are **private** -- use reflection, not direct access
- `EVirtualizationMode` values: `Restart`, `PlayWhenSilent`, `Disabled` (NOT `Pause`)
- MetaSound `AddGraphInputNode` returns `FMetaSoundBuilderNodeOutputHandle` (counterintuitive!)
- `UMetaSoundEditorSubsystem` accessed via `GEditor->GetEditorSubsystem<>()`, NOT `::Get()`
- Sound Cues need `LinkGraphNodesFromSoundNodes()` + `CacheAggregateValues()` after ANY graph mutation
- MetaSound data types: Float, Int32, Bool, String, Trigger, Time, Audio, WaveAsset
- MetaSound interfaces: "UE.Source", "UE.Source.OneShot", "UE.OutputFormat.Mono", "UE.OutputFormat.Stereo"

## 2026-04-25 Test Pass — Discovered Quirks

These were surfaced by the Phase 1-5 audio test pass and form the source-of-truth recipes for future audio agents until the underlying actions are fixed.

### `search_audio_assets` returns duplicates (issue #5)

- The result list is NOT de-duplicated. Identical asset paths can appear multiple times when an asset matches both name and class filters internally.
- **How to apply:** always post-filter unique by `asset_path` before iterating: build a `Set` keyed on path, OR prefer `list_audio_assets` + client-side substring match when you need a clean enumeration.
- Counts in the response (`total`, `count`) reflect the duplicated array length — do NOT use them for "how many distinct assets matched".

### `add_metasound_interface` has no discovery action (issue #8)

There is no `list_metasound_interfaces` action. Use this canonical list — verified registered in UE 5.7 MetaSound subsystem:

| Interface name | Purpose |
|---|---|
| `UE.Source` | Base Source interface (OnPlay/OnFinished triggers, IsFinished output) |
| `UE.Source.OneShot` | One-shot variant — auto-finishes when audio completes |
| `UE.OutputFormat.Mono` | 1-channel audio output (`Out Mono` pin) |
| `UE.OutputFormat.Stereo` | 2-channel audio output (`Out Left` + `Out Right` pins) |
| `UE.OutputFormat.Quad` | 4-channel surround |
| `UE.OutputFormat.5dot1` | 5.1 surround |
| `UE.OutputFormat.7dot1` | 7.1 surround |
| `UE.Attenuation` | Adds Distance + WorldPosition system inputs (auto-driven by attenuation) |
| `UE.Spatialization` | Adds Azimuth/Elevation system inputs |
| `UE.SourceOrientation` | Adds source forward/up/right vectors |
| `UE.AudioParameterInterfaces.*` | Project-specific parameter interfaces — use `find_metasound_node_inputs` on a known-good asset to enumerate |

**Required combinations:**
- Source assets need at minimum `UE.Source` + ONE `UE.OutputFormat.*`.
- For one-shot SFX: `UE.Source.OneShot` + `UE.OutputFormat.Mono` (or Stereo).
- Loops/ambient: `UE.Source` (no OneShot) + format.

**Discovery fallback:** open any shipped MetaSound in the editor, copy the `Interfaces` array from its asset details, then mirror via `add_metasound_interface`. There's no MCP enumeration today.

### Pin-name discoverability for sound cue / metasound nodes (issue #9)

Pin names don't appear in `list_sound_cue_node_types` or `list_available_metasound_nodes`. Recipe per node class:

**Sound Cue nodes** — use `connect_sound_cue_nodes` with `child_index` (integer, NOT pin name). All cue nodes have implicit "input" + indexed "output" children. Children indexes in document order:

| Node class | Pin convention |
|---|---|
| `Mixer`, `Random`, `Concatenator`, `Looping`, `Doppler`, `Switch` | `child_index: 0..N-1` for inputs |
| `Modulator`, `Attenuation`, `LowPassFilter`, `Branch`, `Delay`, `Enveloper` | `child_index: 0` (single input) |
| `WavePlayer`, `Dialogue Player` | terminal — no children, only output |
| `Mature` | `child_index: 0` (mature) or `1` (non-mature) |

For cue nodes, prefer `build_sound_cue_from_spec` — it handles wiring via node IDs and avoids the index trap entirely.

**MetaSound nodes** — pin names ARE discoverable via:
- `find_metasound_node_inputs(asset_path, node_id)` — returns all inputs with their exact pin names + types
- `find_metasound_node_outputs(asset_path, node_id)` — same for outputs
- `get_metasound_node_info(node_class)` — class-level pin schema BEFORE adding to graph

**Common MetaSound pin names** (verified):

| Node class | Inputs | Outputs |
|---|---|---|
| `["UE", "Wave Player", "Mono"]` | `Wave Asset`, `Start Time`, `Pitch Shift`, `Loop`, `Loop Start`, `Loop Duration`, `Play`, `Stop` | `Audio`, `On Play`, `On Finished`, `On Looped`, `On Cue Point` |
| `["UE", "Wave Player", "Stereo"]` | (as Mono) | `Audio L`, `Audio R`, `On Play`, `On Finished`, `On Looped`, `On Cue Point` |
| `["UE", "Multiply", "Audio Float"]` | `Audio`, `Float` | `Out` |
| `["UE", "Add", "Audio"]` | `In Audio 0`, `In Audio 1` | `Out Audio` |
| `["UE", "ADSR Envelope (Audio)", "Audio"]` | `Trigger Attack`, `Trigger Release`, `Attack Time`, `Decay Time`, `Sustain Level`, `Release Time` | `Out Envelope`, `On Attack Triggered`, `On Done` |
| `["UE", "LFO", "LFO"]` | `Frequency`, `Amplitude`, `Offset`, `Pulse Width`, `Type`, `Reset` | `Out` |
| `["UE", "Sine Oscillator (Audio)", "Audio"]` | `Frequency`, `Phase Offset`, `Glide Factor`, `Sync` | `Audio` |
| `["UE", "BiQuad Filter", "Audio"]` | `In`, `Cutoff Frequency`, `Bandwidth`, `Gain`, `Type` | `Out` |

**Rule:** before connecting MetaSound nodes blind, ALWAYS call `find_metasound_node_inputs`/`outputs` on the just-added node — pin names sometimes drift between UE patch versions.

### Batch action conventions: `asset_paths` array, not `asset_path` (issue #10)

Every action prefixed `batch_*` AND `apply_audio_template` takes an ARRAY field named `asset_paths` (plural). Passing a single `asset_path` (singular) string returns a generic "missing required field" error.

**The 11 batch-style actions** (all require `asset_paths: [...]`):

`batch_assign_sound_class`, `batch_assign_attenuation`, `batch_set_compression`, `batch_set_submix`, `batch_set_concurrency`, `batch_set_looping`, `batch_set_virtualization`, `batch_rename_audio`, `batch_set_sound_wave_properties`, `apply_audio_template`, plus any other action with `batch_` prefix.

**Single-asset wrapper recipe:** even for a single asset, wrap it: `{"asset_paths": ["/Game/Audio/MyWave"], ...}`. Cleaner than juggling two code paths.

**Inverse rule:** every NON-batch audio action takes singular `asset_path` (string). The naming distinction is load-bearing — never mix.

### Sound Cue node IDs re-index after `remove_sound_cue_node` (issue #11)

`remove_sound_cue_node` reorders the surviving nodes in `get_sound_cue_graph` output. Node IDs returned by an earlier `get_sound_cue_graph` call are STALE after any `remove_sound_cue_node`.

**How to apply:**
- After every `remove_sound_cue_node`, immediately re-call `get_sound_cue_graph(asset_path)` and rebuild your node-ID map. Discard cached IDs.
- For multi-step graph edits, prefer `build_sound_cue_from_spec` — it operates on declarative node IDs and recomputes the graph atomically.
- If you must script a series of removals: walk in REVERSE order so earlier IDs stay valid through the loop, OR re-fetch between each removal.
- This does NOT apply to MetaSound — MetaSound nodes use stable handles (`node_id_or_handle`) that survive removals. Sound Cue uses array-index IDs; MetaSound uses GUID-style handles.


