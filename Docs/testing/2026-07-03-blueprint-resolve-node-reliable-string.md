# Blueprint resolve_node Reliable String Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-03 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | `blueprint.resolve_node` CustomEvent `reliable` dry-run input compatibility |

---

## 1. Results

| Check | Result |
| --- | --- |
| `editor.get_live_coding_diagnostics` | Passed. Live Coding was available for the running headless editor. |
| `editor.trigger_build(wait=true)` | Passed after each source edit. Final compile finished at `2026-07-03T13:51:22.164Z` with `last_result=success`, `patch_applied=true`, `error_count=0`, and `warning_count=0`. |
| `editor.run_automation_tests(prefix="Monolith.Blueprint.ResolveNode.AcceptsReliableStringLiteral")` | Passed. `requested_tests=1`, `passed=1`, `failed=0`. |
| `editor.run_automation_tests(prefix="Monolith.ParamGuard.Blueprint.NodeActionsRejectMalformedTopLevelParams")` | Passed. `requested_tests=1`, `passed=1`, `failed=0`. |
| Live MCP `blueprint.resolve_node(node_type="CustomEvent", replication="server", reliable="true")` | Passed. Returned `replication="server"` and `reliable=true`. |
| Live MCP `blueprint.resolve_node(node_type="CustomEvent", reliable="maybe")` | Passed as negative case. Returned `isError=true` with `-32602` and message `resolve_node reliable must be a boolean or one of the string literals: true, false, 1, 0, yes, no`. |

## 2. Notes

The ROI trigger was `Saved\Monolith\LogAnalysis\roi-20260703`, specifically `schema_fix:action:blueprint.resolve_node:7fc7ff6f02294432`, where agent calls sent `reliable:"true"` for CustomEvent dry-runs. The fix is intentionally scoped to the read-only `resolve_node` preview action: the schema accepts `bool|string`, the handler normalizes known boolean string literals, and unrelated mutating Blueprint bool params remain strict.
