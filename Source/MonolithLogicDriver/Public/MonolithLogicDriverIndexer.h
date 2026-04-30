#pragma once

#include "CoreMinimal.h"
#include "MonolithIndexer.h"

#if WITH_LOGICDRIVER

/**
 * FStateMachineIndexer -- deep indexer for Logic Driver SM assets.
 * Registers into MonolithIndex at startup. Indexes SM Blueprints,
 * Node Blueprints, and component references for cross-reference queries.
 */
class FStateMachineIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override;
	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override;
};

#endif // WITH_LOGICDRIVER
