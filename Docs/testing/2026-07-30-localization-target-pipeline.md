# Monolith Localization Target Pipeline Verification

**Date:** 2026-07-30
**Scope:** Canonical project-target source configuration plus guarded asynchronous gather/compile execution
**Result:** Review found a culture-specific resource collision that the earlier English-only startup did not exercise; target configuration, regeneration, Korean runtime proof, protected build, API/catalog sync, and visual proof require refresh

---

## 1. Contract

| Surface | Required behavior |
|---|---|
| Target source configuration | Resolve a persisted project `ULocalizationTarget` through runtime-loaded `ILocalizationModule`, reject a stale live model, change only its Gather Text From Source directories, persist `DefaultEditor.ini`, and apply the same directory list to the existing gather config. |
| Source path validation | Require explicit `%LOCENGINEROOT%` or `%LOCPROJECTROOT%` paths that exist and contain no wildcard, duplicate, absolute, root-only, or parent-traversal input. |
| Gather-config integrity | Require unique case-insensitive `GatherTextStepN` names and one canonical `GatherTextFromSource` section with target-scoped source/destination. Replace only its contiguous `SearchDirectoryPaths` rows, preserve all other text/line terminators character-for-character, and reject missing, malformed, duplicate-section, ambiguous, stale, or mixed-line-ending input. |
| Configuration atomicity | Dry-run reports the settings/gather-config delta plus exact structured source-control blockers without mutation. Confirm reacquires source-control state after taking the single-mutation flag and returns before snapshots or writes if it changed; after that gate it snapshots affected bytes and rolls the model/files back if persistence, targeted patching, exact readback, or post-write source-control ownership revalidation fails. |
| Installed-engine link boundary | Consume Localization through public headers plus the runtime-loaded module interface. Persisted target text is imported into reflection-owned `FStructOnScope` memory after the module loads, and only the target name plus Gather Text From Source directories leave that scope; `MonolithConfig` must not directly default-construct `FLocalizationTargetSettings`. Do not statically require `UnrealEditor-Localization.lib`; a foreign process generated that `.lib/.exp` under the installed engine at `2026-07-30 21:13:51`, so their presence is environmental contamination and never counts as acceptance evidence. |
| Target resolution | Accept a strict target identifier and resolve only `Config\Localization\<Target>_Gather.ini` and `Config\Localization\<Target>_Compile.ini` in the current project. |
| Config validation | Reject configs whose source/destination leaves `Content\Localization\<Target>`; compile must use `GenerateTextLocalizationResource`. |
| Write gate | `dry_run=true` reports the exact plan and checkout blockers without launching a process. A real run requires `confirm=true`. |
| Source-control boundary | Target-directory configuration requires `source_control_policy=require_checked_out` and a positive exact `target_changelist`. Both dry-run and confirm ForceUpdate provider state and report per-file `actual_changelist`; confirm repeats the audit after acquiring its mutation flag but before snapshots/`PostEditChange`/file writes, then audits again after write. It permits no missing/unknown/untracked/stale/added/deleted/ignored/conflicted/other-user/default-CL/mismatched-CL file and never auto-checks out. The child GatherText commandlet runs with source control disabled and confirm fails before launch if an existing selected generated output is read-only. |
| Process ownership | Jobs are serialized. Cancellation or timeout terminates only the child process handle created by the active job. |
| Module lifetime | Module shutdown closes the launch gate, cancels the active owned job, and joins its worker before code unload. |
| Async observability | A started run returns `job_id`, `monolith.get_job`, `monolith.cancel_job`, per-operation UTF-8 logs, and a generated-artifact hash audit. |

---

## 2. Verification Gates

| Gate | Required result | Current result |
|---|---|---|
| Protected editor build | `P4_BUILD_CHANGELIST=1357`, `SKIP_EDITOR_LAUNCH=1`, `Build\BatchFiles\BuildGameEditorAndRun.bat` | FAIL WITH ROOT CAUSE IDENTIFIED - the fresh protected run in `Saved\Logs\Codex\20260730_ProtectedBuildFix\ProtectedBuild_CL1357_20260730_221925.log` exited `6` because direct `FLocalizationTargetSettings` construction imported `FGatherTextFromTextFilesConfiguration::GetDefaultTextFileExtensions` and `FGatherTextFromPackagesConfiguration::GetDefaultPackageFileExtensions`. The source boundary is patched; a new protected build awaits coordinator authorization. |
| Static Localization import audit | Inspect the final MonolithConfig link response/dependency inputs and audit direct target-settings construction | SOURCE PASS / LINK PENDING - persisted target parsing now uses `FStructOnScope` and a Monolith-owned name/directory snapshot, with no direct `FLocalizationTargetSettings` object construction in `MonolithConfig`. A newly authorized link must still prove `UnrealEditor-Localization.lib` is absent even though an external raw UBT created a copy in the installed engine intermediate directory. |
| Parameter-guard automation | `Monolith.ParamGuard.MonolithConfig.Localization` | PENDING REFRESH - the earlier `7/7` run predates the lifecycle and target-configuration cases; rerun the expanded suite after the fresh linked build. |
| Live target-config dry-run | `localization.set_target_text_search_directories`, `target=EngineOverrides`, one BuildPatchServices engine path, `source_control_policy=require_checked_out`, `target_changelist=1341`, `dry_run=true` | PENDING - must ForceUpdate and report the exact `DefaultEditor.ini`/generated-config delta plus provider/opened/default-CL/actual-CL blockers without changing either surface. |
| Live target-config confirm/readback | Check out the dry-run files to CL `1341`, invoke with `confirm=true`, then rediscover/read back | PENDING - must persist exactly `%LOCENGINEROOT%Source/Runtime/Online/BuildPatchServices/Private`, change only `DefaultEditor.ini` and `EngineOverrides_Gather.ini`, and prove all non-directory gather-config text plus line terminators are unchanged character-for-character. |
| Live dry-run | `localization.run_target_pipeline` with `target=EngineOverrides`, `dry_run=true` | PASS - the first read-only plan reported all `32` existing outputs as checkout blockers; after exact checkout to CL `1341`, the second plan reported `32` writable outputs and `ready=true`. |
| Live confirmed job | Check out the reported generated files in CL `1341`, invoke with `confirm=true`, and poll `monolith.get_job` | PASS - job `8c156fb5-48a5-ee9a-04b7-54b266ae7f55`; gather and compile both exited `0`, source control was disabled, and the artifact audit reported `59` files with `31` updated, `0` created, and `0` deleted. Logs: `Saved\Monolith\LocalizationJobs\8c156fb5-48a5-ee9a-04b7-54b266ae7f55`. |
| Cross-culture collision audit | Compare project `EngineOverrides` locres entries with the installed UE 5.8 Engine locres before changing gather scope | FAIL AS REVIEW INPUT - Korean has `443` project entries; `404` overlap the installed Engine resource (`295` conflicting translations, `109` identical), while `39` BuildPatchServices entries are project-only. Source hashes match for all overlaps, proving this is duplicate resource ownership rather than stale source text. |
| EngineOverrides runtime readback | Start the current standalone build with `-LANGUAGE=ko -LOCALE=ko`, then inspect startup logs and PC 1920x1080 captures | FAIL AS REVIEW INPUT - `Saved\Logs\Speed-Frontend-PrimaryLayout-Preflight-20260730.log` reports `295` translation conflicts, including `InputKeys`. The prior English startup's zero-warning result was not sufficient because it did not load Korean target translations. After the source-scope fix and regeneration, Korean must report zero translation conflicts. |
| Perforce | Implementation, tests, module spec, and this record belong to CL `1357`; localization outputs belong to CL `1341`. The already-open API reference and generated catalog surfaces remain owned by pre-existing CL `1348` and must be refreshed from the final catalog before either dependent CL is submitted. | PENDING - final ownership and dependency order must be resolved without moving unrelated CL `1348` files into CL `1357`. |
| Screenshot verification | Fresh PC `1920x1080` readiness/loading sequence for CL `1341` | PENDING - required because the regenerated resources repair a user-visible startup flow. |
| Discord screenshot upload | Exact CL `1341` evidence set via `UploadScreenshotTestsToDiscord.bat --files` | PENDING - upload only the inspected fresh baseline captures. |

---

## 3. Acceptance

CL `1357` is submit-ready only when its final build, expanded automation, API/catalog sync, live canonical configuration, and live pipeline gates pass from the same source bytes. CL `1341` is submit-ready only after the target gathers only the `39` project-owned BuildPatchServices entries, all cultures are regenerated, Korean startup has zero translation conflicts, fresh warning-free PC 1920x1080 visual proof is inspected and explicitly uploaded to Discord, and the final Perforce ownership audit passes.
