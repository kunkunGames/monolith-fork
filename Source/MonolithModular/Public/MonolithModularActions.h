#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithModularActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeExtensionReceiverLifecycle(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateAddComponentTargets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult TraceGameFrameworkExtensionEvents(const TSharedPtr<FJsonObject>& Params);
};
