# Monolith — MonolithBABridge Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithBABridge

**Dependencies:** Core, CoreUObject, Engine, MonolithCore (optional — loads only when both Monolith and Blueprint Assist are present)

MonolithBABridge is an **optional** editor module that bridges Blueprint Assist's graph formatter into Monolith's `auto_layout` actions. It registers no MCP actions of its own. Its sole job is to expose BA's layout logic via `IModularFeatures` so that blueprint, material, animation, and niagara modules can consume it without a hard dependency on Blueprint Assist.

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

Consumer pattern used by `auto_layout` actions in each domain module:

```cpp
// Check at call time — BA may not be loaded
if (IMonolithGraphFormatter::IsAvailable())
{
    int32 NodesFormatted = 0;
    FString ErrorMsg;
    IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, ErrorMsg);
}
```

### `formatter` Parameter (three-mode behavior)

All four `auto_layout` actions accept an optional `formatter` param:

| Value | Behavior |
|-------|----------|
| `"auto"` (default) | Uses Blueprint Assist formatter if `IMonolithGraphFormatter` is registered; otherwise falls back to built-in hierarchical layout. Never errors |
| `"blueprint_assist"` | Forces BA formatter. Returns an error if MonolithBABridge is not loaded or BA is not present |
| `"builtin"` | Forces built-in layout regardless of BA presence |

### `bEnableBlueprintAssist` Setting

`UMonolithSettings` exposes a toggle that controls whether MonolithBABridge attempts registration on startup:

| Setting | Default | Description |
|---------|---------|-------------|
| `bEnableBlueprintAssist` | True | When false, MonolithBABridge skips `IModularFeatures` registration even if Blueprint Assist is present. `formatter: "auto"` will fall back to built-in; `formatter: "blueprint_assist"` will error |

**Config key:** `bEnableBlueprintAssist` in `[/Script/MonolithCore.MonolithSettings]`
