## 2024-05-01 - Monolith UISecurity Path Validation Tests
**Target:** ui / create_widget_blueprint
**Learning:** Adding comprehensive malformed path test cases (double slash, empty, missing leading slash, invalid characters, trailing slash) ensures complete validation coverage without requiring a live editor.
**Prevention:** Future Sentinel tasks checking for package path validation should implement these full bounds checks.

## 2024-05-24 - Test MonolithCore Update Version Helpers
**Target:** MonolithCore / UMonolithUpdateSubsystem / ParseVersionFromTag & CompareVersions
**Learning:** Pure C++ static helpers (like semver parsing/comparison) are ideal candidates for Sentinel Test tasks because they can be easily isolated into simple unit tests without requiring complex mock infrastructure or live UE5 editor state.
**Prevention:** Future tests targeting static helper logic should follow this pattern: group related static functions into a single new Automation Test file, cover edge cases (empty strings, whitespace, capitalization), and use straightforward `TestEqual` / `TestTrue` assertions.
