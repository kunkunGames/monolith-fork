#include "MonolithSQLiteMaintenance.h"

#include "MonolithJsonUtils.h"
#include "SQLiteDatabase.h"

bool RunMonolithSQLiteMaintenance(FSQLiteDatabase& Database, const FMonolithSQLiteMaintenanceOptions& Options)
{
	bool bSuccess = true;

	if (Options.bRunPragmaOptimize)
	{
		const TCHAR* OptimizeSQL = Options.bUseFullOptimizeScan
			? TEXT("PRAGMA optimize=0x10002;")
			: TEXT("PRAGMA optimize;");
		if (!Database.Execute(OptimizeSQL))
		{
			UE_LOG(LogMonolith, Warning, TEXT("SQLite maintenance failed: %s -- %s"), OptimizeSQL, *Database.GetLastError());
			bSuccess = false;
		}
	}

	for (const FString& FtsTable : Options.FtsTablesToOptimize)
	{
		if (FtsTable.IsEmpty())
		{
			continue;
		}

		const FString SQL = FString::Printf(TEXT("INSERT INTO %s(%s) VALUES('optimize');"), *FtsTable, *FtsTable);
		if (!Database.Execute(*SQL))
		{
			UE_LOG(LogMonolith, Warning, TEXT("SQLite FTS optimize failed for %s: %s"), *FtsTable, *Database.GetLastError());
			bSuccess = false;
		}
	}

	if (Options.bRunIncrementalVacuum)
	{
		const int32 PageBudget = FMath::Max(0, Options.IncrementalVacuumPageBudget);
		const FString SQL = FString::Printf(TEXT("PRAGMA incremental_vacuum(%d);"), PageBudget);
		if (!Database.Execute(*SQL))
		{
			UE_LOG(LogMonolith, Warning, TEXT("SQLite incremental_vacuum failed: %s"), *Database.GetLastError());
			bSuccess = false;
		}
	}

	return bSuccess;
}
