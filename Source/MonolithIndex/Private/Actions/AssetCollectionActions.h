#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

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
