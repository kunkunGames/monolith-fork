## 2024-05-24 - [Harden MonolithLogicDriver Component Params]
**Malformed input pattern:** `GetStringField` calls crashing the editor when optional parameters (like `component_name` or `network_config`) were absent or provided as numbers instead of strings.
**Learning:** `Params->HasField` only checks if the key exists, not its type. So `HasField(Key) && GetStringField(Key)` still asserts if the client sends `{"component_name": 123}` instead of a string.
**Prevention:** Prefer `TryGetStringField`, which safely fails and leaves the output parameter unchanged if the key is missing or not a string. For optional params, check the return value of `TryGetStringField` rather than using `HasField`.

## 2025-05-04 - Harden LogicDriver Scaffold Actions Array Parsing
**Malformed input pattern:** JSON array parameters (`dialogue_nodes`, `choices`, `objectives`, `states`) supplied as strings or objects, or array elements supplied as the wrong type.
**Learning:** `Params->HasField()` followed by `Params->GetArrayField()` triggers a fatal assert if the field is present but not an array type. Additionally, retrieving array items without validating their type triggers asserts when casting (e.g., `->AsObject()` or `->AsString()`).
**Prevention:** Use `TryGetArrayField()` which safely checks type. Always verify array element types using `Item->Type == EJson::X` prior to casting, returning a clear `FMonolithActionResult::Error` if invalid.
