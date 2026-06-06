#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithWaterActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static int32 ClampWaterLimit(double LimitValue);

private:
	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListBodies(const TSharedPtr<FJsonObject>& Params);
};
