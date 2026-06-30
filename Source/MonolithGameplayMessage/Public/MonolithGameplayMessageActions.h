#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithGameplayMessageActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeListenerContract(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateMessageStruct(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateChannelContract(const TSharedPtr<FJsonObject>& Params);
};
