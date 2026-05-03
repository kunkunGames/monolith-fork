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
