#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FProjectRepairFtsAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("repair_fts"); }
	static FString GetDescription() { return TEXT("Dry-run or rebuild the existing project asset/node FTS5 indexes"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
