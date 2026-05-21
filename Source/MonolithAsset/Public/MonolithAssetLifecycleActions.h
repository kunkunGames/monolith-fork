#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class MONOLITHASSET_API FMonolithAssetLifecycleActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult ImportTextureFromFile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteAssets(const TSharedPtr<FJsonObject>& Params);
};
