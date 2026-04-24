#pragma once

#include "CoreMinimal.h"
#include "SQLiteDatabase.h"

enum class EMonolithSQLiteConnectionRole : uint8
{
	ReadMostly,
	WriteHeavy
};

enum class EMonolithSQLiteIntent : uint8
{
	QueryOnly,
	UpdateExisting,
	CreateOrRebuild
};

struct MONOLITHCORE_API FMonolithSQLitePragmaPreset
{
	int64 MmapSizeBytes = 0;
	int64 CacheSizeKiB = 0;
	bool bUseMemoryTempStore = false;
};

struct MONOLITHCORE_API FMonolithSQLiteTuningResult
{
	ESQLiteDatabaseOpenMode OpenMode = ESQLiteDatabaseOpenMode::ReadOnly;
	FString JournalMode;
	int64 MmapSize = 0;
	int64 CacheSize = 0;
	int32 TempStore = 0;
	int32 ForeignKeys = 0;
	int32 Threads = 0;
	int32 PageSize = 0;
	int32 AutoVacuum = 0;
	int32 ApplicationId = 0;
	bool bFreshDatabase = false;
	bool bApplicationIdStamped = false;
	bool bLegacyApplicationIdPendingStamp = false;
	bool bIntegrityCheckRan = false;
	bool bIntegrityOk = true;
};

struct MONOLITHCORE_API FMonolithSQLiteOpenPolicy
{
	EMonolithSQLiteIntent Intent = EMonolithSQLiteIntent::QueryOnly;
	EMonolithSQLiteConnectionRole Role = EMonolithSQLiteConnectionRole::ReadMostly;
	bool bEnableForeignKeys = false;
	bool bVerifyIntegrity = false;
	bool bApplyCreateOnlyPragmas = true;
	bool bAllowLegacyApplicationIdStamp = true;
};

inline constexpr int32 MonolithSQLiteApplicationId = 0x4D4F4E4C; // "MONL"

MONOLITHCORE_API ESQLiteDatabaseOpenMode GetMonolithSQLiteOpenMode(EMonolithSQLiteIntent Intent);
MONOLITHCORE_API FMonolithSQLitePragmaPreset CapPragmaPresetByAvailableMemory(FMonolithSQLitePragmaPreset Preset, uint64 AvailableRAM_MB);
MONOLITHCORE_API FMonolithSQLitePragmaPreset SelectMonolithSQLitePragmaPreset(uint64 TotalRAM_MB, bool bIs64Bit, uint64 AvailableRAM_MB = 0);
MONOLITHCORE_API bool OpenMonolithSQLiteDatabase(
	FSQLiteDatabase& Database,
	const FString& Path,
	const FMonolithSQLiteOpenPolicy& Policy,
	FMonolithSQLiteTuningResult* OutObserved = nullptr);
