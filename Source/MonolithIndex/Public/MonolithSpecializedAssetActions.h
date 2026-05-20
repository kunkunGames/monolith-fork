#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithSpecializedAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListSupportedAssetEnrichers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult InspectAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult InspectAssetsBatch(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateSpecializedAsset(const TSharedPtr<FJsonObject>& Params);
};
