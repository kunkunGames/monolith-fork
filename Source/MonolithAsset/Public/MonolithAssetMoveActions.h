#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class MONOLITHASSET_API FMonolithAssetMoveActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static FMonolithActionResult MoveAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CleanupMovedRedirectors(const TSharedPtr<FJsonObject>& Params);
};
