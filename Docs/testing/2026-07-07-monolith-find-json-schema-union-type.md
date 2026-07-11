# Monolith Find JSON Schema Union Type Verification

**Date:** 2026-07-07
**Project:** Speed
**Area:** MonolithCore / MCP and discovery input schema exposure
**Change:** Convert internal pipe-delimited parameter type specs such as `array|string` and `array|string|object|number` to standard JSON Schema `type` arrays whenever Monolith exposes an MCP-facing `inputSchema`; normalize internal `bool` to JSON Schema `boolean`; expand internal `any` to the full standard JSON Schema type set on the same path.

---

## 1. Scope

Verified that `monolith_find.fields` no longer exposes the internal union string `array|string` through MCP `tools/list`. The fix now applies through the shared private `MonolithMcpSchemaUtils::BuildInputSchema` path, so detailed discovery rows, `monolith_find(include_schema=true)` rows, and deferred-domain `inputSchema` rows also emit standard JSON Schema while preserving the internal Monolith `params` schema for dispatch-time type validation and compatibility.

---

## 2. Commands And Results

| Check | Command / Action | Result |
|-------|------------------|--------|
| C++ compile smoke | `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=D:\P4\speed\Speed.uproject" -WaitMutex -NoHotReloadFromIDE -NoLink` | Passed. |
| Full editor build | `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=D:\P4\speed\Speed.uproject" -WaitMutex -NoHotReloadFromIDE` | Partially blocked by unrelated running UnrealEditor game processes holding `CommonUser`, `SpeedCoreRuntime`, and `LyraGame` DLLs. The required `UnrealEditor-MonolithCore.dll` link completed before the unrelated DLL locks stopped the overall target. |
| MCP recovery | `Plugins\Monolith\Scripts\recover_mcp.ps1 -TimeoutSec 600` | Passed with `RESULT=MCP_UP`, PID 57020. |
| MCP schema wire check | JSON-RPC `tools/list` POST to `http://localhost:9316/mcp`, inspecting `monolith_find.inputSchema.properties.fields.type` | Passed: JSON `["array","string"]`. |
| Discovery schema wire check | JSON-RPC `monolith_discover(namespace="imagegen", action="generate_image", mode="schema")` | Passed: `inputSchema.properties.resolution.type` was `["array","string","object","number"]`, `inputSchema.properties.save.type` was `boolean`, while internal `params` preserved `array|string|object|number` and `bool`. |
| Registry-wide inputSchema scan | Paged `monolith_discover(namespace, detail=true, schema_detail="full")` over all namespaces and checked every `inputSchema.properties.*.type` token against standard JSON Schema types | Passed: 61 namespaces, 1829 action rows, invalid `inputSchema` type count 0. |
| Runtime find smoke | `monolith_find(query="find caller graph action", limit=2, fields=["action_id","description","score"])` | Passed; returned projected matches including `source.find_callers`. |
| New automation test | `editor.run_automation_tests(prefix="Monolith.ParamSchema.McpJsonSchemaProperty", max_tests=1)` | Passed: 1/1, run `automation-20260707T130303Z-79BECAD1`. Covers JSON Schema property conversion, full `inputSchema` conversion, required-array promotion, internal marker filtering, two-way/four-way union types, `bool` to `boolean` normalization, and `any` expansion to standard JSON Schema types. |
| Existing validator test | `editor.run_automation_tests(prefix="Monolith.ParamSchema.TypedValidation", max_tests=1)` | Passed: 1/1, run `automation-20260707T130305Z-8587EBDC`. |
| Static checks | `python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` from `Plugins\Monolith` | Passed with blocking findings: 0. Existing advisory findings remained, including CRLF hygiene warnings and a skill-drift timeout advisory. |

---

## 3. Notes

The standardization is intentionally limited to MCP-facing `inputSchema` fields. Internal `params` schemas still report Monolith param type specs such as `fields.type = "array|string"` so existing dispatch validation, fuzzy schema search, and compatibility consumers remain unchanged.

---

## 4. Follow-Up Binary Verification

2026-07-08 follow-up: the source files from CL 1071 were correct, but the submitted `UnrealEditor-MonolithCore.dll` still served stale MCP schema output in a live `tools/list` check: `monolith_find.inputSchema.properties.fields.type` returned the raw string `"array|string"`. Rebuilding `MonolithCore` from the same source updated the tracked binary and restored the expected MCP-facing JSON Schema array.

| Check | Command / Action | Result |
|-------|------------------|--------|
| Pre-rebuild live MCP check | JSON-RPC `tools/list` POST to `http://127.0.0.1:9316/mcp`, inspecting `monolith_find.inputSchema.properties.fields.type` | Failed against the submitted binary: returned `"array|string"`. |
| MonolithCore binary rebuild | Touched `MonolithHttpServer.cpp` timestamp, opened `UnrealEditor-MonolithCore.dll` / `.pdb` in CL 1076, then ran `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=D:\P4\speed\Speed.uproject" -WaitMutex -NoHotReloadFromIDE` | `MonolithCore` compile/link completed and updated `UnrealEditor-MonolithCore.dll`; overall target then failed on unrelated read-only `SpeedCoreRuntime` and `MonolithLevelDesign` DLL links. |
| Post-rebuild live tools/list check | Start `Build\BatchFiles\RunHeadlessEditor.bat`, poll JSON-RPC `tools/list`, inspect `monolith_find.inputSchema.properties.fields.type` | Passed: returned `["array","string"]`. |
| Post-rebuild live `monolith_find(include_schema=true)` check | JSON-RPC `tools/call` `monolith_find(query="monolith find fields projection", namespace="monolith", limit=5, include_schema=true)` | Passed: `matches[0].params.fields.type` stayed `"array|string"` for internal compatibility, while `matches[0].inputSchema.properties.fields.type` returned `["array","string"]`. |
