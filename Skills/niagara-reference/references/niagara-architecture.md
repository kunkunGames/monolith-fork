# Niagara Architecture Reference

Sim-target choice, lifecycle, parent-child communication, data interfaces, system archetypes, warm-up/pooling.

## CPU vs GPU Decision Tree

| Question | CPU Sim (`CPUSim`) | GPU Sim (`GPUCompute Sim`) |
|----------|--------------------|----------------------------|
| Particle count? | < 100 particles | > 1,000 particles (test in between) |
| Needs Light Renderer? | Supported | NOT compatible — Light Renderer needs CPU readback for light placement |
| Needs Ribbon Renderer? | Supported | NOT compatible |
| Bounds mode? | `Dynamic` (default) allowed | MUST be `Fixed` — GPU can't read back positions for dynamic bounds |
| Particle events? | Supported (events are CPU only) | Use Attribute Reader instead |
| Data Channel writes? | Read + write | Read only |
| Best for | Complex behavior, low counts, smoke/fog, Light/Ribbon | High counts, simple behavior, fire/sparks/debris |

- **1 emitter x 1,000 particles > 10 emitters x 100 particles** — GPU sim has fixed per-emitter overhead.
- **Map For node, Data Channel Write: GPU only / CPU only respectively** — Map For does NOT work on CPU; Data Channel Write does NOT work on GPU.
- **Skeletal Mesh GPU sampling: D3D12 only** — crashes on Vulkan.
- **Fire + light pitfall:** never put a Light Renderer on a GPU emitter. Split it: fire sprites on a GPU emitter with SpriteRenderer, dynamic light on a separate CPU emitter with Light Renderer (low spawn rate, 1-3 lights).

## Lightweight (Stateless) Emitters

`UNiagaraStatelessEmitter` (Lightweight Emitter, UE 5.4+) is a standalone storage class distinct from `UNiagaraEmitter`. It lives **outside** the Niagara System wrapper — pass the asset path directly.

- **Zero tick cost** for simple ambient effects (no per-frame simulation; precomputed/analytic motion).
- No system wrapper needed: `create_stateless_emitter` produces a standalone asset.
- Loop config writes through reflection against the protected `EmitterState` (`FNiagaraEmitterStateData`) UPROPERTY; stateless-aware actions report `stateless: true`.
- `loop_duration_mode` (`Fixed`/`Infinite`, maps to `ENiagaraLoopDurationMode`) is stateless-only — the stateful `EmitterState` module has no equivalent.

## Lifecycle Patterns (Module Stages)

Stage order: `Emitter Spawn` -> `Emitter Update` -> `Particle Spawn` -> `Particle Update` -> `Render`.

- **Module execution order matters within a stage:** Position -> SolveForcesAndVelocity -> Collision/Readers -> Visual.
- **Spawn vs Update:** Spawn modules run once per particle at birth (InitializeParticle, initial Position/Velocity/Lifetime); Update modules run every frame (forces, drag, color/scale over life, collision).
- **Emitter-stage modules** (Spawn Rate, Spawn Burst, EmitterState loop topology) drive how/when particles are emitted, not per-particle behavior.
- **`get_ordered_modules`** returns each module's `usage` stage field — no separate stage query is needed.

## Parent-Child / Inter-Emitter Communication

Three mechanisms to pass data between emitters:

| Mechanism | Direction / Constraint | Notes |
|-----------|------------------------|-------|
| **Events** | CPU only | Inter-emitter event handler links a source emitter to a receiver. `add_event_handler` creates only the handler + `ParticleEventScript` container; you must add the matching `Receive<Event>` module to the `particle_event` script and set payload fields (e.g. `Position`) to `Apply`. Source emitter cannot be unresolved. |
| **Attribute Reader** | CPU + GPU | GPU alternative to events. **Source emitter must execute BEFORE the reader** — order emitters top-to-bottom so the source is above the reader. |
| **Data Channels** | Write = CPU only; GPU can read | Decoupled bus for cross-system / cross-emitter data. GPU emitters can only read a channel, not write it. |

## Data Interfaces (DI)

Data Interfaces expose external data/functions into the simulation graph (curves, meshes, grids, particle reads, parameter collections).

- Set a DI on a module input with `set_module_input_di`; `di_class` accepts `UNiagaraDataInterfaceCurve` or `NiagaraDataInterfaceCurve` (U prefix optional / auto-resolved).
- Inspect a DI class with `get_di_functions` / `get_di_properties` (CDO reflection) before wiring.
- **Curve DIs:** for `NiagaraDataInterfaceCurve` inputs use `set_module_input_di` (or `configure_curve_keys`), NOT `set_curve_value` — `set_curve_value` is only for inline float curves.
- **Niagara Parameter Collections (NPC):** namespaced parameter assets shared across many systems (`create_npc`, `add_npc_parameter`, `set_npc_default`).
- **Grid / NeighborGrid3D / ParticleRead DIs** feed Custom HLSL: when a Grid3D input is passed, GPU sampling APIs like `SamplePreviousGridVector3Value` become available; `SamplePrevious*` reads previous-frame state, not current-frame writes.

## System Archetypes

- **Single-emitter burst:** one emitter, `loop_behavior: Once`, Spawn Burst — impacts, muzzle flash, pickups. Fire-and-forget.
- **Looping ambient:** `loop_behavior: Infinite`, steady Spawn Rate — torches, embers, fog. Prefer a Lightweight emitter when behavior is simple (zero tick).
- **Multi-emitter composite:** several emitters layered (e.g. fire = core + flames + smoke + embers + heat haze). Keep emitter count low; one big emitter beats many small ones.
- **Event-driven:** source emitter raises Death/Location events; a child emitter spawns from them (CPU), or an Attribute Reader child samples the source (CPU/GPU).
- **Lit effect:** GPU sprite emitter for the visual + a low-count CPU Light Renderer emitter for the dynamic light.

## Warm-up & Pooling

- **Warm-up** advances the system at spawn so it appears mid-flight rather than empty (steady-state ambient effects). Configure via `set_warmup_profile` (`warmup_time`, optional `warmup_tick_delta`); the engine snaps to a resolved `(time, count, delta)` triple via `ResolveWarmupTickCount`.
- **Warm-up spikes:** large `WarmupTime` with a small tick delta means many simulated ticks at spawn — a one-frame cost spike. Use a coarser `warmup_tick_delta` to reduce tick count.
- **Pooling** reuses system instances instead of allocating/freeing per spawn (high-frequency one-shots like impacts/gunfire). `MaxPoolSize` is a system property; reused instances reset rather than reallocate.
- **Fixed bounds** is the #1 Niagara optimization and is REQUIRED for GPU sim — set via `set_fixed_bounds` / `set_emitter_property`.

## Quick Gotchas

- **No spaces in parameter names** — breaks HLSL and scripting.
- **Additive materials don't need sorting** — set sort mode to None for free perf.
- **Dynamic Parameters:** 4 slots x 4 channels = 16 floats for Niagara-to-material communication.
- **Emitter display name vs handle ID:** module/renderer actions may fail with "Emitter not found" on the display name; use `list_emitters` to get the handle ID (e.g. `Emitter_0`).
