## 2026-05-04 - Package Path Validation in MonolithAI Actions
**Failure mode:** Malformed JSON payload paths reaching CreatePackage and causing fatal ensures in UObjectGlobals.
**Learning:** Monolith tools receive paths from untrusted HTTP payloads; CreatePackage inherently assumes validated paths, so validation must happen at the boundary.
**Prevention:** Always use MonolithCore::ValidatePackagePath(Path) before calling CreatePackage, returning FMonolithActionResult::Error on failure.
**Avoid:** Calling CreatePackage directly with payload-derived paths.
