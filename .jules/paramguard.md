## 2025-02-06 - Harden audio/modify_sound_submix param parsing
**Malformed input pattern:** String passed to `output_volume_db` or `output_volume`
**Learning:** Checking `HasField` and directly calling `GetNumberField` causes a fatal engine crash if the client passes an unexpected type like a string.
**Prevention:** Always use `TryGetNumberField` for optional JSON number values to safely handle missing or invalid types.

## 2024-05-10 - Harden `HandleBuildSMFromSpec` array and bool JSON parsing
**Malformed input pattern:** Untrusted spec arrays (`states`, `conduits`, `nested_sms`, `transitions`) and booleans (`is_initial`, `is_end`) checked with `HasField` but accessed via `GetArrayField` or `GetBoolField` without type matching.
**Learning:** `HasField` evaluates if a key exists but not if the underlying value matches the strict C++ type expectations (e.g. an array field provided as an integer will pass `HasField` but crash on `GetArrayField`).
**Prevention:** Handlers accessing nested schema values must always use `TryGetArrayField`, `TryGetBoolField`, and other safe accessors, returning a clear `FMonolithActionResult::Error` if type retrieval fails, rather than deferring type checks to UE crash paths or assertions.

## 2026-05-11 - Harden FVector/FRotator JSON parsing in MonolithCore
**Malformed input pattern:** JSON objects passed to ParseVector and ParseRotator missing required coordinate fields, or containing fields of the wrong type (e.g. strings instead of numbers).
**Learning:** `FJsonObject::GetNumberField` asserts or crashes if the field doesn't exist or is not a number. Initial parameter schema validation does not enforce exact structure for all optional nested data.
**Prevention:** Always use `TryGetNumberField` (or other `TryGet*Field` methods) when parsing nested fields from untrusted JSON, and explicitly handle the boolean return value to gracefully reject malformed inputs.
