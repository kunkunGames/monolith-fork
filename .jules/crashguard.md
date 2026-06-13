## 2026-05-04 - Package Path Validation in MonolithAI Actions
**Failure mode:** Malformed JSON payload paths reaching CreatePackage and causing fatal ensures in UObjectGlobals.
**Learning:** Monolith tools receive paths from untrusted HTTP payloads; CreatePackage inherently assumes validated paths, so validation must happen at the boundary.
**Prevention:** Always use MonolithCore::ValidatePackagePath(Path) before calling CreatePackage, returning FMonolithActionResult::Error on failure.
**Avoid:** Calling CreatePackage directly with payload-derived paths.

## 2025-05-03 - [Package Path Validation in MonolithLogicDriver]
**Failure mode:** Malformed package paths (e.g., //Game/...) crashing the editor via fatal ensures during CreatePackage.
**Learning:** MonolithLogicDriver actions constructing SM Blueprints via factories were missing the standard MonolithCore::ValidatePackagePath check before CreatePackage.
**Prevention:** Added ValidatePackagePath checks immediately before CreatePackage in MonolithLogicDriverAssetActions, ScaffoldActions, and SpecActions.
**Avoid:** Avoid calling CreatePackage with unvalidated paths originating from external JSON inputs.


## 2024-10-27 - 🧯 Crashguard: Niagara create actions malformed path safety
**Failure mode:** Malformed paths (e.g. `//Game/...`) passed directly to `CreatePackage` cause an editor crash.
**Learning:** Actions that take a user-supplied save path and generate a package must validate the path first.
**Prevention:** Always check `MonolithCore::ValidatePackagePath(SavePath)` before constructing new packages.
**Avoid:** Calling `CreatePackage` with an unchecked user string.

## 2025-05-04 - Package Path Validation in MonolithEditor HandleStitchFlipbook
**Failure mode:** Malformed package paths (e.g., //Game/...) crashing the editor via fatal ensures during CreatePackage.
**Learning:** MonolithEditorActions constructing Texture2D from StitchFlipbook were missing the standard MonolithCore::ValidatePackagePath check before CreatePackage.
**Prevention:** Added ValidatePackagePath checks immediately before CreatePackage in MonolithEditorActions.
**Avoid:** Avoid calling CreatePackage with unvalidated paths originating from external JSON inputs.

## 2026-06-12 - Forbid numeric branch evasion
**Coordination issue:** Crashguard generated multiple branches with large numeric suffixes (e.g., `-10520128106955593194`, `-6763324284465746968`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
