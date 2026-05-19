#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

class FMonolithWorldConditionsActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleGetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListQueryOwners(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDescribeQuery(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDescribeConditionTypes(const TSharedPtr<FJsonObject>& Params);
};
