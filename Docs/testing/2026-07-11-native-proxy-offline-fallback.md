# Native Proxy, Immutable Query Bundle, and Headless Control-Plane Verification

| Field | Value |
|---|---|
| Date | 2026-07-11 |
| Final reconciliation | 2026-07-12 |
| Submitted implementation | CL1106 (Query/Source), CL1107 (watchdog/recovery), CL1108 (native Proxy/evidence), and CL1109 (onboarding/release), with a compatibility Proxy/skill follow-up in CL1113 |
| Evidence/follow-up only | Pending CL1114; no submit is performed by this verification pass |
| Current proxy latency follow-up | Pending CL1135; no submit is performed; proxy v1.1.6 build, real-endpoint smoke, and 87-check fake-server regression are recorded below |

The implementation changelists were submitted by an external actor while the
final gates were running. This verification pass did not submit, merge, close,
or delete them. Their descriptions contain superseded intermediate hashes and
gate results; the rerun evidence below is authoritative.

## 1. Adversarial Verdict

**KEEP the architecture, REWORK the original implementation, and ACCEPT the
final candidate.** Every focused submission gate recorded below passed.

The high-ROI decision is not “run another Unreal Editor target.” The retained
design is a UE-DLL-free `/MT` native stdio proxy with two routes:

- validated live editor HTTP/MCP for editor, UObject, world, and asset work;
- a manifest-selected immutable `/MT` Query/catalog generation for a bounded,
  read-only offline surface.

Independent adversarial reviews reproduced correctness and deployment failures
that the original green suite did not cover. Those findings were fixed and
turned into regression cases before this final candidate was selected. The
review also rejected inflated task-completion claims and corrected the artifact
terminology: filenames are **source-addressed**, while manifests verify the full
artifact SHA-256.

## 2. Why This Is High ROI, and What Is Actually Measured

The operational problem is frequent enough to justify a separate control plane.
The July 10-11 watchdog evidence reviewed during this work contained 82 restart
sequence starts and 29 build failures. A long-lived Unreal Editor process also
keeps editor/plugin DLL images mapped, coupling MCP availability to build and
restart churn.

The first production fallback sample was a clustered 56-call slice over roughly
47 minutes, not 56 independent user tasks:

- 31 calls returned non-error tool responses;
- 25 calls returned contract errors;
- 26 calls returned indexed data: 11 `project.search`, nine
  `project.find_references`, four `project.get_asset_details`, one
  `source.review_context`, and one `source.read_file`;
- remaining non-error calls were control-plane or catalog interactions.

This sample does **not** measure user-task completion. In particular, “35/56
tasks completed” and “51/56 success” are unsupported and are withdrawn. A tool
call can return data without completing the user’s larger task, and offline
schema discovery is deliberately degraded guidance rather than live reflected
schema proof.

The defensible ROI is:

1. MCP client lifetime is independent of editor lifetime.
2. The control plane and Query reader import no Unreal, game, or Monolith DLLs.
3. Supported indexed reads and bounded recovery guidance remain available while
   the editor is stopped, rebuilding, or restarting.
4. Existing mapped native images do not need to be killed to publish a new
   source-addressed generation.
5. Editor-down metadata is exactly four stable tools rather than replaying a
   stale 52-53-tool cache.

## 3. Final Architecture and Selected Generations

```text
MCP client
  -> monolith_proxy.current.json
  -> monolith_proxy-<source-hash>.exe              /MT, no UE DLLs
       |-- live -> validated Unreal Editor /mcp
       `-- offline -> monolith_query.current.json
                      |-- monolith_query-<source-hash>.exe
                      `-- monolith_catalog-<semantic-hash>.json
```

The proxy manifest selects:

```text
file        = monolith_proxy-9f6fd870130562cc.exe
version     = 1.1.6
source_hash = 9f6fd870130562cc
sha256      = c16a4a8e474795dc1b6c972c691e8565acd4746b6389f11c6a514aa2fdf55d5c
```

The Query bundle manifest contains exactly 11 fields and selects:

```text
query_file          = monolith_query-fc2c30f99269631c.exe
query_source_hash   = fc2c30f99269631c
query_sha256        = 8faeca8a553a182b0ceedca2d0fd5f36213ed224e072cedcaca3d15ae71d3eaf
catalog_file        = monolith_catalog-9a08e586de19a98d98f585ca464e53469b8ac8f58e20abf50ca7d64ab41e3c50.json
catalog_source_hash = 9a08e586de19a98d98f585ca464e53469b8ac8f58e20abf50ca7d64ab41e3c50
catalog_sha256      = 90069a25edf9ef393e401dd5b4e67b2d2b04b9962701e9cd62e69606706d3644
catalog_actions     = 2040
```

At proxy startup the Query manifest, executable, and catalog are validated once
and pinned. Existing proxy sessions retain their validated Query/catalog
generation even if `monolith_query.current.json` advances. A new session adopts
the new generation. The historical fixed proxy is compatibility-only. The fixed
Query is a best-effort direct-CLI compatibility copy and is not the proxy or
release authority.

The earlier reconciliation recorded Codex and Claude user-level MCP
configuration at the then-current immutable image. Publishing this follow-up
manifest does not rewrite or restart existing long-lived client processes; they
retain their already loaded immutable image until explicitly reconfigured or
restarted. The tracked project `D:\P4\speed\.mcp.json` remains a portable HTTP
configuration and was not changed to a machine-local absolute executable path.

## 4. Why a New Installed-Engine Editor Binary Was Rejected

The proposed isolated editor-backed binary was tested against the installed UE
toolchain rather than accepted from target naming alone:

- another modular Editor target still uses `AppName=UnrealEditor` and consumes
  the same `UnrealEditor-*.dll` module outputs;
- `BuildEnvironment.Unique` is rejected by the installed-engine build;
- a monolithic custom Editor graph can be exported, but the installed engine
  lacks the monolithic Editor `.precompiled` manifests required to link it;
- a Program target cannot host the UnrealEd/GEditor/Slate/editor-subsystem
  surface that Monolith editor actions require.

A truly isolated editor worker remains possible with a matching source-built UE
root, a unique output graph, a private launcher, and a single-writer lease for
editor-owned databases. That cost and risk do not beat the native proxy plus
read-only Query path for the current DLL-lock and availability problem.

## 5. Adversarial Findings Converted to Contracts

| Finding | Final contract |
|---|---|
| Any HTTP 200 or look-alike process on 9316 could be treated as the editor by recovery automation. | Recovery scripts validate the health schema, actual IPv4/IPv6 listener ownership, OS process image and command line, exact host project, and process mode. Foreign projects and game/server/commandlet/other-editor processes are excluded from build, launch, and stop decisions. |
| A protocol-shaped endpoint could pass proxy health without being the reported current editor. | The proxy requires exclusive listener ownership plus `monolith_status` identity for that same PID: `status=current_editor_process`, `commandlet=false`, and the expected host project root. Status-PID spoofing, commandlet status, foreign project identity, and multi-owner listeners fail closed. The proxy does not independently claim recovery's OS command-line or game/server classification. |
| A healthy editor returned `monolith_status` in 2,072--2,522 ms and `/health` in 2,503 ms. Proxy v1.1.5 raised only the identity budget, so the 250 ms health gate still rejected the real endpoint as offline. | Proxy v1.1.6 gives `/health` and each fresh, uncached editor/project identity POST independent 5,000 ms absolute deadlines. A 3,000 ms boundary would leave at most 478 ms above the measured maximum and was rejected. Separate delayed-health and delayed-status regressions each wait 600 ms and route live; PID, commandlet, project-root, listener-owner, and project-swap rejection checks remain intact. |
| The official recovery and watchdog scripts still hard-coded a 3-second `/health` request timeout. At 17:53:36 on 2026-07-12, the watchdog rejected the valid exclusive listener/editor PID 49268 as `Blocked reason=foreignOrUntrustedMcpEndpoint healthError=request_failed detail="The operation has timed out."` and exited, leaving no active supervisor. | Both scripts now expose validated `-HealthTimeoutSec` (default 5, range 1--60); `watch_mcp.ps1` forwards the same value to every `recover_mcp.ps1` child. It is used only for an individual `/health` request. Health schema, process/project identity, exclusive listener ownership, and every pre-build/pre-launch/pre-stop mutation gate remain fail closed. |
| A first 5-second watchdog rollout still failed after post-recovery indexing: PID 30740 returned `McpUp` at 18:09:02, the next request timed out at 18:09:22, and the old occupied-listener branch logged `Blocked` and terminated the wrapper/watch/editor chain. Any fixed short timeout can be exceeded by loaded editor work, so raising it again would only move the failure. | A transport failure becomes `TrustedEditorBusy` only when OS state independently proves one exclusive listener PID, a live `UnrealEditor.exe` / `UnrealEditor-Cmd.exe`, a readable eligible editor-mode command line for the exact `Speed.uproject`, and a matching PID. The watchdog performs no build/recover/launch/stop mutation, retries with exponential sleep capped at the validated `-TrustedBusyBackoffMaxSec` (default 60), resets the state on valid health, and stays alive. Invalid HTTP, wrong executable/project/mode/PID, unreadable identity, and multi-owner/inconsistent listeners remain blocked. Full recovery skips launch and waits inside `-TimeoutSec`; one-shot probes return `MCP_BUSY`/exit 2. |
| `/health` used per-phase timeouts, so a trickling body could exceed the intended total budget. | Health GET uses an absolute total deadline, a 64 KiB bound, exact `Content-Length`, and fails on read errors, early EOF, or declared-length mismatch. |
| Live MCP responses were insufficiently bounded. | POST `/mcp` has an absolute total deadline, a 16 MiB body cap, and exact body-length validation. |
| Every generic call could use the short read route, including live mutations. | Fast-path eligibility requires concrete namespace/action metadata proving offline availability, no live requirement, no mutation, and no long-running behavior. Unknown or live-only actions use the strong live route. |
| Missing or corrupt catalog metadata shortened mutation duplicate protection from 60 seconds to three seconds. | Only explicitly proven safe reads get the short repeat window. Missing/invalid catalog, unknown actions, and missing metadata use the strong guard. The window starts at completion, in-flight calls are protected, and the cache is bounded. |
| Bundle validation could report SHA failure but leave partially filled paths usable. | Query and catalog paths are returned only when the complete bundle is `valid`; SHA or leaf tampering now prevents offline execution. This fail-open was reproduced by the new production-path regression before being fixed. |
| The original suite always set `MONOLITH_CATALOG_SNAPSHOT`, so it never exercised the production Query-manifest branch. | The suite now launches an isolated Binaries directory containing no fixed Query or source-tree catalog, proves manifest-selected Query and catalog success, proves a running proxy keeps its pinned generation, and proves SHA/leaf tampering fails closed. |
| A fixed Query and mutable source-tree catalog could change an old proxy session’s semantics or hit a Windows image lock. | Query and catalog publish under immutable names, a strict current manifest selects a pair, the proxy pins that pair once, and release/onboarding consume only validated generations. |
| Query build could publish an internally valid but stale generated catalog. | `build.bat` runs the catalog generator `--check` before compilation/publication; failure leaves the current manifest unchanged. |
| Python “strict JSON” accepted duplicate keys and `NaN`/`Infinity` that the C++ parser rejects. | Publisher parsing rejects duplicate keys and non-finite constants; semantic serialization uses `allow_nan=False`. |
| Release validated Query freshness using the manifest’s own source hash. | Release independently hashes the current Query source inputs/build contract, validates the frozen manifest against that expected hash, validates the selected immutable Query rather than the fixed alias, and revalidates staged bytes. |
| Direct Query catalog actions silently used `Tools/.../Generated`, bypassing the immutable pair. | Binaries executions validate the strict current manifest, running/selected Query SHA and source identity, catalog SHA/semantic identity, and use the validated catalog bytes. Explicit `--snapshot` remains the development/test override. |
| Global client remove/add and project/instruction writes could leave partial state. | Onboarding uses a transaction-wide compare-and-swap rollback and same-directory atomic file replacement; concurrent external edits are preserved rather than overwritten. |
| A per-row parity skip could hide widespread database unavailability. | One exact rollback-journal refusal in preflight invalidates the entire benchmark run: all 317 rows become `SKIP(environment_blocked)`, comparable count and score are zero, and the Python reader is not opened. |
| A dirty workspace polluted the generated catalog with test registrations and unrelated pending Lyra/PCG work. | Generator input excludes every path component named `Tests` case-insensitively. The tracked snapshot was generated from isolated source plus intended CL overlays and has zero test rows. |
| Raw source-byte hashing minted different Query generations for P4 CRLF and clean-Git LF checkouts of identical text. | `Scripts/source_generation_hash.py` canonicalizes CRLF and lone CR to LF for generation identity across Query, Proxy, freshness, build, and release. Executable/catalog/manifest SHA-256 remains exact raw-byte verification. |
| Proxy builds used compiler `/Brepro` but still encoded checkout-dependent differences, and the final linker invocation lacked explicit `/Brepro`, allowing different PE bytes under one source-addressed name. | The v3 contract adds compiler `/experimental:deterministic` plus `/pathmap:<plugin-root>=.`, explicit linker `/Brepro`, and regression checks for both build.bat and CMake. Two isolated LF builds and one shared CRLF build reproduced the rotated generation and SHA byte-for-byte. |
| A project-source prune SQL failure returned before clearing `bIsIndexing`, while the commandlet treated an existing stale DB as success. | Scope-exit closes the writer and broadcasts one explicit outcome on every run; observable database open/reset/schema/prune, module/file/symbol insert, transaction, final-maintenance, and cancellation failures fail the run. The subsystem keeps a failed DB latched closed, and the commandlet exits `1` for failed or missing completion. The three `Monolith.IndexGuard.Source.*` tests pin prune cleanup/completion, cancellation reporting, and commandlet propagation; legacy `void` auxiliary-row helpers and warning-only CRG refresh are not overstated as covered. |
| Recursive plugin discovery treated every nested directory named `Source` as a module, so 25 repeated file visits produced 239 exact duplicate symbol rows while the completion count exceeded the `files` table by 25. An attempted descriptor-only fix removed the duplicates but also dropped 51 unique files, including three real `EditableMesh` engine headers, and was rejected. The first defense patch also used `RemoveDuplicateSlashes`, which would corrupt UNC prefixes, while stored caller casing could drift module uniqueness across incremental Windows runs. | Discovery preserves standalone descriptor-free source roots and the prior unique-path corpus, suppresses only non-descriptor `Source` roots nested below another `Source`, and orders genuine nested descriptor roots most-specific first. It preserves UNC roots without passing them through UE 5.8's network-unsafe case corrector, canonicalizes local configured roots to on-disk casing without resolving junctions, and applies a normalized indexer-instance lexical path claim as defense in depth. The focused fixture covers a false nested `Source`, a retained descriptor-free root, an overlapping descriptor-backed inner plugin, clean/lowercase-relative-spelling incremental counts, inner ownership, and exact-symbol uniqueness; junction/hardlink identity is not claimed. |

Offline `monolith.discover` and `monolith.find` return
`status=degraded`, `completion_class=degraded_guidance`, and
`catalog_matches_live=unknown`. Source-scanned candidates do not claim current
live registry or profile membership.

## 6. Verification Evidence

### Native binaries

- `cmd /c Tools\MonolithQuery\build.bat`: the selected clean-source build is
  `monolith_query-fc2c30f99269631c.exe`, SHA-256
  `8faeca8a553a182b0ceedca2d0fd5f36213ed224e072cedcaca3d15ae71d3eaf`.
  A second isolated clean-source build reproduced the same filename, size, and
  SHA-256 byte-for-byte.
- `cmd /c Tools\MonolithProxy\build.bat`: the selected build is
  `monolith_proxy-9f6fd870130562cc.exe` v1.1.6, SHA-256
  `c16a4a8e474795dc1b6c972c691e8565acd4746b6389f11c6a514aa2fdf55d5c`.
  The source-generation helper independently returned `9f6fd870130562cc`, and
  the manifest-selected and compatibility executables have the same SHA-256.
- `python Tools\MonolithProxy\test_offline_fallback.py`: **87/87** named
  subprocess/socket/security/reproducibility/contract checks passed.
- A fresh exact `monolith_proxy-9f6fd870130562cc.exe` stdio process reached the
  real PID 60196 Unreal Editor endpoint. The read-only live-only
  `editor.get_build_status` gate completed in 3,672 ms with no offline marker;
  the following `monolith_status` completed direct in 515 ms with
  `status=current_editor_process`, PID 60196, and host root `D:/P4/speed/`.
  Proxy stderr recorded `Validated live MCP request succeeded; closing the
  offline circuit`. A status-only cold call remained on the documented
  offline-capable unknown-backend fast path and was not treated as timeout
  evidence. A separate follow-up `/health` request completed in 2,441 ms.
- `python Tools\MonolithQuery\test_publish_query_bundle.py -v`: **10/10**.
- `python Tools\MonolithQuery\test_generate_monolith_catalog_snapshot.py -v`:
  **4/4**.
- The accepted fresh full source reindex completed 1,390 modules (1,372 engine
  + 18 project), 89,619 files, 1,325,574 symbols, and 5,597,447 references with
  zero indexer errors. Comparison with the pre-fix corpus reported
  `old_only_paths=0`, `new_only_paths=0`, and exact duplicate symbol
  groups/extra rows `0/0`.
- Incremental project-source indexing then updated the native corpus to
  5,591,842 references and 54,411 inheritance rows. Its worker emitted
  `Indexer complete`, but the editor received an exit request while the
  game-thread completion path was still running a synchronous full derived-CRG
  repair. The next startup preserved native rows and correctly reported zero
  metrics/override rows as stale. An uninterrupted, health-gated
  `source.repair_crg_cache --execute` completed in 443 seconds; no
  `source.build_crg_graph` call was chained. Final counted health is
  `status=ok`, `warnings=[]`, nodes/metrics 1,325,574, valid edges 90,830,
  override edges 133,185, and `maintenance_required=false`.
- A physical SQLite audit then found orphan `never used` pages in the
  post-repair database even though logical Source health was clean. With the
  endpoint down and no sidecars, offline `VACUUM INTO` reconstructed the file.
  The 77 schema objects, all user-table counts, sequence/statistics rows, and
  CRG metadata matched the pre-vacuum database. The pre-watchdog promoted
  baseline was 3,433,975,808 bytes, SHA-256
  `968ffe183730d92677e654e8867dd767429fddb43621ab682de6c589e8014df6`;
  `PRAGMA quick_check` and full `integrity_check` both returned `ok`, a real
  FTS5 query succeeded, and offline counted Source health remained clean.
- `python Scripts\verify_offline_parity.py --exe
  Binaries\monolith_query-fc2c30f99269631c.exe` was rerun against a
  byte-identical copy of that final database: **134 MATCH**, **0 DIFF**,
  **0 ERROR**, and **3 corpus SKIP** -- 137 rows total, 134 comparable; exit 0.
  Output is retained at
  `D:\P4\speed\Saved\MonolithParity-final-post-vacuum-20260712.log`.
- `python Scripts\test_offline_parity_benchmark.py`: all **12** test functions
  passed, including run-level rollback-journal invalidation.
- Catalog inspection: declared/actual action count 2040, semantic hash matched,
  zero `Tests` source rows.
- `python Tools\MonolithProxy\verify_proxy_bundle.py --binaries-root Binaries`:
  release-stage pair validation passed with exactly four offline tools and
  `degraded_guidance` discovery.
- `dumpbin /dependents`: Proxy imports only `WINHTTP.dll`, `bcrypt.dll`,
  `IPHLPAPI.DLL`, and `KERNEL32.dll`; Query imports only `bcrypt.dll` and
  `KERNEL32.dll`. Neither imports VC runtime, Unreal, game, or Monolith DLLs.

### Recovery, onboarding, and live routing

- PowerShell 7.6.3/Pester 3.4 ran
  `Scripts/tests/WatchMcpChildProcess.Tests.ps1` **19/19** in 13.60 seconds;
  Windows PowerShell 5.1/Pester 3.4 ran the same suite **19/19** in 13.82
  seconds with `-ExecutionPolicy Bypass`. Both runtimes also parsed
  `watch_mcp.ps1`, `recover_mcp.ps1`, and the test file with zero errors. The
  added production-function regression delays valid project-bound health for
  3,200 ms, captures `Invoke-WebRequest -TimeoutSec 5`, and pins the validated
  default/range plus watchdog-to-recovery forwarding. A second production
  function test simulates a 5,200 ms post-index timeout and proves the exact
  trusted-busy identity boundary, capped 15/30/60-second retry sequence,
  no-mutation supervisor continuation/reset path, and recover skip-launch/wait
  path. Foreign executables/projects, commandlets, PID mismatch, unreadable
  command lines, multi-owner listeners, and invalid HTTP remain blocked.
- PowerShell 7 and Windows PowerShell 5.1:
  `Scripts/tests/OnboardMonolithAtomicity.Tests.ps1` **14/14** in each shell.
- `Scripts\recover_mcp.ps1 -TimeoutSec 180` returned `RESULT=MCP_UP`, exit 0.
  Final `monolith.status` reported version `0.20.3`, 1,840 live actions, 61
  namespaces, catalog `sha256:4af1375172e8818a`, project `Speed`, and an
  ownership-bound non-commandlet headless editor. Recorded PIDs are transient
  probe evidence, not stable identifiers.
- A default `Scripts\recover_mcp.ps1 -ProbeOnly` accepted the real exclusive
  listener/editor PID 49268 (`version=0.20.3`, `tools_registered=1842`) and
  returned `RESULT=MCP_UP`. The official
  `Binaries\monolith_watchdog.exe D:\P4\speed` wrapper then started
  `watch_mcp.ps1` PID 23956 through wrapper PIDs 37472/56688; its first event
  recorded `healthTimeoutSec=5`. After the due daily maintenance pass completed,
  the retained supervisor logged consecutive `McpUp` events at 18:05:01 and
  18:05:17 for the same PID 49268. The timeout-fix rollout did not stop that
  editor. After this proof, the root orchestrator deliberately stopped the
  known-stale headless editor; the still-running watchdog completed guarded
  pre-restart source/graph maintenance and recovered a fresh project-bound
  editor/MCP PID 30740 with `RecoverDone exitCode=0` at 18:06:53. Post-recovery
  indexing finished and PID 30740 logged `McpUp` at 18:09:02, but the following
  request exceeded five seconds at 18:09:22. The pre-state-fix branch logged
  `Blocked` and the wrapper/watch/editor chain exited; this run is negative
  evidence and is not claimed as a stable supervisor rollout.
- Live default-bound smokes returned 100 Blueprint rows from
  `project.find_by_type` (schema default 100), 50 symbol plus 50 source-line
  rows with a cursor from `source.search_source` (default 50 per result class),
  and lines 1--200 from `source.read_file` when no range was supplied.
- `source.trigger_project_reindex` completed 18 project modules, 1,875 files,
  14,768 symbols, and 119,872 references with zero errors; it reported a scoped
  CRG refresh for 15,287 affected symbols. `Indexer complete` was only the
  worker milestone: the later `Project source indexing complete:` line and a
  clean counted health result are the shutdown gate. The interrupted completion
  maintenance and subsequent health-gated recovery are recorded above and in
  `Docs/TODO.md`; they are not mislabeled as a normal clean reindex shutdown.
- After all offline gates, the per-user `Monolith MCP Watchdog - Speed` task was
  re-enabled and started. Startup UBT reported the target up to date; its
  restart commandlet completed 18 modules, 1,875 files, 14,768 symbols, and zero
  errors in 33.9 seconds, then the cooldown-gated graph export completed and the
  headless editor returned. The editor-side incremental hook reached the true
  terminal `Project source indexing complete:` line and skipped the global CRG
  rebuild as already fresh. At the 2026-07-12 11:55 KST restoration gate,
  bridge status was `indexing=false` with both databases available/open and
  counted Source health was clean with 1,325,574 symbols/nodes/metrics,
  5,591,843 references, 90,830 valid edges, and 133,185 override edges. The
  11:58 KST post-restoration read-only audit returned
  `quick_check=ok` and `integrity_check(1)=ok`, with page size 4,096, page count
  839,709, and freelist count zero. The one-reference delta from the earlier
  gate was the then-current incremental source corpus; file, symbol, FTS, and
  CRG parity counts stayed stable at that gate.
- A fresh `monolith_proxy-930fceebf8593512.exe` 1.1.4 stdio process called the
  live-only `editor.get_build_status`; `isError=false`, `compiling=false`,
  `errors_since_compile=0`, and no offline fallback marker was present.
- The earlier transactional onboarding pass selected
  `monolith_proxy-930fceebf8593512.exe` for both Codex and Claude user scope;
  Claude reported the stdio MCP server connected. The CL1135 build does not
  claim an active-client restart or reconfiguration.

### Repository gates

- The required command, `python Scripts/ci_static_checks.py --config
  .github/monolith-static-ci.json --github check`, ran in an isolated worktree
  containing the final CL1114 scope and a validated current database snapshot.
  It exited 0 with no blocker. The exact advisory-only output is retained at
  `D:\P4\speed\Saved\MonolithStaticGate-final-success-20260712_120005.log`;
  advisory text-hygiene, external-prerequisite, heuristic, and live skill-drift
  results are preserved there rather than summarized beyond the log evidence.
- Full UBT command `Build.bat SpeedEditor Win64 Development
  -Project=D:\P4\speed\Speed.uproject -NoHotReloadFromIDE` first exposed only
  P4 read-only linker outputs. After checking out exactly the 21 requested DLLs
  in a non-submittable verification CL, the identical retry completed all 23
  actions and the post-build launcher with exit 0. The 21 DLL edits were then
  reverted and the temporary changelist deleted.
- UE automation report
  `D:\P4\speed\Saved\Logs\Automation\MonolithSubmissionGate15-20260712-110838`
  records **15 succeeded / 0 succeeded-with-warnings / 0 failed / 0 not-run /
  0 in-process**; `UnrealEditor-Cmd.exe` exited 0. The set covers the two C++
  ergonomics fixtures, project type limit/filter contracts, five cursor
  contracts, four source failure/limit contracts, and the nested-Source
  discovery fixture.
- The implementation scope is submitted as CL1106--1109 with artifact
  follow-up CL1113. Before this evidence edit, `p4 reconcile -n` reported
  no Source implementation, database-contract, or selected-artifact drift.
  Pending CL1114 contains only this evidence file, the root high-ROI roadmap,
  and the directly related CRG/native-fallback TODO entry; this pass performs
  no submit.
- The duplicate/collision snapshot at 2026-07-12 12:19 KST ran
  `git fetch origin --prune`, scanned all 216 remote refs against the three
  CL1114 paths, and fetched every open PR's actual file list. PRs #1719, #1720,
  and #1721 touched only `MonolithUIActions.cpp`, `Docs/API_REFERENCE.md`, and
  `UIReflectionHelper.cpp`, respectively. The only remote exact-path hit was
  `Docs/TODO.md` on the historical
  `origin/p4-snapshot/monolith-ue58-divergent-20260620` branch; it has no open
  PR and is not an active work queue. Under `//speed/Plugins/Monolith/...`, the
  snapshot also enumerated CL1100's AssetEditing benchmark scope and CL1115's
  GIF-fix scope from their actual opened files. Neither overlaps CL1114; counts
  are intentionally omitted because those concurrent scopes can continue to
  grow after this evidence snapshot.

## 7. Explicit Boundaries

- No offline mutation is enabled. Editor/UObject/world/asset mutation remains
  live-only.
- The offline catalog has 2,040 source-scanned entries. It is a different
  surface from the live reflected registry and is not claimed to match it;
  offline responses say `catalog_matches_live=unknown`. The final live count is
  recorded once in the recovery/routing evidence above.
- Valid live `tools/list` success resets a **transport-wide** circuit. Invalid or
  timed-out metadata calls do not open the action circuit. This is intentional,
  not a claim of two fully independent circuits.
- Source-addressed filenames plus full SHA verification detect byte drift but do
  not claim cross-toolchain artifact-content addressing.
- Actual user-task completion remains unmeasured and must be evaluated in later
  end-to-end session samples, separately from tool-call success.
- No installed-engine isolated editor worker was shipped or claimed.
- Full UBT, the focused 15-test UE automation set, native parity/regression, and
  final live-routing/health gates were all rerun after artifact selection. This
  is a focused submission gate, not a claim that every Monolith or game test was
  executed.
- The interrupted completion-maintenance event is a real derived-cache
  durability gap, not a measurement artifact. It does not invalidate the
  duplicate-free discovery result because native rows survived and the final
  health, SQLite integrity, and parity gates are clean. Commit-result handling,
  interruption/reopen coverage, terminal job signaling, and a typed physical-DB
  repair path remain explicit follow-up work.
- Only the manifest-selected `9f6fd870130562cc` generation is the current
  shipping and onboarding authority. Any older diagnostic local process or
  generation is outside P4/release authority; no client process is killed merely
  to delete a Windows-locked old image.

## 8. Submitted Scope and Ownership

| CL | Single responsibility |
|---|---|
| 1106 | Query/catalog immutable bundle, offline parity contract, and the dependent Source/Index changes and binaries. |
| 1107 | Watchdog/recovery ownership checks, DLL-lock preflight, and fail-closed build/launch gates. |
| 1108 | Native Proxy transport/routing/security, immutable session consumption, proxy image/manifest, and verification evidence. |
| 1109 | Transactional onboarding and explicit allowlisted release staging for validated Query/Proxy generations. |
| 1113 | Compatibility fixed-Proxy alias and skill-readme follow-up. |
| 1114 (pending) | Final gate evidence, root-roadmap reconciliation, and the directly related CRG/native-fallback TODO; no code or public contract change. |
| 1135 (pending) | Independent 5,000 ms Proxy health/identity budgets, delayed-health and delayed-status live-routing regressions, proxy v1.1.6 immutable/fixed artifacts and manifest, plus the matching validated 5-second recovery/watchdog health budget, exact-project trusted-busy no-mutation supervisor state, capped retry/reset semantics, cross-script forwarding, regressions, and synchronized contract/evidence docs. |

CL1106--1109 and CL1113 were submitted by an external actor while
the gates ran. This verification pass did not submit them, and no existing
editor, game, server, or MCP process was killed merely to publish or delete a
generation.
