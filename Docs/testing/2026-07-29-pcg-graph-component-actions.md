# PCG Graph and Component Actions Verification

**Date:** 2026-07-29
**Scope:** `MonolithPCG`, `pcg` namespace, shared exact asset-path helpers, and strict complex-parameter schemas
**Stacked base:** `912b4bf4a9b073965ff95cf8badb72b2bb8ca9b4` (`jules/codex/source-control/provider-actions`)
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the fork gains exactly 28 practical PCG actions without duplicating source-control behavior, that exact-path graph authoring and editor-world component lifecycle contracts remain fail-closed and bounded, and that one implementation builds and passes its focused automation suite on UE 5.7 and UE 5.8.

This change is intentionally stacked on the source-control action branch because the nine PCG component mutators/schedulers and guarded graph writes use `FMonolithSourceControlUtils` at the handler boundary. The PCG change does not copy, stub, or silently bypass that dependency.

---

## 2. Surface and Contracts

| Surface | Count | Evidence |
|---------|------:|----------|
| Discovery and reference migration | 5 | `get_status`, `list_graph_assets`, `get_graph_asset`, `list_components`, `remap_graph_references` |
| Typed graph authoring | 12 | Node-type discovery, graph create/read, node/edge/settings mutation, graph user parameters, exact static-subgraph assignment, complete graph replacement, and structural validation |
| Component lifecycle | 11 | Exact component create/read/configure, Blueprint-template assignment, generate/refresh/cancel/cleanup, bounded output inspection, and graph-instance user-parameter overrides |
| Total | 28 | Registration automation and regenerated catalog agree |

The important acceptance contracts are:

| Contract | Result |
|----------|--------|
| Exact identity | Project-owned graph assets, Actor Blueprints, live actors, and components require canonical exact paths; labels, short names, redirectors, stale pointers, and cross-world objects do not substitute |
| Strict containers | Required object/array parameters use `allow_string_encoded_complex=false`; quoted JSON objects or arrays return invalid params |
| Graph mutation | Public PCG graph/node/pin APIs, structural validation, bounded read-back, save evidence, snapshot rollback, and dirty-bit restoration |
| Complete replacement | Seeded CoreUObject duplication preserves the target package/object and default I/O identities while comparing the complete donor-owned persistent object graph under explicit bounds |
| Component mutation | Validates and classifies rejection/no-op/coalescing before handler-owned source-control preparation and the first side effect |
| Async execution | Generation and cleanup return promptly; callers poll exact component state, and available 64-bit task ids are decimal strings |
| UE version honesty | UE 5.8-only property-bag kinds, graph workspace state, offline-generation state, current cleanup-id access, and document helpers are compile-gated; UE 5.7 reports explicit `*_supported=false` fields instead of fabricated values |
| Excluded feature classes | No security, benchmark, invocation-log/metadata, action-search/planning-metadata, or reinforcement-learning implementation was ported |

---

## 3. Verification Environment

| Engine | Engine association | Host project | Plugin source |
|--------|--------------------|--------------|---------------|
| UE 5.7 | `5.7` | `D:\P4\MonolithPCGUE57Host\MonolithPCGUE57Host.uproject` | Junction to `D:\P4\MonolithForkPCG` |
| UE 5.8 | `5.8` | `D:\P4\MonolithPCGUE58Host\MonolithPCGUE58Host.uproject` | Junction to `D:\P4\MonolithForkPCG` |

Each engine root was resolved from the isolated host project's `EngineAssociation`. The UE 5.8 build was a full clean target build, so no binary produced by the UE 5.7 run could satisfy its link step.

---

## 4. Commands

The isolated editor targets were built through each resolved engine's UnrealBuildTool:

```powershell
& D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithPCGUE57Host\MonolithPCGUE57Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE

& D:\Engine\UE_5.8\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithPCGUE58Host\MonolithPCGUE58Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE
```

Focused automation used the same prefix on both engines:

```text
Automation RunTests Monolith.PCG
```

The action catalog was regenerated independently for the exact stacked base and target:

```powershell
python D:\P4\MonolithPortAudit\Tools\MonolithQuery\generate_monolith_catalog_snapshot.py `
    --root D:\P4\MonolithPCGBaseAudit `
    --out D:\P4\MonolithPCGBaseCatalog.json

python D:\P4\MonolithPortAudit\Tools\MonolithQuery\generate_monolith_catalog_snapshot.py `
    --root D:\P4\MonolithForkPCG `
    --out D:\P4\MonolithPCGTargetCatalog.json
```

The fork does not carry upstream's hosted-static-CI configuration, so the latest upstream checker and an identical temporary configuration were applied to both exact worktrees. Benchmark, analyzer/log, proxy, skill-drift, and offline checks were disabled because they are outside this PR and because the fork's older offline-freshness helper exposes `SRC_PATH` while the latest checker expects `SRC_PATHS`. Repository-wide CRLF advisories were also allowlisted for the parity comparison. The temporary files were removed after the run and are not part of the change.

---

## 5. Results

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Full editor target build | PASS, 203/203 build actions | PASS, clean 454/454 build actions |
| Final build result | Succeeded | Succeeded; UBA 281.49 seconds, total 304.50 seconds |
| Focused automation | PASS, 36/36 | PASS, 36/36 |
| Success-with-warning tests | 0 | 0 |
| Automation failures / not-run | 0 / 0 | 0 / 0 |
| Automation log | `D:\P4\MonolithPCGUE57Host\Saved\Logs\PCGAutomation-UE57-Final.log` | `D:\P4\MonolithPCGUE58Host\Saved\Logs\PCGAutomation-UE58.log` |
| `UnrealEditor-MonolithPCG.dll` bytes | 1,467,904 | 1,532,416 |
| `MonolithPCG` DLL SHA256 | `C1D5CFE58072013F380BE7665ACCE4807A88DE44D4A895552BAFD86D37F7C474` | `C9764D6F470A431591EB73CE21B31CE4473DDFC509E1E56B73BABCF874E7B725` |

Both automation logs end with `Automation Test Queue Empty 36 tests performed`. The isolated headless hosts also logged an occupied Monolith HTTP port; UE 5.8 logged that its intentionally content-free host has no `Content` directory. Those startup diagnostics did not fail or skip a PCG test and are not acceptance evidence for the action surface.

Additional gates:

| Gate | Result |
|------|--------|
| Catalog base → target | PASS: 1572 → 1600 actions, 25 → 26 namespaces |
| Catalog delta | PASS: exactly 28 additions, all `pcg.*`; zero removals |
| Duplicate full names | PASS: 0 |
| `pcg` roster | PASS: exactly 28 |
| Latest checker self-test | PASS |
| Base-vs-target static parity | PASS: base 8 blockers/11 advisories; target 8/11; new findings 0, resolved findings 0 |
| Pre-existing static blockers | Seven Niagara high-risk registrations lacking the upstream checker marker, plus the fork's absent hosted-static-CI workflow |
| Pre-existing static advisories | Ten Niagara raw-parameter/direct-load notices, plus the external `.claude/agents` prerequisite |

---

## 6. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | This PR adds editor C++ handlers, schemas, automation, specs, and a skill; it does not add or change project content, gameplay presentation, UI, VFX, material, or asset visuals. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 7. Conclusion

PASS. The stacked change adds exactly 28 unique `pcg` actions, compiles and links the same implementation on UE 5.7 and UE 5.8, passes 36/36 focused tests on each engine, adds no static finding relative to its exact base, and keeps unsupported cross-version values explicit instead of substituting defaults.
