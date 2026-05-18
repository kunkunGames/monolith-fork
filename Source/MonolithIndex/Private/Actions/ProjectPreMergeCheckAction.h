#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.pre_merge_check — advisory pre-merge gate over project index signals. */
class FProjectPreMergeCheckAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("pre_merge_check"); }
	static FString GetDescription() { return TEXT("Compose project health, changed asset risk, impact, and optional unused checks into a pre-merge decision"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
