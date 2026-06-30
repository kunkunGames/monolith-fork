#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithLoadingActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeLoadingProcessors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateLoadingReasonContract(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult TraceLoadingScreenBlockers(const TSharedPtr<FJsonObject>& Params);
};
