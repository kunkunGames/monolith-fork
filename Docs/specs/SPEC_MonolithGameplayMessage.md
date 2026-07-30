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
| `gameplay_message.describe_listener_contract` | none | Reports reflected listener/broadcast functions, supported match types, lifetime rules, and the runtime-compatible payload rule: a broadcast struct must equal or derive from the listener's accepted parent struct. |
| `gameplay_message.validate_message_struct` | `message_struct`; optional `require_blueprint_type=false`, `require_no_object_references=false` | Loads one exact object path without redirects, verifies `UScriptStruct` identity, and reports metadata, `structure_size`, property counts, recursive object-reference-bearing property counts, checks, and issues. |
| `gameplay_message.validate_channel_contract` | `channel_tag`; optional `message_struct`, `match_type=ExactMatch`, `require_registered_tag=true`, `require_blueprint_type=false` | Validates canonical tag syntax, exact registration spelling/case, exact match-type spelling, and an optional exact payload struct. |
| `gameplay_message.trace_channel_usage` | optional `channel_tag`, `source_root`, `source_roots`, `include_monolith_source=false`, `include_engine_gameplay_message_sources=false`, `max_files=2000`, `max_results=500`, `include_line_text=false` | Scans bounded project/plugin source text call by call, groups inferred channel/payload/match relationships, and reports mismatch/orphan/ambiguity candidates plus explicit truncation and absence-analysis metadata. |

---

## 4. Input and identity contracts

| Contract | Requirement |
| --- | --- |
| JSON scalar types | Strings, booleans, and integers must arrive as their declared JSON type. String-encoded booleans/integers are rejected with `-32602`. |
| Integer ranges | `max_files` accepts only integral values in `1..5000`; `max_results` accepts only integral values in `1..1000`. Values are never clamped. |
| Object identity | Paths must pass `FPackageName::IsValidObjectPath`, use exact case and spelling, and resolve without redirects, extensions, backslashes, subobjects, whitespace, or alternate-object substitution. |
| Channel syntax | Single-segment and hierarchical tags are accepted when they pass the engine validator. Tags must contain no whitespace and no leading, trailing, or internal empty dot-delimited segment. |
| Registration | `require_registered_tag=true` requires an exact registered tag and exact returned spelling/case. `false` permits unregistered preflight while preserving syntax validation. |
| Match type | Only case-sensitive `ExactMatch` and `PartialMatch` values are accepted. |

Validation findings are returned as structured `checks` and `issues`. A syntactically valid request can execute successfully while returning `ok=false` for a failed content contract; malformed request parameters return JSON-RPC invalid params.

---

## 5. Bounded static trace

| Boundary | Limit or rule |
| --- | --- |
| Default roots | Current project `Source` plus every discovered `EPluginType::Project` source directory, including plugins nested below containers such as `Plugins/GameFeatures`. |
| Explicit roots | Must exist lexically and physically under the current project's `Source` or `Plugins` directory. Junction/symlink targets outside the project boundary are rejected, and recursive traversal rechecks each physical path. |
| Monolith source | Excluded unless `include_monolith_source=true`. The exclusion root is derived from the discovered Monolith plugin's base directory, so it holds when the plugin is installed below a grouping directory such as `Plugins/Developer/Monolith`. |
| Engine source | Limited to the installed `GameplayMessageRouter/Source` directory and opt-in only. |
| Roots | Maximum 256. Duplicate canonical roots are removed, and a root nested inside another accepted root is dropped in favour of the outermost one, so overlapping inputs such as `Source` plus `Source/Game` cannot scan the same file twice. |
| Files | Default selected count 2,000; hard selected maximum 5,000; supported extensions are `.cpp`, `.h`, `.hpp`, `.inl`. Eligible files are deduplicated and sorted by normalized path before the requested prefix is selected, so `max_files` is deterministic. Enumeration fails explicitly above the separate 100,000-file safety bound rather than returning a filesystem-order-dependent subset. |
| File size | Files larger than 2 MiB are skipped and counted. |
| Results | Default 500; hard maximum 1,000. |
| Per-call candidates | Maximum 32 from the call's declared channel argument; truncation is counted. Each pattern declares both the channel argument and, for listeners, the match-type argument, so a callback named `OnPartialMatch` cannot change the inferred contract and the Blueprint async listener reads its second argument rather than the world context object. Multiple supported calls on one source line are parsed independently so channels do not inherit another call's payload or match type. |
| Lexical hygiene | Inline and multi-line comments are removed while preserving source positions. Supported call tokens require a C++ identifier boundary, and scoped constants retain their complete qualifier (for example `Combat::TAG_Event`), preventing commented, prefixed, or same-leaf cross-namespace false matches. |
| Issues | Maximum 1,000; truncation is reported. |
| Text output | Both `line_text` and `function_context` are omitted by default and bounded when explicitly requested with `include_line_text=true`. |
| Absence claims | Orphan broadcaster/listener candidates are emitted only after a complete scan. File/result limits, skipped files, unreadable files, or candidate truncation set `orphan_analysis_complete=false`, mark channel rows `indeterminate`, zero orphan counts, and emit `absence_analysis_indeterminate`. |
| Counterpart matching | A `PartialMatch` listener on an ancestor tag counts as the counterpart for every descendant broadcaster, matching router delivery, so neither side is reported as an orphan. A filtered child-channel request retains the relevant ancestor listener row instead of discarding the evidence before counterpart analysis. |
| Unresolved channels | Calls whose channel is held in a variable share the synthetic `<unresolved>` key. That key never participates in payload-mismatch, orphan, or match-ambiguity diagnostics, because those rows are unrelated to one another. |

The response declares `analysis_mode="bounded_static_source"` and `runtime_execution="not_performed"`. Literal channel extraction validates the complete first argument and accepts a valid root tag such as `"Message"`; prefixed constants such as `TAG_...` remain lexical candidates. `channel_graph`, `broadcasters`, `listeners`, and `issues` are lexical candidates only; they do not prove branch reachability, listener lifetime, live registration, message delivery, or runtime payload compatibility.

---

## 6. Output contracts

| Action family | Key output |
| --- | --- |
| Status/contract | Exact plugin/module/reflection rows, reflected functions, match types, and listener-contract rows. |
| Validation | `ok`, target summary including `structure_size`, `object_property_count`, and `object_reference_scan_recursive`, `checks[]`, `issues[]`, and exact requested/resolved identity fields. |
| Trace | `analysis_mode`, `runtime_execution`, `files_selected`, actual `files_scanned`, `limits.eligible_files_enumerated`, `limits`, `summary.orphan_analysis_complete`, `source_roots`, `patterns`, `counts_by_code`, `counts_by_role`, per-channel `orphan_analysis_status`, `channel_graph`, `broadcasters`, `listeners`, `matches`, `checks`, `issues`, `limitations`. |

All free-form output text and source excerpts are bounded before serialization.

---

## 7. Verification gates

| Gate | Required result |
| --- | --- |
| Registry | Exactly five `gameplay_message` actions with required/default schema fields. |
| Param guards | Wrong JSON scalar types, non-canonical object paths/tags (including leading/trailing empty segments), case-mismatched match types, fractional/out-of-range limits, missing roots, and lexical or physical out-of-project roots fail explicitly. |
| Validation | Exact native `UScriptStruct` readback includes positive structure size; nested container object references are found recursively; missing/wrong-type object diagnostics execute without substitution. |
| Trace | Fixture proves paired broadcaster/listener discovery, nested project-plugin roots, independent same-line calls, single-segment literal tags, filtered ancestor `PartialMatch` evidence, qualified constants, comment/token-boundary rejection, argument-local match inference, deterministic `max_files`, actual scanned-file counts, opt-in-only source excerpts, and truncation-safe indeterminate orphan analysis. |
| UE 5.7 build/test | `UnrealEditor-MonolithGameplayMessage.dll` links and `Monolith.GameplayMessage` passes 4/4. |
| UE 5.8 build/test | `UnrealEditor-MonolithGameplayMessage.dll` links and `Monolith.GameplayMessage` passes 4/4. |
| Catalog | Generated catalog adds exactly the five actions and removes none. |
| Exclusions | No security, benchmark, invocation-log, metadata/RL, execution-policy, or search-planning capability is added. |

Verification evidence is recorded in [2026-07-30-gameplay-message-contract-actions.md](../testing/2026-07-30-gameplay-message-contract-actions.md).
