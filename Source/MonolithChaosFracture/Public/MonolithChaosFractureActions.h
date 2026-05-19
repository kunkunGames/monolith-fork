#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithChaosFractureActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListGeometryCollectionAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListGeometryCollectionComponents(const TSharedPtr<FJsonObject>& Params);
};
