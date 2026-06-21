#pragma once

#include "MonolithIndexer.h"

/**
 * UMG-aware indexer for Widget Blueprints.
 *
 * Routed the "WidgetBlueprint" asset class away from the generic FBlueprintIndexer because the
 * generic path only walks UEdGraphs + NewVariables and never reaches the widget tree or the
 * runtime delegate bindings. This indexer additionally emits, into asset_search_values:
 *   - per source widget: DisplayLabel (fallback GetName) and "<Name>_Class" -> widget class,
 *     plus top-level editor-visible reflected scalar widget properties.
 *   - per FDelegateRuntimeBinding on the generated class:
 *     "[Binding] <ObjectName>.<PropertyName>" -> SourcePath (property path) or FunctionName.
 *
 * It still performs the same graph/node/connection/variable indexing the generic blueprint
 * indexer did, so widget BPs lose no prior coverage.
 *
 * Pattern + field set verified against the UE 5.7 AssetSearch reference
 * (Engine/Plugins/Editor/AssetSearch/Source/Private/Indexers/WidgetBlueprintIndexer.cpp).
 */
class FWidgetBlueprintIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("WidgetBlueprint") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("WidgetBlueprintIndexer"); }

private:
	/** Re-uses the generic blueprint graph/node/variable passes so widget BPs keep prior coverage. */
	void IndexBlueprintGraphsAndVariables(class UWidgetBlueprint* WidgetBlueprint, FMonolithIndexDatabase& DB, int64 AssetId, class FMonolithSearchValueWriter& SearchValues);

	/** Emits widget-tree labels/classes + top-level reflected widget props. */
	void IndexSourceWidgets(class UWidgetBlueprint* WidgetBlueprint, int64 AssetId, class FMonolithSearchValueWriter& SearchValues);

	/** Emits the generated class FDelegateRuntimeBinding rows. */
	void IndexDelegateBindings(class UWidgetBlueprint* WidgetBlueprint, int64 AssetId, class FMonolithSearchValueWriter& SearchValues);
};
