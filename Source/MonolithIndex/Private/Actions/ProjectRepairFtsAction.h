#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.repair_fts — dry-run by default; rebuilds FTS only when execute=true. */
class FProjectRepairFtsAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("repair_fts"); }
	static FString GetDescription() { return TEXT("Rebuild ProjectIndex FTS tables (fts_assets/fts_nodes). Dry-run unless execute=true"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
