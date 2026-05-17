#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.repair_crg_cache - dry-run by default; rebuilds derived CRG projection/cache only when execute=true. */
class FProjectRepairCrgCacheAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("repair_crg_cache"); }
	static FString GetDescription() { return TEXT("Rebuild derived ProjectIndex CRG projection/cache tables. Dry-run unless execute=true"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
