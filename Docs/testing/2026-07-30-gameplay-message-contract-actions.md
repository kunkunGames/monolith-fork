# Gameplay Message Contract Actions Verification

**Date:** 2026-07-30
**Branch:** `jules/codex/gameplay-message/contracts`
**Fork base:** `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Reference source:** `kunkunGames/monolith@a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1`
**Scope:** Five bounded, read-only `gameplay_message` contract actions in a new editor module
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Goal

Verify that the public fork gains a practical GameplayMessageRouter preflight
surface without adding runtime mutation, project-specific dependencies, or any
of the explicitly excluded feature classes. The accepted implementation must:

- add exactly five `gameplay_message` actions and remove none;
- avoid a compile-time dependency on `GameplayMessageRuntime` or
  `GameplayMessageNodes`;
- validate exact channel, match-type, and payload identities without
  normalization, redirect following, case correction, or scalar coercion;
- bound every static source-search input and response dimension;
- report lexical candidates as static evidence rather than runtime truth;
- register no listeners, broadcast no messages, run no PIE, and modify no
  package or asset;
- compile and pass focused automation on UE 5.7 and UE 5.8; and
- exclude security, benchmark, invocation-log, action-metadata,
  reinforcement-learning, execution-policy, and search-planning features.

---

## 2. Action Surface

| Action | Contract |
|---|---|
| `gameplay_message.get_status` | Reports exact optional-plugin, module, subsystem, async-listener, listener-handle, and match-enum availability. |
| `gameplay_message.describe_listener_contract` | Reports reflected listener/broadcast functions, supported match types, and channel/payload/lifetime rules. |
| `gameplay_message.validate_message_struct` | Loads one exact non-redirecting `UScriptStruct` object path and validates requested metadata/property constraints. |
| `gameplay_message.validate_channel_contract` | Validates canonical channel syntax, exact registration/case, exact match-type spelling, and an optional exact payload struct. |
| `gameplay_message.trace_channel_usage` | Performs bounded lexical source analysis for broadcaster/listener candidates and inferred channel/payload/match relationships. |

`MonolithGameplayMessage` owns registration and shutdown unregistration for the
new namespace. The module depends only on core editor/runtime reflection and
project-discovery modules; optional GameplayMessageRouter types are resolved
from exact reflected paths.

---

## 3. Fail-Closed Contract

| Contract | Result |
|---|---|
| JSON scalar types | PASS — declared string, boolean, integer, and array values require the corresponding raw JSON type. String-encoded booleans/integers and fractional integers fail with `-32602`. |
| Integer limits | PASS — `max_files` is `1..5000`; `max_results` is `1..1000`; invalid values are rejected and never clamped. |
| Object identity | PASS — only canonical object paths are accepted. Whitespace, backslashes, extensions, subobjects, redirects, case changes, and resolved-path substitutions fail. |
| Channel syntax | PASS — tags must pass the engine validator, contain no whitespace, and contain no empty dot-delimited segment. |
| Channel registration | PASS — registered tags require exact spelling/case by default; unregistered preflight is explicit through `require_registered_tag=false`. |
| Match type | PASS — only case-sensitive `ExactMatch` and `PartialMatch` values are accepted. |
| Default source roots | PASS — current-project `Source` and eligible project-plugin source directories only. |
| Explicit source roots | PASS — roots must exist under the current project's `Source` or `Plugins` directory. |
| Engine source | PASS — opt-in is limited to the installed `GameplayMessageRouter/Source` directory. |
| Source bounds | PASS — 256 roots, 5,000 files, 2 MiB per file, 1,000 results, 32 candidates per line, and 1,000 issues are hard limits with explicit truncation metadata. |
| Runtime claim | PASS — output declares `analysis_mode="bounded_static_source"` and `runtime_execution="not_performed"` and lists reachability/lifetime/delivery limitations. |
| Mutation | PASS — handlers do not call listener registration, broadcast, compile, save, modify, dirty, or package-creation APIs. The `RegisterListener(` occurrence in the trace implementation is a bounded lexical search token, not a function call. |

---

## 4. Verification Environment and Source Identity

| Engine | Engine association | Resolved engine root | Host project | Plugin source |
|---|---|---|---|---|
| UE 5.7 | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\MonolithGameplayMessageUE57Host\MonolithGameplayMessageUE57Host.uproject` | Physical copy under `D:\P4\MonolithGameplayMessageUE57Host\Plugins\Monolith` |
| UE 5.8 | `5.8` | `D:\Engine\UE_5.8` | `D:\P4\MonolithGameplayMessageUE58Host\MonolithGameplayMessageUE58Host.uproject` | Physical copy under `D:\P4\MonolithGameplayMessageUE58Host\Plugins\Monolith` |

Each engine root was resolved from the corresponding host project's
`EngineAssociation` and the registered installed-engine root. The hosts use
independent physical plugin copies so UHT records, import libraries, and DLLs
cannot cross-contaminate engine versions.

The final worktree, UE 5.7 host, and UE 5.8 host matched for all 14 build inputs
(the descriptor plus every module/source/test file). Representative final
SHA-256 values:

```text
Monolith.uplugin
690DC76F7C77BEAA9011CD6A25985A77CFE1E3F30D482BAD50171C473C1A9807

MonolithGameplayMessageActions.cpp
32817AB9E83807410D629F4D8AE85C5D2455807FFA4DFB0F9F87F60A88DB1B7C

MonolithGameplayMessageCommon.cpp
B500503BCF58168DBF1D367240676DF6D8B29E681DF55FBDB1E8F5933CAAFB6D

MonolithGameplayMessageTrace.cpp
AC37823E91C8C919A573254B8AAB99BA65A50C7C77EE2E1B7EF0E6D4B987F581

MonolithGameplayMessageParamGuardTests.cpp
364E8D5282EE3723DF1EA43DABDF18DA4ABE6C778EA5803AF914FC4B282B70FF
```

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | Physically isolated full editor-target build | PASS — 452/452 actions explicitly compiled all four implementation files and four test files, then linked `UnrealEditor-MonolithGameplayMessage.dll`. | `D:\P4\MonolithGameplayMessageUE57Host\Build-UE57-Initial-20260730.log` |
| UE 5.7 | Final root-fix relink | PASS — 6/6 actions recompiled the common/handler/param-guard files and freshly linked the affected DLL with `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE57Host\Build-UE57-GameplayMessage-20260730-050748.log` |
| UE 5.7 | Final canonical-trace relink | PASS — 7/7 actions recompiled the shared validator, handler, trace, and param-guard test, then freshly linked the affected DLL with `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE57Host\Build-UE57-GameplayMessage-CanonicalTrace-20260730.log` |
| UE 5.7 | Final affected DLL | PASS — 280,576 bytes, SHA-256 `7FCAAFF1DA571B253025227709A4465DC5753375269A9BC302D8D8DD30169E6A`. | `D:\P4\MonolithGameplayMessageUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithGameplayMessage.dll` |
| UE 5.8 | Physically isolated full editor-target build | PASS — 452/452 actions explicitly compiled all four implementation files and four test files, then linked the affected DLL with `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE58Host\Build-UE58-GameplayMessage-20260730-050838.log` |
| UE 5.8 | Final canonical-trace relink | PASS — 7/7 actions recompiled the shared validator, handler, trace, and param-guard test, then freshly linked the affected DLL with `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE58Host\Build-UE58-GameplayMessage-CanonicalTrace-20260730.log` |
| UE 5.8 | Final affected DLL | PASS — 261,632 bytes, SHA-256 `6D6B0C92E068B0FD6FE6851E8BE970D2708CE419A51FAA9D4ACBFDF41CC1D0A3`. | `D:\P4\MonolithGameplayMessageUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithGameplayMessage.dll` |

The UE 5.8 full build emitted deprecation warnings from pre-existing modules.
The new GameplayMessage module emitted no compiler warning or error.

---

## 6. Automation Results and Root Fixes

Both engines ran one fresh process with:

```text
-ExecCmds="Automation RunTests Monolith.GameplayMessage"
-TestExit="Automation Test Queue Empty"
```

| Engine | Started | Succeeded | Failed | Queue-empty marker | TestExit marker | Accepted log |
|---|---:|---:|---:|---:|---:|---|
| UE 5.7 | 4 | 4 | 0 | present | present | `D:\P4\MonolithGameplayMessageUE57Host\GameplayMessage-UE57-CanonicalTrace-Direct-20260730.log` |
| UE 5.8 | 4 | 4 | 0 | present | present | `D:\P4\MonolithGameplayMessageUE58Host\GameplayMessage-UE58-CanonicalTrace-20260730.log` |

The four tests are:

1. `Monolith.GameplayMessage.BoundedSourceTrace`
2. `Monolith.GameplayMessage.ParamGuards`
3. `Monolith.GameplayMessage.RegistryAndSchemas`
4. `Monolith.GameplayMessage.ValidationContracts`

The first UE 5.7 automation run correctly exposed three contract defects:

- `TJsonValueString::TryGetBool` accepts string-encoded booleans;
- `FString` equality operators compare case-insensitively; and
- the engine's default gameplay-tag validator does not reject internal
  whitespace.

The root fix checks exact `EJson` types, uses explicit case-sensitive string
comparison, and adds canonical no-whitespace/no-empty-segment tag validation.
The final param-guard suite also covers string-encoded integers, empty tag
segments, and canonical `trace_channel_usage.channel_tag` filters. The original
failing log is retained at
`D:\P4\MonolithGameplayMessageUE57Host\GameplayMessage-UE57-20260730-050030.log`;
the final 4/4 runs supersede it.

An attempted final rerun appended `; Quit` to `-ExecCmds`. UE 5.8 returned
process exit code 0 but stopped immediately after opening the automation test
session, so that log was rejected rather than counted as proof. `Quit` races
the asynchronous automation queue; removing it and letting `-TestExit` own
termination produced complete 4/4 evidence on both engines. The rejected logs
are `GameplayMessage-UE57-FinalDescriptor-20260730.log` and
`GameplayMessage-UE58-FinalDescriptor-20260730.log`.

Two later UE 5.7 full-suite attempts used `Start-Process -Wait` after the
canonical trace-filter fix. Each process returned exit code 0 before the
accepted evidence contract was complete: one log stopped after the first test
and the other stopped while starting the second, with neither a queue-empty
termination nor a `TestExit` request. Those rejected logs are
`GameplayMessage-UE57-CanonicalTrace-20260730.log` and
`GameplayMessage-UE57-CanonicalTrace-Retry-20260730.log`.

The accepted UE 5.7 rerun invoked `UnrealEditor-Cmd.exe` directly through
PowerShell's native `&` operator, which retained ownership through all four
tests and the final log flush. Four individual one-test UE 5.7 runs also
completed independently, but the direct full-suite log above is the primary
acceptance evidence. Process exit code alone is not treated as automation
proof.

---

## 7. Catalog and Static Gates

The latest upstream generator was run independently against the exact clean
fork base and the GameplayMessage worktree.

| Gate | Result |
|---|---|
| Generator tests | PASS — 4/4. |
| Base catalog | PASS — 1,561 actions across 24 namespaces. |
| Target catalog | PASS — 1,566 actions across 25 namespaces; semantic source hash `92fa447e3d2285a819e2eaa1aff5522a7ae7b15e6b167bc95f74afa8ee8adc40` (`action_semantics_v1`). |
| Exact delta | PASS — five additions, all under `gameplay_message`; zero removals. |
| Duplicate full names | PASS — zero. |
| Final GameplayMessage roster | PASS — exactly five actions. |
| Latest checker self-test | PASS. |
| Base-vs-target static parity | PASS — base 8 blockers/11 advisories; target 8/11; new findings zero and resolved findings zero. |
| Diff hygiene | PASS — `git diff --check` reports no whitespace error. |
| Excluded-feature scan | PASS — zero security, benchmark, invocation-log, action-metadata, reinforcement-learning, execution-policy, or search-planning implementation matches in `Source\MonolithGameplayMessage`. |

The fork does not contain the latest upstream hosted-static-CI workflow or all
of its auxiliary systems. The latest checker therefore ran against the clean
base and target with one identical temporary configuration. Proxy,
analyzer/invocation-log, benchmark, offline parity/catalog, and skill-drift
checks were disabled because those systems are absent or explicitly excluded;
repository-wide CRLF findings were symmetrically allowlisted.

The identical remaining findings are pre-existing: seven Niagara high-risk
registrations lack the latest checker marker, the hosted static workflow is
absent, ten Niagara raw-parameter/direct-load advisories remain, and
`.claude/agents` is an external prerequisite.

---

## 8. Visual and Discord Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This change adds headless editor action handlers, schemas, tests, docs, and a routing skill. It does not change gameplay, runtime/editor UI, VFX, animation presentation, materials, or project assets. |
| Discord screenshot upload | N/A | No visual artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 9. Result

PASS. The fork gains exactly five bounded, read-only GameplayMessageRouter
contract diagnostics through an isolated module. The same source compiles and
links on UE 5.7 and UE 5.8, passes 4/4 focused tests on each engine under the
non-racy `TestExit` termination contract with complete log markers, changes the
generated catalog only by the intended five actions, introduces no new static
finding, and adds none of the excluded feature classes.
