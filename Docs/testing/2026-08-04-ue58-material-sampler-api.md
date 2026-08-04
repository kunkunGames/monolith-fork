# UE 5.8 Material Sampler API Verification

**Date:** 2026-08-04
**Scope:** `MonolithCore`, `MonolithIndex`, and `MonolithMaterial`
**Related:** issue #121

---

## 1. Regression

UE 5.8 deprecates `UMaterialExpressionTextureBase::GetSamplerTypeForTexture` in favor of `MaterialExpressionUtils::GetSamplerTypeForTexture`. UE 5.7 does not expose the replacement header, so an unconditional migration would break the supported compile floor. The deprecated call existed in both texture metadata consumers.

## 2. Compatibility contract

| Engine | Routed API |
|---|---|
| UE 5.7 | `UMaterialExpressionTextureBase::GetSamplerTypeForTexture` |
| UE 5.8+ | `MaterialExpressionUtils::GetSamplerTypeForTexture` |

Both consumers call `MonolithMaterialSamplerCompat::GetSamplerTypeForTexture`; the engine-version boundary is defined once in `Source/MonolithCore/Public/MonolithMaterialSamplerCompat.h`.

## 3. Verification

| Gate | Expected result | Result |
|---|---|---|
| Repository diff audit | One compatibility boundary and two migrated consumers | Pass — `git diff --check` returned no errors |
| UE 5.7 plugin package build | Editor plugin compiles with the legacy API path | Pass — 434/434 actions, UAT exit 0 |
| UE 5.8 plugin package build | Editor plugin compiles without either sampler deprecation warning | Pass — 434/434 actions, UAT exit 0, zero matching sampler C4996 warnings |

Both builds compiled `GenericAssetIndexer.cpp` and `MonolithMaterialActions.cpp` as individual actions. Engine roots were resolved from validation-host `.uproject` `EngineAssociation` values through `Build/BatchFiles/Script/ResolveUnrealEngine.ps1`. UAT logs were redirected to external evidence paths:

- `D:\P4\MonolithValidation20260804\02-ue58-material-sampler\Logs\UE57`
- `D:\P4\MonolithValidation20260804\02-ue58-material-sampler\Logs\UE58`

The authoritative command shape was `RunUAT BuildPlugin -NoTargetPlatforms -Rocket`. An earlier UE 5.7 run also completed all 434 compile actions, but its final UAT log flush failed because the system drive was full; the redirected-log exit-0 run above supersedes it.

Issue #121 contains other engine deprecations that are intentionally outside this single-concern change.
