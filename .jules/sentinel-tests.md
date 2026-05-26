## 2024-05-01 - Monolith UISecurity Path Validation Tests
**Target:** ui / create_widget_blueprint
**Learning:** Adding comprehensive malformed path test cases (double slash, empty, missing leading slash, invalid characters, trailing slash) ensures complete validation coverage without requiring a live editor.
**Prevention:** Future Sentinel tasks checking for package path validation should implement these full bounds checks.

## 2024-05-24 - Test MonolithCore Update Version Helpers
**Target:** MonolithCore / UMonolithUpdateSubsystem / ParseVersionFromTag & CompareVersions
**Learning:** Pure C++ static helpers (like semver parsing/comparison) are ideal candidates for Sentinel Test tasks because they can be easily isolated into simple unit tests without requiring complex mock infrastructure or live UE5 editor state.
**Prevention:** Future tests targeting static helper logic should follow this pattern: group related static functions into a single new Automation Test file, cover edge cases (empty strings, whitespace, capitalization), and use straightforward `TestEqual` / `TestTrue` assertions.

## 2026-05-05 - Enforce Comprehensive Path Tests
**Target:** Path validation testing (`ValidatePackagePath`)
**Learning:** When adding validation tests for malformed inputs (like package paths), checking a single malformed state (like double-slash) is insufficient for a complete security regression test.
**Prevention:** Future path/package tests must explicitly iterate multiple malicious/boundary states: empty path, double-slash path (`//Game/...`), missing leading slash (`Game/...`), trailing slashes, and invalid characters.

## 2024-05-28 - Test Param Parsing Rejections
**Target:** MonolithMesh / place_storytelling_scene / ParseVector
**Learning:** Adding test cases for actions that accept arrays or vectors ensures that malformed inputs (like providing a string where an array of numbers is expected) are correctly rejected without crashing the editor.
**Prevention:** Future Sentinel Test tasks targeting parameter schemas should test that helper parsers gracefully reject badly-typed fields (like strings passed to array params).

## 2024-05-18 - Test Core JSON Utils
**Target:** MonolithCore / FMonolithJsonUtils
**Learning:** Monolith core JSON infrastructure functions like FMonolithJsonUtils `Parse`, `Serialize`, `SuccessResponse`, and `ErrorResponse` form the foundation of Monolith's safety boundaries. Validating them with comprehensive checks (e.g. malformed JSON strings properly returning `nullptr` without crashing) ensures safe MCP communication layers.
**Prevention:** Future tests targeting static protocol helpers should verify edge cases (null results, malformed strings, exact response schema compliance) instead of just testing valid structures.

## 2024-05-19 - Test CORS Origin Allowlist Helpers
**Target:** MonolithCore / FMonolithHttpServer / IsAllowedOrigin
**Learning:** Pure security guardrail helpers (like CORS origin validation) are often locked inside anonymous namespaces or private blocks. Exposing them via a minimal public static test seam (`FMonolithHttpServer::IsAllowedOrigin`) allows for comprehensive security regression testing (subdomain spoofing, `null` origin, invalid protocols) without needing to spin up the entire HTTP server stack.
**Prevention:** Future Sentinel tasks targeting network or security guardrails should isolate the validation logic into static helpers and add comprehensive edge-case tests instead of relying on integration-level HTTP requests.

2026-05-20 - Expose pure helpers for malformed validation
Target: MonolithCore/MonolithUpdateSubsystem/ParseSha256FromReleaseNotes
Learning: Regex boundary tests `(?![0-9a-fA-F])` for 64-hex string hashes are difficult to test while deeply embedded in an HTTP response handler.
Prevention: Extract targeted logic into public static helper functions so they can be unit-tested without needing mock HTTP requests.

## 2026-05-22 - MRQ Package Path Validation Testing
**Target:** MonolithLevelSequence / MonolithMovieRenderQueueActions / SaveQueue
**Learning:** Security bounds testing for package paths (`ValidatePackagePath`) must be verified on the real action namespace and after earlier action preconditions have been satisfied. MRQ save_queue path tests must run only when `WITH_MONOLITH_MRQ` is enabled and must seed the current queue with a job before asserting malformed-path errors.
**Prevention:** Future Sentinel tasks checking optional/domain actions with package writes must confirm the action namespace, optional-dependency gate, and earlier resource preconditions before treating a failing action result as path-validation coverage.

## 2026-05-24 - Comprehensive Path Testing for Material Actions
**Target:** MonolithMaterial / create / ValidatePackagePath
**Learning:** Checking a single malformed state (`//Game/...`) in Material actions is insufficient. Path validation testing must explicitly iterate multiple malicious/boundary states (empty, missing slash, trailing slash, invalid characters) using a parameter array for comprehensive security regression.
**Prevention:** Future tests targeting path validation must employ `TArray<FString> MalformedPaths` containing empty path, double slash, missing slash, trailing slash, and illegal character test cases, and iterate over them ensuring graceful failure.
