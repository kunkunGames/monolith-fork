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
