## 2024-05-02 - [Sentinel Refactor: Widget Blueprint Loading Cohesion]
**Pattern:** Duplicated ad-hoc path parsing (checking for `/Game/` prefix and using raw `StaticLoadObject`) in `MonolithUIInternal::LoadWidgetBlueprint` and direct action handlers like `MonolithUIAccessibilityActions`.
**Learning:** `FMonolithAssetUtils::LoadAssetByPath<T>(AssetPath)` provides a canonical, robust 4-tier asset lookup system that correctly normalizes paths and catches missing assets securely.
**Reuse rule:** Always prefer `FMonolithAssetUtils::LoadAssetByPath<T>` for asset loading instead of writing manual `StaticLoadObject` fallbacks or path manipulation logic. When loading specific blueprints or assets inside a module, route through the module's `Internal` helper (e.g. `MonolithUIInternal::LoadWidgetBlueprint`) if available, and ensure the internal helper leverages `FMonolithAssetUtils`.
**Avoid:** Avoid using `StaticLoadObject` directly for asset loading in action handlers when `FMonolithAssetUtils` can be used. Avoid duplicating path-normalization logic across multiple files.

## 2024-11-20 - Safe JSON Extraction
**Pattern:** Ad-hoc and unsafe parameter extraction via `GetStringField` and duplicate presence checks like `if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())` appearing frequently across MCP action handlers.
**Learning:** `GetStringField` can trigger Editor-level crashes when a parameter is missing because UE's `FJsonObject::GetStringField` asserts internally on failure.
**Reuse rule:** Always prefer `TryGetStringField` pattern over explicit `HasField`/`GetStringField` pairs to safely extract fields without risking assertions, logging explicit errors manually if necessary.
**Avoid:** Avoid raw `GetStringField` calls for user-supplied JSON input.

## 2026-05-04 - [Replace GetStringField with TryGetStringField]
**Pattern:** Using GetStringField directly for JSON parameter extraction in UI styling actions, leading to potential assertions/crashes when fields are missing.
**Learning:** FParamSchemaBuilder ensures validation if run, but defensive extraction using TryGetStringField prevents crashes from invalid data completely. Required fields must explicitly check the result of TryGetStringField and return FMonolithActionResult::Error.
**Reuse rule:** Always use Params->TryGetStringField instead of Params->GetStringField. For required parameters, wrap in an if statement to return an explicit error string.
**Avoid:** Do not blindly use Params->GetStringField, even if you assume the schema validation guarantees presence.

## 2026-05-04 - [Extract LoadSMBlueprintAndRootGraph helper]
**Pattern:** Repeated loading of SM Blueprint and immediately getting root graph, leading to 6-line boilerplate repeated >15 times across MonolithLogicDriver action handlers.
**Learning:** Monolith tools are generally action-handler heavy, and parameter extraction/asset loading boilerplate can quickly bloat handlers. Extracting paired operations (load asset + extract internal root structure) into a single helper with consolidated error propagation significantly improves readability.
**Reuse rule:** Prefer extracting common multi-step setup logic (like load + get sub-object) into Internal module helpers to keep action handlers focused on logic.
**Avoid:** Duplicating 6-8 lines of asset load / error checking boilerplate across every action handler in a module.

## 2024-05-06 - [Sentinel Refactor: LoadSoundCue cohesion via FMonolithAssetUtils]
**Pattern:** Ad-hoc path normalization and `StaticLoadObject` fallbacks were repeatedly used throughout `MonolithAudioSoundCueActions.cpp` for resolving Sound Cues, Sound Waves, and base assets.
**Learning:** Manual path logic and direct tier-4 lookups (`StaticLoadObject`) bypass Monolith's standard validation and robust 4-tier asset lookup, causing duplication and potentially missing un-saved objects.
**Reuse rule:** Always utilize `FMonolithAssetUtils::LoadAssetByPath<T>` for generic asset loading within Monolith action handlers, dropping duplicated fallback boilerplate in favor of the canonical path.
**Avoid:** Avoid writing manual path normalization (`/Game/Foo.Foo`) combined with `AssetRegistry` and `StaticLoadObject` fallbacks when a Core helper already exists.
## 2024-11-06 - Normalize FJsonObject string extraction
**Pattern:** Repeated missing-param checks like `HasField(...) ? GetStringField(...) : Default` in MonolithAnimation and MonolithMaterial.
**Learning:** TryGetStringField safely extracts string parameters, avoiding ad-hoc bounds and making the error handling clearer.
**Reuse rule:** Use TryGetStringField for extracting optional parameters into an existing variable or default.
**Avoid:** Duplicated `if HasField then Get...` blocks for optional strings.

## 2024-05-09 - Sentinel Refactor: Convert MonolithLogicDriver to TryGetStringField
**Pattern:** Ad-hoc and unsafe parameter extraction via `GetStringField` across multiple action handler files without checking if the field exists, relying on empty string fallbacks.
**Learning:** In Monolith JSON extraction, `GetStringField` can throw warnings or fail if the field is missing. Using `TryGetStringField` gracefully handles missing fields and prevents Unreal Engine's internal `LogJson` warning spam.
**Reuse rule:** Future actions and parameter handlers should use `TryGetStringField` and explicitly check boolean return codes rather than using `GetStringField`.
**Avoid:** Avoid using `GetStringField` directly on `FJsonObject` instances in Monolith action handlers.
## 2026-05-10 - Normalize parameter extraction across MonolithAudio
**Pattern:** Extensive use of `GetStringField` across the MonolithAudio module which warns internally or can fail if JSON keys are absent.
**Learning:** Monolith C++ handlers benefit from using `TryGetStringField` instead of `GetStringField` to improve stability and avoid warning spam.
**Reuse rule:** Future tasks should use `TryGetStringField` instead of `GetStringField` unless the parameter extraction is explicitly guaranteed.
**Avoid:** Writing new handler logic with raw `GetStringField` instead of checking with `TryGetStringField`.

## 2024-05-23 - Normalize MonolithLogicDriver GetNumberField extraction
**Pattern:** Repeated missing-param checks like `HasField(...) ? GetNumberField(...) : Default` in MonolithLogicDriverGraphActions.
**Learning:** TryGetNumberField safely extracts numeric parameters, avoiding ad-hoc type casts directly inside ternary statements and making the error/default handling clearer.
**Reuse rule:** Use TryGetNumberField for extracting optional numbers into an existing variable with a fallback default.
**Avoid:** Duplicated `if HasField then GetNumberField` or `HasField ? GetNumberField : Default` blocks.

## 2026-05-11 - Normalize MonolithUI Param Extraction
**Pattern:** Ad-hoc use of `GetStringField` directly on `Params` objects, which is unsafe when not preceded by type validation or when the field is missing (even with `HasField`, since it could be null or non-string).
**Learning:** Monolith C++ modules should never use direct typed accessors (`GetStringField`, etc.) without `TryGet*Field` since malformed JSON can cause assertion crashes.
**Reuse rule:** Future UI actions and module handlers should consistently use `TryGetStringField` and properly initialize local variables.
**Avoid:** Using `GetStringField`, especially right after a `HasField` check.

## 2026-05-14 - Replace GetStringField with TryGetStringField in AIControllerActions

**Pattern:** Unsafe calls to `GetStringField` for optional parameters caused crashes when the fields were missing from the JSON payload.
**Learning:** Monolith MCP actions should always validate optional JSON fields properly using `TryGetStringField` instead of blindly assuming their existence, even when a schema defines them as optional, because schema validation alone does not populate missing optional JSON fields before handler execution.
**Reuse rule:** Future handlers should prefer using `TryGetStringField` or checking `HasField` prior to accessing optional JSON values.
**Avoid:** Avoid using `GetStringField` directly on `Params` JSON objects unless the parameter has been strictly guaranteed to exist through prior programmatic validation (like `RequireStringParam`).

## 2026-05-14 - Normalize parameter extraction across MonolithNiagara
**Pattern:** Repeated missing-param checks like `HasField(...) ? GetNumberField(...) : Default` and `HasField(...) ? GetStringField(...) : Default` in `MonolithNiagaraActions.cpp`.
**Learning:** `TryGetNumberField` and `TryGetStringField` safely extract numeric and string parameters, avoiding ad-hoc type casts directly inside ternary statements and making the error/default handling clearer and safer against missing fields.
**Reuse rule:** Use `TryGetNumberField` and `TryGetStringField` for extracting optional numbers/strings into an existing variable with a fallback default.
**Avoid:** Duplicated `if HasField then GetNumberField` or `HasField ? GetNumberField : Default` blocks.

## 2026-05-14 - Replace GetStringField with TryGetStringField across AI Actions

**Pattern:** Unsafe `GetStringField` usage in `MonolithAIDiscoveryActions`, `MonolithAIEQSActions`, and `MonolithAIBehaviorTreeActions` for extracting optional parameters or when fields might be missing.
**Learning:** Hard crashes occur when FJsonObject fails to find a field using `GetStringField`. Using `TryGetStringField` gracefully checks presence. The `ToLower()` chain must be wrapped inside the if statement where `TryGetStringField` returns true to retain original behavior safely.
**Reuse rule:** Future AI action handlers should always prefer `TryGetStringField` with a fallback initialization rather than directly chaining `GetStringField().ToLower()`.
**Avoid:** Avoid using `GetStringField` unless field existence is fully guaranteed.

## 2026-05-29 - Safe JSON Schema Extraction for StateTree

**Pattern:** Replaced `HasField(TEXT("schema_class")) ? GetStringField(TEXT("schema_class")) : FString()` with `TryGetStringField` in `MonolithAIStateTreeActions.cpp`.
**Learning:** Optional string extraction should not depend on `GetStringField`; if a supplied JSON value has the wrong type, the handler should keep the default or return a controlled error instead of relying on assertion-prone accessors.
**Reuse rule:** For optional string properties that default to empty strings, initialize an `FString` and use `TryGetStringField`.
**Avoid:** Avoid the ternary `HasField` + `GetStringField` pattern for optional user-supplied JSON fields.

## 2026-05-13 - Replace GetStringField with TryGetStringField in MonolithEditor actions

**Pattern:** Unsafe calls to `GetStringField` for optional or required string parameters across multiple handlers in `MonolithEditorActions.cpp` and `MonolithEditorPreviewActions.cpp` which can crash the editor if fields are missing.
**Learning:** Monolith C++ handlers must prefer `TryGetStringField` over direct `GetStringField` calls. When a field is known optional or required but missing from JSON payload, the former securely falls back to the default or enables an explicit error return.
**Reuse rule:** Initialize an `FString` and use `Params->TryGetStringField` instead of using `GetStringField`. Maintain `const` semantics only if the string is inherently constant and properly initialized.
**Avoid:** Using `GetStringField` directly on the `Params` object, especially without a preceding type and existence guarantee.

## 2024-05-14 - Replace GetStringField with TryGetStringField across Anim Actions

**Pattern:** Unsafe `GetStringField` usage in `MonolithAnimationActions`, `MonolithAbpWriteActions`, and `MonolithAnimLayoutActions` for extracting required parameters.
**Learning:** Hard crashes or silent logic failures occur when FJsonObject fails to find a field using `GetStringField` and we try to consume the invalid value. Using `TryGetStringField` gracefully checks presence. We must check the return value and explicitly return a `FMonolithActionResult::Error` if a required field is missing.
**Reuse rule:** Future animation action handlers should always prefer `TryGetStringField` with an explicit fallback or error return rather than directly calling `GetStringField()`.
**Avoid:** Avoid using `GetStringField` unless field existence is fully guaranteed, and never use `TryGetStringField` without checking its boolean return value.

## 2026-06-06 - Replace GetStringField with TryGetStringField across GAS Actions

**Pattern:** Unsafe `GetStringField` usage in `MonolithGASASCActions`, `MonolithGASCueActions`, and `MonolithGASEffectActions` for extracting optional parameters or when fields might be missing.
**Learning:** Hard crashes or warning spam occur when FJsonObject fails to find a field using `GetStringField`. Using `TryGetStringField` gracefully checks presence and is safer.
**Reuse rule:** Future GAS action handlers should always prefer `TryGetStringField` with a fallback initialization rather than directly chaining `GetStringField()`.
**Avoid:** Avoid using `GetStringField` unless field existence is fully guaranteed.
