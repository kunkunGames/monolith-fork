## YYYY-MM-DD - Harden MonolithSource parameter parsing
**Malformed input pattern:** Using GetStringField/GetBoolField/GetNumberField directly without Try* counterparts or immediately after HasField.
**Learning:** HasField does not guarantee type safety and can cause assertion crashes on malformed inputs.
**Prevention:** Always use the equivalent TryGet*Field.

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

## 2026-05-14 - Harden MonolithGAS Target Actions parameter parsing
**Malformed input pattern:** Using GetStringField/GetBoolField/GetNumberField immediately after HasField.
**Learning:** HasField does not guarantee type safety and can cause assertion crashes on malformed inputs. WaitTargetData actions have multiple optional fields that are susceptible to this.
**Prevention:** Always use the equivalent TryGet*Field.
## 2026-05-14 - ProjectIndexer: harden index action param parsing
**Malformed input pattern:** array parameter fallback behavior
**Learning:** array parameters need to report an error if wrong type was provided, instead of being silently ignored
**Prevention:** FJsonObject::HasField + FJsonObject::TryGetArrayField

## 2026-05-14 - Harden Niagara duplicate_emitter param parsing
**Malformed input pattern:** String parameter `new_name` in `duplicate_emitter` checked with `HasField` but accessed directly with `GetStringField`.
**Learning:** `HasField` only checks if the key exists, not the type. Calling `GetStringField` directly causes assertion crashes if the user provides a different JSON type (like a number or boolean).
**Prevention:** Handlers accessing optional string fields must use `TryGetStringField` after `HasField`, and explicitly handle the boolean return to reject malformed types with a clear `FMonolithActionResult::Error`.
**Avoid:** Assuming `HasField` guarantees type safety for optional parameters.

## 2026-05-14 - Reject malformed string types in optional number params
**Malformed input pattern:** String values reaching optional number params parsed with `HasField` and `GetNumberField`, causing assert/crash.
**Learning:** `GetNumberField` throws or asserts if a present field is the wrong JSON type (like a string where a number is expected).
**Prevention:** Use `TryGetNumberField` inside a `HasField` block to validate present params. If `TryGetNumberField` fails, return an explicit error instead of a default fallback.
**Avoid:** Falling back to default values when a client explicitly sends a malformed (wrong type) value. Defaults are only for absent fields.

## 2026-05-18 - Do not use HasField with GetNumberField or GetBoolField for optional untrusted JSON properties
**Malformed input pattern:** A field is present with the wrong JSON type (e.g., a string instead of a number/boolean) and is accessed with `HasField` followed by an unsafe cast like `GetNumberField` or `GetBoolField`.
**Learning:** This crashes or defaults to incorrect values (like 0.0) without throwing a validation error because `HasField` returns true even for wrong types. This allows malformed client input to mutate assets with unintended defaults.
**Prevention:** Always use `TryGetNumberField`, `TryGetBoolField`, etc. If it fails, explicitly return an invalid-param error instead of silently falling back to defaults.
**Avoid:** Leaving `HasField` plus `GetNumberField` or `GetBoolField` around optional properties.

## 2026-05-18 - Harden mesh furnishing parameter parsing
**Malformed input pattern:** `HasField` checks followed by `GetNumberField` in `ParseFurnitureItemJson` and other furnishing actions in `MonolithMeshFurnishingActions.cpp`.
**Learning:** `HasField` only confirms a field exists, it does not confirm the type. Calling `GetNumberField` directly causes assertions if the JSON type is incorrect (e.g. a string).
**Prevention:** Use `TryGetNumberField` to safely validate existence and extract type in a single call.
**Avoid:** Assuming `HasField` is sufficient for robust optional parameter type safety.
