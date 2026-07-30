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
| Channel syntax | PASS — valid single-segment or hierarchical tags pass; whitespace and leading, trailing, or internal empty dot-delimited segments fail. |
| Channel registration | PASS — registered tags require exact spelling/case by default; unregistered preflight is explicit through `require_registered_tag=false`. |
| Match type | PASS — only case-sensitive `ExactMatch` and `PartialMatch` values are accepted. |
| Default source roots | PASS — current-project `Source` and every discovered `EPluginType::Project` source directory, including nested `Plugins/GameFeatures/...` plugins. |
| Explicit source roots | PASS — roots must exist lexically and physically under the current project's `Source` or `Plugins` directory; junction/symlink escapes fail. |
| Engine source | PASS — opt-in is limited to the installed `GameplayMessageRouter/Source` directory. |
| Source bounds | PASS — 256 roots, 100,000 eligible files enumerated, 5,000 path-sorted files selected, 2 MiB per file, 1,000 results, 32 candidates per call, and 1,000 issues are hard limits with explicit truncation metadata. Incomplete scans suppress orphan claims and report indeterminate absence analysis. |
| Runtime claim | PASS — output declares `analysis_mode="bounded_static_source"` and `runtime_execution="not_performed"` and lists reachability/lifetime/delivery limitations. |
| Mutation | PASS — handlers do not call listener registration, broadcast, compile, save, modify, dirty, or package-creation APIs. The `RegisterListener(` occurrence in the trace implementation is a bounded lexical search token, not a function call. |

---

## 3.1. AI Review Remediation

The initial ten actionable review findings and every subsequent source-trace
accuracy finding were reproduced as contract gaps and closed in production
code plus focused regression assertions.

| Review finding | Root fix | Regression evidence |
|---|---|---|
| Container/nested object references were missed | Each top-level `FProperty` now calls recursive `ContainsObjectReference` with the complete strong/weak/soft/conservative flag set shared by UE 5.7 and UE 5.8. | `/Script/Engine.PooledCameraShakes` fails `require_no_object_references=true` because its `TArray<TObjectPtr<UCameraShakeBase>>` is detected. |
| Nested project plugins were omitted | Default roots come from sorted `IPluginManager::GetDiscoveredPlugins()` project plugins rather than one-level directory enumeration; relative plugin bases are canonicalized before validation. | Both hosts mount and discover `Plugins/GameFeatures/MonolithNestedTraceFixture`, and its unique channel is found without an explicit root. |
| Listener payload compatibility was overstated | The contract now matches `UGameplayMessageSubsystem`: the broadcast struct may equal or derive from the listener's accepted parent struct. | `ValidationContracts` asserts the equal-or-derived/accepted-parent wording. |
| Truncation produced false orphan findings | Orphan flags and issues require complete absence analysis; limits/skips/truncation set `orphan_analysis_complete=false`, zero orphan counts, and emit `absence_analysis_indeterminate`. | `max_results=1` proves no orphan flag/issue is emitted and every channel reports `indeterminate`. |
| `function_context` leaked when excerpts were disabled | Both `function_context` and `line_text` are serialized only for `include_line_text=true`; function context is not inferred otherwise. | Default and opt-in trace calls assert absence/presence of both fields. |
| Multiple calls on one line cross-bound channels and payloads | The scanner iterates balanced call expressions, extracts only that call's first argument, and binds template/static payload plus match type per call. | Two broadcasts on one line independently report `MultiA`/payload A and `MultiB`/payload B. |
| Leading/trailing empty tag segments passed | Canonical validation now rejects `StartsWith(".")`, `EndsWith(".")`, and `Contains("..")`. | Param guards cover `.Monolith.GameplayMessage`, `Monolith.GameplayMessage.`, and `Monolith..GameplayMessage`. |
| Lexical path checks allowed junction escapes | Root and allowed-directory identities are resolved through `IFileManager::GetFilenameOnDisk`; recursive enumeration rechecks each physical path against its root. | Each host exposes a project-local junction to an external fixture, and the root is rejected with invalid params. |
| Single-segment literal tags were ignored | Quoted first-argument candidates use the canonical tag validator instead of requiring a dot. | Literal `"Message"` is found with its exact payload. |
| Promised struct size was absent | `message_struct.structure_size` now reports `UScriptStruct::GetStructureSize()`. | `GameplayTagContainer` asserts a positive structure size on both engines. |
| A filtered child channel discarded an ancestor partial listener | Filter acceptance now keeps a listener row when the requested channel is a strict descendant and the listener's argument-local match type is `PartialMatch`. | Filtering `Message.Child` returns both its broadcaster and the `Message` partial listener, with zero false-orphan counts. |
| Scoped tag constants collapsed to the same leaf | Constant extraction walks complete C++ `::` qualifier chains while retaining strict token boundaries. | `Combat::TAG_Event` and `UI::TAG_Event` remain separate channel candidates. |
| Calls inside inline or block comments were treated as live | A quote-aware line sanitizer removes `//` and multi-line `/* ... */` regions while preserving line/column positions. | Inline and multi-line commented fixture calls are absent from results. |
| Callback names containing `PartialMatch` changed listener semantics | Each listener pattern declares its match argument and only exact enum tokens in that argument are recognized. | `&ThisClass::OnPartialMatch` with no match argument reports `ExactMatch(default)`. |
| `max_files` depended on filesystem enumeration order | Eligible paths are deduplicated, fully sorted, then truncated; enumeration above 100,000 fails explicitly. | `max_files=1` always selects `AA_DeterministicFirst.inl`, never `ZZ_DeterministicLast.inl`. |
| Larger identifiers containing a supported call token produced false matches | Every trace token now requires a C++ identifier boundary while the explicit `K2_BroadcastMessage` pattern still owns its exact call. | `CanBroadcastMessage` and `TryRegisterListener` fixtures produce no matches. |
| `files_scanned` counted selected paths that were never opened | The scanner increments `files_scanned` only after successful load and separately reports `files_selected`. | `max_results=1` reports three selected fixture files but only the first actually scanned file. |

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

The final worktree, UE 5.7 host, and UE 5.8 host matched for all 16 build inputs
(the descriptor plus every module/source/test file). Representative final
SHA-256 values:

```text
Monolith.uplugin
690DC76F7C77BEAA9011CD6A25985A77CFE1E3F30D482BAD50171C473C1A9807

MonolithGameplayMessageActions.cpp
FDD5A7059D901CE65DD5FA22A0701D3ADD619737921C5A168D10B7AB576AB9A8

MonolithGameplayMessageCommon.cpp
D57719FF6B7A16A8049CAD38216BDFFC3326DDCECF114FB2FF9B4E994E977AC3

MonolithGameplayMessageTrace.cpp
8375F934185C2AB5762913312FD29EC1AF4C964836D25B033FBE7020B9B303AC

MonolithGameplayMessageParamGuardTests.cpp
408EA679C511A58E1B552505E72BE1EFCA08E96DA7523FA802F38773D8E39B39

MonolithGameplayMessageTraceTests.cpp
36BF6565E19862F0D6C9AD0B34FB4FDA9B97676F54A58A5859BC208CB60BE3E9

MonolithGameplayMessageValidationTests.cpp
2039A3A5847BA6E85AB901E1839F6F1F2852A10BA5C3CA004E1C317A95CD3759

AA_DeterministicFirst.inl
511C10DDA130D58E9DA83204B5157A034B94AD03D20B6F96061A764AF820A3B5

GameplayMessageTraceFixture.inl
A3A80191684861627A57253A1E7974E3C5DEE724CB0F9ADED8C5C82382FD2374

ZZ_DeterministicLast.inl
944E8ABE4E6C080B99E1DA915616875B82F35C643640596AC79987C41D662562
```

---

## 5. Build Results

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | Physically isolated full editor-target build | PASS — 452/452 actions explicitly compiled all four implementation files and four test files, then linked `UnrealEditor-MonolithGameplayMessage.dll`. | `D:\P4\MonolithGameplayMessageUE57Host\Build-UE57-Initial-20260730.log` |
| UE 5.7 | Final source-trace review build | PASS — exact 16-input source identity, `-NoEngineChanges`, 411/411 actions, all eight GameplayMessage implementation/test translation units compiled, two affected library/DLL link actions, and `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE57Host\Build-UE57-GameplayMessage-FinalReview-20260731.log` |
| UE 5.7 | Final affected DLL | PASS — 344,064 bytes, SHA-256 `D80493DC2FBB6A23FCB3E91B03C11F372C44C412484C0D9A0ACEF40F228B89E9`. | `D:\P4\MonolithGameplayMessageUE57Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithGameplayMessage.dll` |
| UE 5.8 | Physically isolated full editor-target build | PASS — 452/452 actions explicitly compiled all four implementation files and four test files, then linked the affected DLL with `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE58Host\Build-UE58-GameplayMessage-20260730-050838.log` |
| UE 5.8 | Final source-trace review build | PASS — exact 16-input source identity, `-NoEngineChanges`, 411/411 actions, all eight GameplayMessage implementation/test translation units compiled, two affected library/DLL link actions, and `Result: Succeeded`. | `D:\P4\MonolithGameplayMessageUE58Host\Build-UE58-GameplayMessage-FinalReview-20260731.log` |
| UE 5.8 | Final affected DLL | PASS — 323,072 bytes, SHA-256 `ECB0598FF865FFB582B2B29D44E6BFB0950CB5CCF80546B56DE9E712388D7D18`. | `D:\P4\MonolithGameplayMessageUE58Host\Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithGameplayMessage.dll` |

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
| UE 5.7 | 4 | 4 | 0 | present | present | `D:\P4\MonolithGameplayMessageUE57Host\GameplayMessage-UE57-FinalReview-Console-20260731.log` |
| UE 5.8 | 4 | 4 | 0 | present | present | `D:\P4\MonolithGameplayMessageUE58Host\GameplayMessage-UE58-FinalReview-Console-20260731.log` |

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
The final suites additionally cover every review remediation listed in
Section 3.1, including deterministic path selection and exact selected-versus-
scanned counters. Both accepted commands omit a queued `Quit` command and let
`-TestExit="Automation Test Queue Empty"` own termination. Acceptance requires
all four named success markers, zero failure markers, the `4 tests performed`
queue-empty marker, and the final `TestExit` marker; process exit code alone is
not treated as proof.

The UE 5.7 accepted evidence is the direct stdout capture because that engine's
separate `-abslog` file can stop flushing before the final automation messages
even though the owned process and stdout stream complete. The console capture
contains all four success rows plus both final markers. Both hosts also log a
pre-existing `127.0.0.1:9316` bind error because another local Monolith endpoint
owns the configured port; the focused tests execute the registry directly and
do not depend on that HTTP listener.

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
non-racy `TestExit` termination contract with complete log markers, and closes
every reproduced review finding with executable regressions. The generated catalog
changes only by the intended five actions, introduces no new static finding,
and adds none of the excluded feature classes.
