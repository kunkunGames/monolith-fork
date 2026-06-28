#pragma once

#include "MonolithIndexer.h"

/**
 * AssetRegistry-only domain indexer for graph-like systems that are risky or
 * expensive to deep-load during a project-wide index pass.
 *
 * Emits searchable metadata for ControlRig/RigVM, StateTree, and Chooser assets
 * into asset_search_values with source_kind "domain_asset". It intentionally
 * does not inspect loaded UObject graphs.
 */
class FDomainAssetIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__DomainAssets__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("DomainAssetIndexer"); }
	virtual int32 GetIndexerVersion() const override { return 1; }
	virtual bool IsSentinel() const override { return true; }
	virtual bool SupportsIncrementalIndex() const override { return true; }
	virtual bool IndexScoped(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths, FMonolithIndexDatabase& DB) override;
};
