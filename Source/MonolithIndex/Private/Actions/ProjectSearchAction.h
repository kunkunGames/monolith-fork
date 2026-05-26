#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FProjectSearchAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("search"); }
	static FString GetDescription()
	{
		return TEXT("Read-only full-text discovery across indexed project assets, nodes, variables, parameters, and content values. Results are search provenance only; use describe/action schema before mutation.");
	}
	static TSharedPtr<FJsonObject> GetSchema();
};
