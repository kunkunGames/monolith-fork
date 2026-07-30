#pragma once

#include "Containers/BitArray.h"
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithCollection
{
	/**
	 * Counts one successful dynamic-filter match per logical asset while still
	 * allowing every Content Browser alias to be evaluated.
	 *
	 * A bitset is retained for each asset that matched at least one filter.
	 * This avoids the much larger per-collection arrays of full object paths
	 * while preserving alias-sensitive query semantics.
	 */
	class FDynamicCollectionMatchCounter
	{
	public:
		bool RecordMatch(
			const FSoftObjectPath& AssetPath,
			int32 FilterIndex,
			int32 FilterCount);

	private:
		TMap<FSoftObjectPath, TBitArray<>> CountedFiltersByAsset;
	};
}

class FAssetCollectionActions
{
public:
	static void Register(FMonolithToolRegistry& Registry);

	static FMonolithActionResult ListCollections(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetCollection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateCollection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteCollection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ContainsAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetDynamicQuery(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetDynamicQuery(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetCollectionColor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateCollectionName(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateUniqueCollectionName(const TSharedPtr<FJsonObject>& Params);
};
