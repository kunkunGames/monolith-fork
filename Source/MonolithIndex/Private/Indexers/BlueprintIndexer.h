#pragma once

#include "MonolithIndexer.h"

/**
 * Indexes Blueprints: graphs, nodes, pins, connections, variables.
 * Walks every UEdGraph in the Blueprint, extracts node topology,
 * pin connections, and variable declarations.
 */
class FBlueprintIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		// C2 (PRD AssetSearchSemanticSearch): "WidgetBlueprint" is intentionally NOT listed here so
		// the single-class ClassToIndexer dispatch hands widget BPs to the dedicated UMG-aware
		// FWidgetBlueprintIndexer (which still performs the graph/variable passes, losing no coverage).
		return { TEXT("Blueprint"), TEXT("AnimBlueprint") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("BlueprintIndexer"); }

private:
	void IndexGraph(class UEdGraph* Graph, FMonolithIndexDatabase& DB, int64 AssetId, class FMonolithSearchValueWriter& SearchValues);
	void IndexVariables(class UBlueprint* Blueprint, FMonolithIndexDatabase& DB, int64 AssetId);
};
