## 2024-05-24 - [Harden MonolithLogicDriver Component Params]
**Malformed input pattern:** `GetStringField` calls crashing the editor when optional parameters (like `component_name` or `network_config`) were absent or provided as numbers instead of strings.
**Learning:** `Params->HasField` only checks if the key exists, not its type. So `HasField(Key) && GetStringField(Key)` still asserts if the client sends `{"component_name": 123}` instead of a string.
**Prevention:** Prefer `TryGetStringField`, which safely fails and leaves the output parameter unchanged if the key is missing or not a string. For optional params, check the return value of `TryGetStringField` rather than using `HasField`.
