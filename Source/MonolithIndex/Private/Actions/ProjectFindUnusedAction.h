#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.find_unused — advisory orphan-asset candidates. */
class FProjectFindUnusedAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("find_unused"); }
	static FString GetDescription() { return TEXT("Find advisory orphan-asset candidates with confidence and reasons; read-only"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
