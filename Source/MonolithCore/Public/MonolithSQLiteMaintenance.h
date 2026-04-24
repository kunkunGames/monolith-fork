#pragma once

#include "CoreMinimal.h"

class FSQLiteDatabase;

struct MONOLITHCORE_API FMonolithSQLiteMaintenanceOptions
{
	bool bRunPragmaOptimize = false;
	bool bUseFullOptimizeScan = false;
	bool bRunIncrementalVacuum = false;
	int32 IncrementalVacuumPageBudget = 1024;
	TArray<FString> FtsTablesToOptimize;
};

MONOLITHCORE_API bool RunMonolithSQLiteMaintenance(
	FSQLiteDatabase& Database,
	const FMonolithSQLiteMaintenanceOptions& Options);
