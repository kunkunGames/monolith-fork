#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"

class UEdGraph;
class UEdGraphNode;

/**
 * Diagnostic info returned by GetFormatterInfo().
 */
struct FMonolithFormatterInfo
{
	/** Human-readable formatter type: "Blueprint", "BehaviorTree", "Simple", "Unsupported" */
	FString FormatterType;

	/** Whether this graph type is supported by the formatter */
	bool bIsSupported = false;

	/** The UEdGraph subclass name (e.g. "MaterialGraph", "AnimationGraph") */
	FString GraphClassName;
};

/**
 * Abstract interface for external graph formatting providers.
 * Registered via IModularFeatures. MonolithBABridge is the canonical implementation.
 *
 * Consumers call IsAvailable() then Get().FormatGraph() -- zero BA dependency.
 */
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
	 * External formatters mutate graph assets outside Monolith's built-in layout path.
	 * They are allowed only when the action has no built-in Monolith formatter.
	 */
	static bool IsExternalMutationFormattingEnabled(bool bHasBuiltInFormatter = true)
	{
		return !bHasBuiltInFormatter;
	}

	static FString GetExternalMutationFormattingDisabledMessage()
	{
		return TEXT("Blueprint Assist formatting is disabled for Monolith asset mutation actions that have a built-in formatter.");
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
