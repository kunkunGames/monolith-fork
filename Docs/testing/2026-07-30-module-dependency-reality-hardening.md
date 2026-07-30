# Monolith Module Dependency Reality Hardening

**Date:** 2026-07-30  
**Scope:** `source.audit_module_dep_reality` and `source.suggest_build_cs_deps` module/type ownership resolution  
**Changelist:** 1360  
**Result:** Submit-ready: exact current bytes compile/link, focused automation and both live dependency surfaces pass, and final ownership/line-ending gates are clean

---

## 1. Regression

`EngineSource.db` may retain an aggregate index module such as `Runtime` for a
type whose indexed file actually belongs to
`Engine/Source/Runtime/<OwningModule>/...`. Treating the aggregate label as the
UBT owner produces false missing dependencies and can also misclassify a
project module that is literally named `Runtime`, `Editor`, `Developer`,
`Programs`, or `ThirdParty`.

The resolver also accepted non-type symbol rows. An initial filter then treated
`symbols.is_ue_macro` as meaning “this symbol is a macro,” but the index uses
that flag for reflected UCLASS/USTRUCT declarations as well. That removed the
canonical CoreUObject `FAssetData` row, left a same-name Dataflow struct as an
incorrect unique match, and allowed an enum value named `UPROPERTY` to be
mistaken for a dependency-bearing type.

---

## 2. Fix Contract

| Surface | Required behavior |
|---|---|
| Shared module resolver | Both dependency actions use `ModuleDepRealityUtils.h`; tests call the production helper instead of duplicating its parser. |
| Project/plugin paths | The last `Source/<Module>/...` segment is authoritative, including project modules named like engine grouping categories. |
| Engine paths | `Runtime`, `Editor`, `Developer`, `Programs`, and `ThirdParty` are skipped only below the resolved current engine source root or when a relative indexed path is corroborated by the indexed module name. |
| Symbol-kind filter | Only `class`, `struct`, `enum`, `union`, `typedef`, and `type_alias` rows can resolve a dependency; non-type symbols are rejected. |
| Reflected types | UCLASS/USTRUCT declarations remain valid even when `symbols.is_ue_macro=1`; the flag describes declaration provenance, not the symbol's identifier kind. |
| Macro identifiers | `UPROPERTY`, `UCLASS`, and the other reflection/declaration macro names are rejected lexically before database lookup. |
| Ambiguous names | All valid reflected/non-reflected type rows participate in ownership resolution, so same-name declarations such as CoreUObject/Dataflow `FAssetData` remain ambiguous and fail closed instead of choosing one module. |
| Fallback | The indexed module label is used only when no valid `Source/<Module>` ownership can be derived. |

This keeps the correction in one reusable private helper and improves both the
whole-project audit and the forward dependency suggestion action.

---

## 3. Verification Gates

| Gate | Required result | Current result |
|---|---|---|
| Resolver automation | Project/category edge cases, relative/absolute engine paths, every accepted type kind, case-insensitivity, non-type rejection, macro-identifier rejection, and reflected-type retention pass | **PASS:** current-source run `automation-20260730T151100Z-785784CE`, `1/1`, zero errors and warnings in 0.000322 seconds |
| Protected editor build | Coordinator-owned protected `Build\BatchFiles\BuildGameEditorAndRun.bat` run; this review must not start another build | **PASS:** the exact current `ModuleDepRealityTests.cpp` and `FModuleDepRealityAdapter.cpp` bytes compiled at lines 149-150, `UnrealEditor-MonolithReflectionIntel.dll` linked at line 162, and the 24-action build ended `Result: Succeeded` in 37.56 seconds at lines 230-231 of `C:\Users\12336\AppData\Local\UnrealBuildTool\Log.txt`. This review did not run another build. |
| Monolith automation | `Monolith.ReflectionIntel.SourceAudit.SuggestBuildCsDepsForward` passes in the freshly linked editor | **PASS:** `automation-20260730T151100Z-785784CE`, `1/1`, zero errors and warnings |
| Whole-module live read-back | `audit_module_dep_reality` on `MonolithReflectionIntel` reports neither the `UPROPERTY -> MonolithLogicDriver` nor `FAssetData -> Dataflow` false positive | **PASS:** current live `source.audit_module_dep_reality` with `scan_root=D:/P4/speed/Plugins/Monolith/Source/MonolithReflectionIntel` and `limit=200` returned `violations=[]`, `total_estimate=0` |
| Forward live read-back | The shared resolver retains genuine dependency signal and reports no false missing module for the adapter | **PASS:** current live `source.suggest_build_cs_deps` on `FModuleDepRealityAdapter.cpp` returned `declaring_module=MonolithReflectionIntel`, required modules `Json`, `MonolithCore`, and `MonolithSource`, with `missing=[]`; no `MonolithLogicDriver` or `Dataflow` false dependency was emitted |
| Line endings | Every CL1360 text file uses CRLF | **PASS:** `TestSourceLineEndings.ps1 -ProjectRoot D:\P4\speed -Changelist 1360` verified `5/5` files after normalization; no bare LF remains |
| Source-control audit | Current files, no unresolved integrations, no foreign opens/locks, and no unchanged checkout | **PASS:** exactly five cohesive files are open in CL 1360; `p4 fstat -Ol` reports no `otherOpen`, `otherLock`, or `unresolved`; `p4 resolve -n -c 1360` reports nothing to resolve; `p4 revert -n -a -c 1360` reports no unchanged file |
| Screenshot verification | N/A | Read-only source analysis has no runtime/editor visual presentation change. |
| Discord screenshot upload | N/A | `UploadScreenshotTestsToDiscord.bat` is not run because screenshot verification is not relevant. |

---

## 4. Acceptance

Every non-N/A gate above records exact passing evidence from the current source
bytes and freshly linked editor binary. CL `1360` is submit-ready.
