# Monolith Niagara `add_module` Parameter-Guard Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-13 |
| Scope | `niagara.add_module` required-string validation, JSON-RPC error classification, focused editor automation |
| Changelist | 1143 |
| Specification | `Plugins\Monolith\Docs\specs\SPEC_MonolithNiagara.md` |

---

## 1. Confirmed Defect

`FMonolithNiagaraActions::HandleAddModule` read `emitter`, `usage`, and `module_script` directly with `TryGetStringField`. That check rejected a missing field or a non-string value, but it accepted `""` and whitespace-only strings and returned the generic action-error code rather than the Monolith invalid-parameter contract.

The action's registry schema already declared all three fields as required strings. The handler and direct-call behavior therefore disagreed with the registered contract.

## 2. Root Fix

| Before | After |
| --- | --- |
| Three independent `TryGetStringField` branches | All three fields use `MonolithParamUtils::GetRequiredStringParam`. |
| Empty and whitespace-only values reached asset/emitter resolution | Missing, wrong-type, empty, and whitespace-only values fail before any asset work. |
| Handler validation returned a generic action error | Handler validation returns `FMonolithJsonUtils::ErrInvalidParams` (`-32602`) with the utility's field-specific message. |
| The test mainly exercised registry pre-validation | The focused test calls `HandleAddModule` directly for every invalid field/value case and also retains a registry-level missing-required assertion. |

No fallback emitter, usage, script path, default asset, or legacy branch was introduced.

## 3. Verification Results

| Gate | Exact evidence | Result |
| --- | --- | --- |
| Focused source coverage | `MonolithNiagaraParamGuardAddModuleTest.cpp` iterates `emitter`, `usage`, and `module_script` across missing, wrong-type, empty, and whitespace-only cases, then checks the registry missing-required path. | PASS, 13 validation assertions/groups cover the handler and registry boundary. |
| Standard EOS DevAuth build | `Build\BatchFiles\BuildClient_EOS_DevAuth.bat` | PASS, Editor and Game targets built; wrapper exit `0`; the configured DevAuth editor launch occurred only after success. |
| Clang editor build | `Build\BatchFiles\BuildGameEditorClang.bat` | PASS, `Result: Succeeded`; official wrapper exit `0`. |
| Strict non-unity editor build | `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat` | PASS, 1,327/1,327 actions; warnings-as-errors and unity disabled; `Result: Succeeded`; wrapper exit `0`. |
| Live Monolith discovery | `editor.list_automation_tests(prefix="Monolith.ParamGuard.Niagara.AddModule")` | PASS, exactly one registered test: `FMonolithNiagaraAddModuleParamGuardTest`. |
| Live Monolith automation | `editor.run_automation_tests(prefix="Monolith.ParamGuard.Niagara.AddModule", max_tests=2)` | PASS, run `automation-20260713T134141Z-46F158DA`; matched 1, completed 1, passed 1, failed 0, skipped 0, warnings 0. |

## 4. Visual And Discord Evidence

This change hardens an editor API input contract and has no runtime gameplay, UMG, VFX presentation, animation, material, level, or asset-presentation output. Screenshot capture and `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` are therefore `N/A`; no Discord screenshot upload was performed.
