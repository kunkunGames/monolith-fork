# Monolith ImageGen Audit Verification

**Date:** 2026-05-22
**Engine:** Unreal Engine 5.7, resolved from `D:\P4\game\GO.uproject`
**Scope:** `Plugins/Monolith/Source/MonolithImageGen`
**Result:** PASS.

---

## 1. Change Under Test

Full ImageGen review found a contract mismatch: `imagegen.generate_image_via_ima2` exposed provider formats wider than the project wants for generated Texture2D assets, and `texture_role` affected only import settings, not the upstream generation prompt. The fix keeps the bridge/import contract explicit, restricts bridge output to PNG, validates role-sensitive inputs before any external provider call, and adds opt-in role-aware prompt composition.

| Area | Result |
|------|--------|
| Generated payload formats | `generate_image_via_ima2` now accepts only `png` for provider output, and `import_generated_image` accepts only generated PNG bytes/files. JPEG, WebP, and other formats are rejected before the bridge call or generated import. |
| Local placeholder format | `generate_image` now emits a deterministic PNG placeholder (`monolith/local-gradient-png-v1`), with the legacy `monolith/local-gradient-bmp-v1` model name retained as an alias. |
| Prompt composition | `compose_prompt` defaults to `true`, appending Unreal Texture2D and `texture_role` constraints to the provider prompt, including strict evenly spaced grid constraints for multi-frame `sprite` output, and storing only prompt hashes. Set `compose_prompt=false` for verbatim caller prompt passthrough. |
| Texture role strictness | `generate_image_via_ima2` now validates `texture_role` before the bridge call, keeps the provider `background` option at `auto` when omitted or when callers request `transparent` for ima2/gpt-5.5 compatibility, and rejects transparent backgrounds for `world_tile`, `normal`, `orm_mask`, and `height`. |
| Transparent background | Caller `background="transparent"` is validated for role compatibility but normalized to provider `background="auto"` before the ima2/OpenAI request to avoid gpt-5.5 HTTP 400 rejection. |
| Data URL import | Data URL MIME type now overrides stale or wrong `format_hint`, so a PNG data URL is decoded as PNG even if the caller supplied `format_hint="jpeg"`. |
| Raw payload validation | `import_generated_image` now detects the compressed image format from bytes when no hint is supplied and returns a clear validation error when the format cannot be detected or is unsupported. |
| URL hygiene | `server_url` rejects credentials, query strings, and fragments before provenance can store the bridge URL. |

---

## 2. Verification Results

| Gate | Command / Evidence | Result |
|------|--------------------|--------|
| ImageGen module compile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithImageGen -WaitMutex -NoHotReloadFromIDE` | PASS: compiled and linked `MonolithImageGenActions.cpp`, `ImageGenTextureRoleTests.cpp`, and `UnrealEditor-MonolithImageGen.dll`. |
| Full GoGameEditor compile | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | BLOCKED outside ImageGen: `Source\GoGame\Private\Skill\GoSkillActor.cpp(27,63)` fails with C2445 ambiguous conditional between `const TObjectPtr<AActor>` and `AActor*`. |
| ImageGen automation | `UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSplash -Unattended -NoSound -ExecCmds="Automation RunTests MonolithImageGen.TextureRoles; Quit" -TestExit="Automation Test Queue Empty"`; evidence in `Saved\Logs\GO.log` | PASS: 5/5 tests succeeded: `Defaults`, `GenerateLocalForwardsRole`, `ImportGeneratedImageForwardsRole`, `ReferenceInputsArchive`, and `Validation`. |
| New validation coverage | `MonolithImageGen.TextureRoles.Validation` | PASS: covers WebP/JPEG pre-bridge rejection, invalid `texture_role` rejection, role/background incompatibility rejection, query-string server URL rejection without echoing query content, and data URL MIME override for stale `format_hint`. |
| Existing startup noise | `Saved\Logs\GO.log` | Non-blocking existing PaperZD member-initialization, missing EngineSource.db, and unrelated map Blueprint load/compile log errors still appear during commandlet startup; targeted MonolithImageGen tests completed with exit code 0. |
