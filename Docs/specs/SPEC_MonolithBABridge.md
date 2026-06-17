# Monolith — MonolithBABridge Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.20.1 (Beta)

---

## MonolithBABridge

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, BlueprintAssist (optional)

MonolithBABridge is an **optional** editor module that bridges Blueprint Assist's graph formatter into Monolith's graph formatting interface. It registers no MCP actions of its own. The bridge remains available for diagnostics and as a fallback formatter for asset mutation actions only when the target domain has no built-in Monolith formatter.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBABridgeModule` | IModuleInterface. On startup, registers `IMonolithGraphFormatter` immediately when Blueprint Assist is already loaded; otherwise subscribes to `FModuleManager::OnModulesChanged()` and registers when `BlueprintAssist` loads, then removes the delegate handle. |
| `FMonolithBAFormatterImpl` | Concrete `IMonolithGraphFormatter` impl. Delegates to BA's `FBAGraphHandler`. Checks `IsCalculatingNodeSize()` before formatting. |

### IMonolithGraphFormatter Interface

```cpp
class IMonolithGraphFormatter : public IModularFeature
{
public:
    static FName GetModularFeatureName()
    {
        static const FName Name(TEXT("MonolithGraphFormatter"));
        return Name;
    }

    /** Check if this formatter can handle the given graph type */
    virtual bool SupportsGraph(UEdGraph* Graph) const = 0;

    /**
     * Format an entire graph.
     * @param Graph             The graph to format (must be open in an editor tab)
     * @param OutNodesFormatted Number of nodes repositioned
     * @param OutErrorMessage   Populated on failure
     * @return true on success
     *
     * Precondition: The asset MUST be open in an editor tab.
     */
    virtual bool FormatGraph(
        UEdGraph* Graph,
        int32& OutNodesFormatted,
        FString& OutErrorMessage) = 0;

    /**
     * Get diagnostic info about what formatter would be used for this graph.
     * Returns FMonolithFormatterInfo with type, support status, and graph class name.
     */
    virtual FMonolithFormatterInfo GetFormatterInfo(UEdGraph* Graph) const = 0;

    // --- Static helpers for consumers ---

    /** Check if any graph formatter is registered */
    static bool IsAvailable()
    {
        return IModularFeatures::Get().IsModularFeatureAvailable(GetModularFeatureName());
    }

    static bool IsExternalMutationFormattingEnabled(bool bHasBuiltInFormatter = true)
    {
        return !bHasBuiltInFormatter;
    }

    /**
     * Get the registered formatter (check IsAvailable() first!).
     * Returns the first registered provider.
     */
    static IMonolithGraphFormatter& Get()
    {
        return IModularFeatures::Get().GetModularFeature<IMonolithGraphFormatter>(
            GetModularFeatureName());
    }
};
```

External formatter calls must be gated when an action mutates an asset. Pass `false` only for domains that do not have a built-in Monolith formatter:

```cpp
// Check at call time — BA may not be loaded
if (IMonolithGraphFormatter::IsExternalMutationFormattingEnabled(/*bHasBuiltInFormatter=*/false)
    && IMonolithGraphFormatter::IsAvailable())
{
    int32 NodesFormatted = 0;
    FString ErrorMsg;
    IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, ErrorMsg);
}
```

### `formatter` Parameter (three-mode behavior)

Asset-mutating `auto_layout` actions accept an optional `formatter` param. Blueprint Assist is disabled when a built-in Monolith formatter exists and allowed only as fallback where no built-in formatter exists:

| Value | Behavior |
|-------|----------|
| `"auto"` (default) | Uses built-in layout when the domain has one; otherwise uses Blueprint Assist when the bridge is available and the graph type is supported. |
| `"blueprint_assist"` | Allowed only for domains with no built-in formatter; domains with a built-in formatter return an explicit disabled error. |
| `"builtin"` / `"monolith"` | Forces built-in layout when supported; returns unsupported when the domain has no built-in formatter. |

### `bEnableBlueprintAssist` Setting

`UMonolithSettings` exposes a toggle that controls whether MonolithBABridge attempts registration on startup:

| Setting | Default | Description |
|---------|---------|-------------|
| `bEnableBlueprintAssist` | True | When false, MonolithBABridge skips `IModularFeatures` registration even if Blueprint Assist is present. Asset mutation actions use built-in formatters when available; BA formatting is fully disabled and unavailable for all domains. |

**Config key:** `bEnableBlueprintAssist` in `[/Script/MonolithCore.MonolithSettings]`
