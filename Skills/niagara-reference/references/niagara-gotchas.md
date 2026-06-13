# niagara-reference — Niagara gotchas

Subtle Niagara traps that silently break effects or tank performance. Confirm any
live action params with `monolith_discover({ namespace: "niagara", action: "<action>", mode: "schema" })`.

## Module execution order

- Module order within a stage matters. The reliable ordering is:
  Position -> SolveForcesAndVelocity -> Collision/Readers -> Visual.
- Modules run top-to-bottom; a module that reads an attribute must run AFTER the
  module that writes it. Reordering with `move_module` preserves input overrides.
- Stages, in evaluation order: `Emitter Spawn`, `Emitter Update`, `Particle Spawn`,
  `Particle Update`, `Render`. `get_ordered_modules` returns a `usage` field per
  module so you can confirm which stage a module actually lives in.

## GPU sim limitations

GPU compatibility rules are enforced by the engine — violating them produces
errors/warnings that block or break the effect. Run `validate_system` before compile.

- **Fixed bounds REQUIRED.** GPU emitters can't read back particle positions for
  dynamic bounds. Set fixed bounds (`set_fixed_bounds` / `set_emitter_property`).
  This is also the #1 Niagara optimization in general.
- **Light Renderer is CPU only.** It needs CPU readback of particle positions for
  light placement. GPU alternative: Component Renderer + PointLight.
- **Ribbon Renderer is CPU only.** Not compatible with GPU sim.
- **Events are CPU only.** GPU alternative: Attribute Reader.
- **Data Channel Write is CPU only.** GPU can only read channels.
- **Skeletal Mesh GPU sampling is D3D12 only.** Crashes on Vulkan.
- **Best on GPU:** high counts (1,000+), simple behavior — fire/sparks/debris.
- **Common pitfall:** fire+light. Do NOT put a Light Renderer on a GPU emitter.
  Split it: fire sprites on a GPU emitter (SpriteRenderer), dynamic light on a
  separate low-spawn-rate CPU emitter (1-3 Light Renderer particles).

## Attribute Reader ordering

- The Attribute Reader is the GPU-friendly replacement for events, but it has a
  hard ordering rule: **the source emitter must execute BEFORE the reader.**
- Emitters execute top-to-bottom, so place the source emitter ABOVE the reading
  emitter in the system's emitter list (`reorder_emitters`). A reader above its
  source reads stale or zeroed data.
- Reads are previous-frame in spirit: like GPU resource access, a reader sees the
  source's committed state, not its current-frame writes.

## Map For — CPU bug

- **The Map For node is GPU only. It does NOT work on CPU.** A Map For loop placed
  on a CPU emitter silently fails to iterate — no error, just no effect. If you
  need the per-element loop on CPU, restructure the logic instead of relying on
  Map For, or move the emitter to GPU sim.

## Static switch permutations

- Each static switch input multiplies shader/script permutations. Keep them lean —
  the same discipline as master materials (treat ~6 switches as a ceiling).
- Set switches with `set_static_switch_value` (bool true/false, enum value name,
  int number); read with `get_static_switch_value` (omit `input` to list all).
- Switches are compile-time: changing one forces a recompile and a new permutation,
  so prefer a dynamic input or user parameter when the value must vary at runtime.

## Deterministic random

- Niagara randomness is seeded. For reproducible effects set system determinism and
  a fixed seed: `set_system_property` with `bDeterminism` and `RandomSeed`
  (snake_case aliases `determinism` / `random_seed` are accepted; read back with
  `get_system_property`).
- Without determinism, two instances of the same system diverge frame-to-frame —
  fine for ambient variety, wrong for gameplay-synced or networked effects that
  must match across machines.

## Warm-up spikes

- Warm-up advances the simulation before the first visible frame so an effect (e.g.
  smoke, fog) appears already-running. The cost is a one-time spike: warm-up
  simulates many ticks in a single frame.
- Configure with `set_warmup_profile` (`warmup_time`, optional `warmup_tick_delta`);
  it returns the engine-resolved `(time, count, delta)` triple so you can observe
  the `ResolveWarmupTickCount` snap. Read current values with `get_system_timing`.
- Keep `WarmupTime` only as large as the effect needs — a long warm-up with a small
  tick delta means a high tick count and a worse hitch on spawn. Combine with
  pooling (`MaxPoolSize`) so the spike is paid at load, not at gameplay spawn.

## Parameter naming

- **No spaces in parameter names.** Spaces break HLSL codegen and scripting. Use
  PascalCase / underscores instead.
- The primary asset param across actions is `asset_path`, NOT `system` or `asset`.
- Module actions need `module_node`, a GUID from `get_ordered_modules` — never the
  module display name.
- Parameter actions accept the `User.` prefix (e.g. `User.MyParam`) as well as bare
  names. User parameters are the main Blueprint/C++ control surface.
- **`rename_user_parameter` does NOT update HLSL string references.** It rewrites
  module bindings but raw string refs inside Custom HLSL stay pointing at the old
  name — fix those by hand.
- In Custom HLSL use bare input/output names (`InColor`, not `Module.InColor`); the
  compiler generates `In_X` / `Out_X` internally. You can't swizzle ParameterMap
  variables directly (`Particles.Color.xyz` is one token) — copy to a local first:
  `float4 C = Particles.Color;`.

## Other silent traps

- **Emitter display name vs handle ID.** `list_renderers`, `get_ordered_modules`,
  and `get_renderer_bindings` may fail with "Emitter not found" when given the
  display name (e.g. `Fire`) instead of the handle ID (e.g. `Emitter_0`). Call
  `list_emitters` first; if the display name fails, try the handle ID.
- **`UseVelDistribution=true` ignores the Velocity vector.** Speed comes from
  `Velocity Speed` instead. Set `UseVelDistribution=false` to use a direct vector.
- **`set_curve_value` is for inline float curves only.** For DataInterface curve
  inputs (e.g. `NiagaraDataInterfaceCurve`) use `set_module_input_di` /
  `configure_curve_keys` instead.
- **Additive materials don't need sorting** — set sort mode to None for free perf.
- **One big emitter beats many small ones.** 1 emitter x 1,000 particles outperforms
  10 emitters x 100 particles; GPU sim has fixed per-emitter overhead.
- **Dynamic Parameters cap material comms** at 4 slots x 4 channels = 16 floats.
