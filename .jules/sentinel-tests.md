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
