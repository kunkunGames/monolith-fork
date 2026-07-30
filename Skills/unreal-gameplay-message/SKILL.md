---
name: unreal-gameplay-message
description: Use when inspecting Unreal GameplayMessageRouter contracts via Monolith MCP — plugin/runtime availability, channel gameplay tags, ExactMatch/PartialMatch behavior, payload UScriptStruct validation, and bounded static broadcaster/listener source tracing. Read-only; never registers listeners, broadcasts messages, runs PIE, or edits assets.
---

# Unreal Gameplay Message

Use the `gameplay_message` namespace for read-only GameplayMessageRouter contract inspection. The module uses reflection and source text only; it has no compile-time dependency on `GameplayMessageRuntime` or `GameplayMessageNodes`.

## Discover first

Confirm action names and the exact live schema before calling:

```text
monolith_discover({ namespace: "gameplay_message", mode: "actions" })
monolith_discover({ namespace: "gameplay_message", action: "validate_channel_contract", mode: "schema" })
monolith_discover({ namespace: "gameplay_message", action: "trace_channel_usage", mode: "schema" })
```

## Action reference

| Action | Params | Use |
| --- | --- | --- |
| `get_status` | none | Check exact `GameplayMessageRouter` plugin, runtime/nodes modules, subsystem class, async-listener class, listener-handle struct, and match-enum availability. |
| `describe_listener_contract` | none | Summarize reflected listener/broadcast functions and the runtime payload rule: a broadcast struct must equal or derive from the listener's accepted parent struct. |
| `validate_message_struct` | `message_struct*`, `require_blueprint_type?=false`, `require_no_object_references?=false` | Validate one exact payload `UScriptStruct` object path, report its structure size, and apply metadata plus recursive object-reference constraints. |
| `validate_channel_contract` | `channel_tag*`, `message_struct?`, `match_type?=ExactMatch`, `require_registered_tag?=true`, `require_blueprint_type?=false` | Validate one canonical channel tag, exact match-type spelling, registration policy, and optional payload struct. |
| `trace_channel_usage` | `channel_tag?`, `source_root?`, `source_roots?`, `include_monolith_source?=false`, `include_engine_gameplay_message_sources?=false`, `max_files?=2000`, `max_results?=500`, `include_line_text?=false` | Perform bounded lexical source analysis for broadcaster/listener candidates and inferred channel/payload/match relationships. |

## Recommended flow

1. Call `gameplay_message.get_status`.
2. Validate the payload with `gameplay_message.validate_message_struct`.
3. Validate the channel/payload pair with `gameplay_message.validate_channel_contract`.
4. Call `gameplay_message.trace_channel_usage` only when static call-site candidates are needed.

For a channel that is intentionally not registered yet, pass `require_registered_tag=false` during preflight. The action still requires canonical tag syntax and exact `ExactMatch` or `PartialMatch` spelling. A valid single-segment root such as `Message` is allowed; leading, trailing, and internal empty dot-delimited segments are not.

## Exact-input contract

- Scalar params use their declared JSON types. String-encoded booleans and integers are invalid.
- Object paths must be canonical Unreal object paths. Whitespace, backslashes, subobject delimiters, package extensions, redirects, case changes, and resolved-path substitutions are rejected.
- Channel tags must pass Unreal's validator, contain no whitespace, contain no leading/trailing/internal empty dot-delimited segment, and match registered spelling/case when `require_registered_tag=true`.
- `match_type` is case-sensitive: only `ExactMatch` and `PartialMatch` are accepted.
- `max_files` is an integer in `1..5000`; `max_results` is an integer in `1..1000`.

## Source-trace boundaries

`trace_channel_usage` is candidate analysis, not runtime tracing:

- Default roots are the current project's `Source` and every discovered non-Monolith project-plugin `Source`, including plugins nested under directories such as `Plugins/GameFeatures`.
- Explicit roots must exist both lexically and physically under the current project's `Source` or `Plugins` directory. Junction/symlink escapes are rejected, and recursive traversal rechecks the physical boundary.
- `Plugins/Monolith/Source` is excluded unless `include_monolith_source=true`.
- Engine scanning is limited to the installed `GameplayMessageRouter/Source` directory and requires `include_engine_gameplay_message_sources=true`.
- Each supported call on a source line is parsed separately. Its pattern declares the channel and match-type argument positions, so a later call cannot inherit an earlier call's payload or match type and callback names cannot masquerade as `PartialMatch`.
- Eligible files are deduplicated and sorted before `max_files` selects the prefix. At most 100,000 eligible files may be enumerated; at most 5,000 are selected, with 256 roots, 1,000 matches, 2 MiB per file, 32 candidates per call, and 1,000 issues. The response separates `files_selected` from files actually loaded in `files_scanned` and reports `limits.eligible_files_enumerated`.
- Inline and multi-line comments are excluded, supported call tokens require an identifier boundary, and scoped constants such as `Combat::TAG_Event` retain their qualifier.
- `include_line_text=false` omits both `line_text` and `function_context`; opt in only when source excerpts are appropriate.
- Orphan broadcaster/listener findings require a complete scan. When any file/result/candidate bound or skipped-file condition prevents absence proof, require `summary.orphan_analysis_complete=false`, inspect `absence_analysis_indeterminate`, and do not interpret zero counterpart rows as an orphan.
- A filtered child-channel trace retains any ancestor `PartialMatch` listener that can receive the child broadcast; inspect that ancestor row as part of the counterpart evidence.
- Matches are lexical single-line candidates. They do not prove branch reachability, listener lifetime, runtime registration, broadcast execution, or payload compatibility at runtime.

Use `channel_graph`, `broadcasters`, `listeners`, `issues`, and `limits` as bounded review evidence. Do not treat them as PIE or live-subsystem proof.
