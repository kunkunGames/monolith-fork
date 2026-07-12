## 2024-05-06 - Bound Audio Query Limits
**Boundary:** list_audio_assets, search_audio_assets, find_unused_audio limits
**Learning:** Extracting unvalidated double/string fields via GetNumberField for counts causes potential crashes and extreme unbounded scans.
**Prevention:** Always use TryGetNumberField for limit/count properties and explicitly FMath::Clamp them to safe boundaries (e.g. 0 to 1000).

## 2025-02-18 - Bound MonolithAudioQueryActions find_unattenuated_sounds results array
**Boundary:** `limit` param and `ResultsArray.Num()` on `find_unattenuated_sounds` action
**Learning:** `FindUnattenuatedSounds` traverses potentially tens of thousands of assets via the asset registry. An unbounded list risks returning a huge JSON array, which could consume significant memory and crash the VM/MCP if a project has huge numbers of missing attenuations.
**Prevention:** Bound queries iterating `AssetDataList` and returning large result sets with an optional `limit` parameter, defaulting to a conservative max like 100.
[blocked: UE 5.7 editor unavailable in Jules VM]
## 2026-05-05 - 🧱 LimitGuard: Bound audio/find_sounds_without_class limit
**Boundary:** `limit` query parameter upper bound and JSON type.
**Learning:** Raw JSON number access combined with unbounded array allocation for results allows trivial out-of-memory or timeout exploits for large queries.
**Prevention:** Always use `TryGetNumberField` and immediately `FMath::Clamp` user-provided limits against a sane local maximum (e.g., 1000) before pre-allocating result arrays or initiating resource-heavy loops.
## 2026-05-10 - 🧱 LimitGuard: Bound GAS effect and tag bounds
**Boundary:** `depth` and `stack_limit` query properties in MonolithGAS.
**Learning:** Using GetNumberField for bounds and directly casting to int without clamping can result in unbound tree recursion (in tag hierarchies) or excessive loop bounds/stack limits (in effects).
**Prevention:** Always use `TryGetNumberField` and apply `FMath::Clamp` against a sane local maximum (e.g. 100 for tag depth, 1000 for stack limits) to prevent trivial resource exhaustion or overflows.
## 2026-05-11 - 🧱 LimitGuard: Bound editor/run_automation_tests max_tests
**Boundary:** `max_tests` parameter in `run_automation_tests`.
**Learning:** `run_automation_tests` operates synchronously inside the editor (without PIE or a separate process). Using the `max_tests` argument with extreme values allows thousands of tests to be executed sequentially on the main thread, resulting in potentially massive editor hangs/freezes.
**Prevention:** Clamp the maximum tests limit using `FMath::Clamp` with a hard limit (e.g. 1000) to prevent single-action test exhaustion and document it properly in schemas.
## 2026-05-16 - 🧱 LimitGuard: Bound editor/search_build_output limit
**Boundary:** `limit` parameter in `search_build_output`.
**Learning:** `search_build_output` scans the cached log capture for build-related entries. An unbounded limit combined with unsafe `GetNumberField` usage could lead to type-casting errors or excessive JSON array allocation for very large build logs.
**Prevention:** Always use `TryGetNumberField` to validate the `limit` param and apply `FMath::Clamp` with a hard limit (e.g. 1000) to prevent oversized payloads.
## 2026-05-17 - 🧱 LimitGuard: Bound editor/capture_sequence_frames timestamps array
**Boundary:** `timestamps` array limit in `capture_sequence_frames`.
**Learning:** An unbounded timestamps array combined with synchronous frame rendering in `capture_sequence_frames` can lock up the editor and use immense memory/disk space if extremely large payloads are supplied.
**Prevention:** Always use `TryGetArrayField` to parse arrays safely and assert a hard maximum (e.g., 1000) using `Num()` on array size before allocating frame-capture loops.
2026-05-18 - Bound preview_textures asset_paths array length

Boundary: `preview_textures` parameter `asset_paths` maximum array size
Learning: Processing massive numbers of textures in a single grid layout operation spikes memory consumption aggressively because it creates large `TArray<uint8>` pixel sheets synchronously. Without an upper bound, a client query matching all project textures could OOM the editor during contact sheet generation.
Prevention: Operations that allocate uncompressed rendering buffers or composite layouts scaled by input counts must clamp user-provided `asset_paths` to a conservative maximum (e.g. 100 tiles max per contact sheet).
## 2026-05-19 - 🧱 LimitGuard: Bound audio/create_random_sound_cue wave generation
**Boundary:** `sound_waves` array length in `CreateWavePlayerNodes` helper inside `MonolithAudioSoundCueActions.cpp`.
**Learning:** `CreateWavePlayerNodes` constructs `USoundNodeWavePlayer` instances directly using the size of the unvalidated `sound_waves` array parameter. Since this helper is shared by multiple cue creation actions (like `create_random_sound_cue`, `create_switch_sound_cue`, etc.), an unbound array could maliciously or accidentally exhaust memory via massive loop iterations and internal allocations if the client supplies an extraordinarily large list of asset paths.
**Prevention:** Bound collection sizes before allocating sound nodes. Add an immediate `if (WavePaths.Num() > 100)` check in the helper (or the corresponding `TryGetArrayField` extraction phase) to enforce a sane local limit, protecting against resource exhaustion.
## 2026-05-20 - 🧱 LimitGuard: Bound audio/create_interactive_metasound wave generation
**Boundary:** `sound_waves` array length in `CreateInteractiveMetaSound` action inside `MonolithAudioMetaSoundActions.cpp`.
**Learning:** An unbounded arrays combined with iterating and creating builder node representations within `CreateInteractiveMetaSound` can lead to unchecked resource allocation resulting in out-of-memory or timeout exploits for large inputs.
**Prevention:** Always assert a hard maximum array boundary (e.g. 100) after reading an array field and before processing nodes using `FString::Printf(TEXT("... exceeds the maximum allowed (100)"), ArrayPtr->Num())` for large list based actions.
## 2026-05-28 - 🧱 LimitGuard: Bound audio batch actions array lengths
**Boundary:** `asset_paths` array length in all audio batch actions.
**Learning:** `asset_paths` was unbounded across 10+ batch operations like `batch_assign_sound_class`, `batch_assign_attenuation`, `apply_audio_template`, etc. If an extremely large array of assets (e.g. 50,000+ project assets) is passed by a user, iterating and mutating all those assets synchronously in a single undo transaction without yield could cause massive memory spikes and lock up the editor.
**Prevention:** In `AudioBatchHelpers::ParseAssetPaths`, add a hard cap on `asset_paths.Num()` (e.g. `200` to mirror `batch_execute` standards) to prevent excessive single-action processing load.

## 2026-06-25 - Forbid numeric branch evasion
**Coordination issue:** LimitGuard generated multiple branches with large numeric suffixes (e.g., `-6365188721292045649`, `-10223219369105163440`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.

## 2026-07-12 - Explicit duplicate / collision check required
**Coordination issue:** LimitGuard merged identical fixes multiple times (e.g., `da40349` and `39c15f3` for WorldGen prop scatter counts) because it did not check for overlapping PRs or branches from itself or domain keepers.
**Learning:** Checking for collisions only within an agent's own track or relying strictly on explicit file checks is insufficient if the agent doesn't check open branches or closed PRs first.
**Prevention:** Run the cross-agent branch check first; if the keeper (or any agent) has an open branch or recently merged PR touching the intended files or same-area, stop without PR.
**Avoid:** Submitting duplicate PRs for limits that are already clamped by another branch.
