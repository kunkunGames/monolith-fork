#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithPCGActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListGraphAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGraphAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListComponents(const TSharedPtr<FJsonObject>& Params);
};
