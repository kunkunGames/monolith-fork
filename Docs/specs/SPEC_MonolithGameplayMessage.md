# Monolith — MonolithGameplayMessage Module

| Field | Value |
| --- | --- |
| Parent | [SPEC_CORE.md](../SPEC_CORE.md) |
| Module | `MonolithGameplayMessage` |
| Namespace | `gameplay_message` |
| Type | Editor |
| Engine floor | Unreal Engine 5.7 |
| Version | 0.21.3 (Beta) |
| Status | Current |

---

## 1. Purpose

`MonolithGameplayMessage` provides five reusable read-only diagnostics for Unreal projects that use Epic's `GameplayMessageRouter` plugin. It reports runtime contract availability, validates canonical channel/payload contracts, and performs bounded static source tracing without linking against or mutating the runtime plugin.

---

## 2. Module boundary

| Area | Contract |
| --- | --- |
| Dependencies | `Core`, `CoreUObject`, `Engine`, `GameplayTags`, `MonolithCore`, `Json`, `Projects` |
| Runtime isolation | No compile-time dependency on `GameplayMessageRuntime` or `GameplayMessageNodes`; optional plugin types are resolved by exact reflection paths. |
| Registration | `FMonolithGameplayMessageModule::StartupModule` registers exactly five `gameplay_message` actions; shutdown unregisters the namespace. |
| Mutability | Every action is read-only. The module does not register listeners, broadcast messages, run PIE, load redirect targets, or save assets. |
| Portability | The implementation contains no Speed-, Lyra-, or project-specific class dependency. |

---

## 3. Actions

| Action | Params | Behavior |
| --- | --- | --- |
| `gameplay_message.get_status` | none | Reports exact `GameplayMessageRouter` plugin, `GameplayMessageRuntime`/`GameplayMessageNodes` module, subsystem/async-listener class, listener-handle struct, and match-enum availability. |
| `gameplay_message.describe_listener_contract` | none | Reports reflected listener/broadcast functions, supported match types, and the shared channel/payload/lifetime contract. |
| `gameplay_message.validate_message_struct` | `message_struct`; optional `require_blueprint_type=false`, `require_no_object_references=false` | Loads one exact object path without redirects, verifies `UScriptStruct` identity, and reports metadata, size, property counts, object-reference counts, checks, and issues. |
| `gameplay_message.validate_channel_contract` | `channel_tag`; optional `message_struct`, `match_type=ExactMatch`, `require_registered_tag=true`, `require_blueprint_type=false` | Validates canonical tag syntax, exact registration spelling/case, exact match-type spelling, and an optional exact payload struct. |
| `gameplay_message.trace_channel_usage` | optional `channel_tag`, `source_root`, `source_roots`, `include_monolith_source=false`, `include_engine_gameplay_message_sources=false`, `max_files=2000`, `max_results=500`, `include_line_text=false` | Scans bounded project/plugin source text for broadcaster/listener call-site candidates, groups inferred channel/payload/match relationships, and reports mismatch/orphan/ambiguity candidates plus explicit truncation metadata. |

---

## 4. Input and identity contracts

| Contract | Requirement |
| --- | --- |
| JSON scalar types | Strings, booleans, and integers must arrive as their declared JSON type. String-encoded booleans/integers are rejected with `-32602`. |
| Integer ranges | `max_files` accepts only integral values in `1..5000`; `max_results` accepts only integral values in `1..1000`. Values are never clamped. |
| Object identity | Paths must pass `FPackageName::IsValidObjectPath`, use exact case and spelling, and resolve without redirects, extensions, backslashes, subobjects, whitespace, or alternate-object substitution. |
| Channel syntax | Tags must pass the engine validator, contain no whitespace, and contain no empty dot-delimited segment. |
| Registration | `require_registered_tag=true` requires an exact registered tag and exact returned spelling/case. `false` permits unregistered preflight while preserving syntax validation. |
| Match type | Only case-sensitive `ExactMatch` and `PartialMatch` values are accepted. |

Validation findings are returned as structured `checks` and `issues`. A syntactically valid request can execute successfully while returning `ok=false` for a failed content contract; malformed request parameters return JSON-RPC invalid params.

---

## 5. Bounded static trace

| Boundary | Limit or rule |
| --- | --- |
| Default roots | Current project `Source` plus eligible project plugin source directories. |
| Explicit roots | Must exist under the current project's `Source` or `Plugins` directory. |
| Monolith source | Excluded unless `include_monolith_source=true`. |
| Engine source | Limited to the installed `GameplayMessageRouter/Source` directory and opt-in only. |
| Roots | Maximum 256. Duplicate canonical roots are removed. |
| Files | Default 2,000; hard maximum 5,000; supported extensions are `.cpp`, `.h`, `.hpp`, `.inl`. |
| File size | Files larger than 2 MiB are skipped and counted. |
| Results | Default 500; hard maximum 1,000. |
| Per-line candidates | Maximum 32; truncation is counted. |
| Issues | Maximum 1,000; truncation is reported. |
| Text output | Source-line text is omitted by default and bounded when explicitly requested. |

The response declares `analysis_mode="bounded_static_source"` and `runtime_execution="not_performed"`. `channel_graph`, `broadcasters`, `listeners`, and `issues` are lexical candidates only; they do not prove branch reachability, listener lifetime, live registration, message delivery, or runtime payload compatibility.

---

## 6. Output contracts

| Action family | Key output |
| --- | --- |
| Status/contract | Exact plugin/module/reflection rows, reflected functions, match types, and listener-contract rows. |
| Validation | `ok`, target summary, `checks[]`, `issues[]`, and exact requested/resolved identity fields. |
| Trace | `analysis_mode`, `runtime_execution`, `limits`, `summary`, `source_roots`, `patterns`, `counts_by_code`, `counts_by_role`, `channel_graph`, `broadcasters`, `listeners`, `matches`, `checks`, `issues`, `limitations`. |

All free-form output text and source excerpts are bounded before serialization.

---

## 7. Verification gates

| Gate | Required result |
| --- | --- |
| Registry | Exactly five `gameplay_message` actions with required/default schema fields. |
| Param guards | Wrong JSON scalar types, non-canonical object paths/tags, case-mismatched match types, fractional/out-of-range limits, missing roots, and out-of-project roots fail explicitly. |
| Validation | Exact native `UScriptStruct` readback and missing/wrong-type object diagnostics execute without substitution. |
| Trace | Fixture finds one broadcaster and one listener, respects result truncation, and omits line text by default. |
| UE 5.7 build/test | `UnrealEditor-MonolithGameplayMessage.dll` links and `Monolith.GameplayMessage` passes 4/4. |
| UE 5.8 build/test | `UnrealEditor-MonolithGameplayMessage.dll` links and `Monolith.GameplayMessage` passes 4/4. |
| Catalog | Generated catalog adds exactly the five actions and removes none. |
| Exclusions | No security, benchmark, invocation-log, metadata/RL, execution-policy, or search-planning capability is added. |

Verification evidence is recorded in [2026-07-30-gameplay-message-contract-actions.md](../testing/2026-07-30-gameplay-message-contract-actions.md).
