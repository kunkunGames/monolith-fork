#include "MonolithSQLitePragmaPolicy.h"

#include "MonolithJsonUtils.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/Paths.h"
#include "SQLitePreparedStatement.h"

namespace
{
bool ExecutePragma(FSQLiteDatabase& Database, const FString& SQL, const TCHAR* Label)
{
	if (!Database.Execute(*SQL))
	{
		UE_LOG(LogMonolith, Warning, TEXT("SQLite pragma failed (%s): %s -- %s"), Label, *SQL, *Database.GetLastError());
		return false;
	}
	return true;
}

bool ReadPragmaInt(FSQLiteDatabase& Database, const TCHAR* Name, int64& OutValue)
{
	const FString SQL = FString::Printf(TEXT("PRAGMA %s;"), Name);
	return Database.Execute(*SQL, [&OutValue](const FSQLitePreparedStatement& Statement)
	{
		Statement.GetColumnValueByIndex(0, OutValue);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	}) == 1;
}

bool ReadPragmaString(FSQLiteDatabase& Database, const TCHAR* Name, FString& OutValue)
{
	const FString SQL = FString::Printf(TEXT("PRAGMA %s;"), Name);
	return Database.Execute(*SQL, [&OutValue](const FSQLitePreparedStatement& Statement)
	{
		Statement.GetColumnValueByIndex(0, OutValue);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	}) == 1;
}

const TCHAR* LexOpenMode(ESQLiteDatabaseOpenMode OpenMode)
{
	switch (OpenMode)
	{
	case ESQLiteDatabaseOpenMode::ReadOnly:
		return TEXT("ReadOnly");
	case ESQLiteDatabaseOpenMode::ReadWrite:
		return TEXT("ReadWrite");
	case ESQLiteDatabaseOpenMode::ReadWriteCreate:
		return TEXT("ReadWriteCreate");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* LexRole(EMonolithSQLiteConnectionRole Role)
{
	return Role == EMonolithSQLiteConnectionRole::WriteHeavy ? TEXT("WriteHeavy") : TEXT("ReadMostly");
}
}

ESQLiteDatabaseOpenMode GetMonolithSQLiteOpenMode(EMonolithSQLiteIntent Intent)
{
	switch (Intent)
	{
	case EMonolithSQLiteIntent::QueryOnly:
		return ESQLiteDatabaseOpenMode::ReadOnly;
	case EMonolithSQLiteIntent::UpdateExisting:
		return ESQLiteDatabaseOpenMode::ReadWrite;
	case EMonolithSQLiteIntent::CreateOrRebuild:
		return ESQLiteDatabaseOpenMode::ReadWriteCreate;
	default:
		return ESQLiteDatabaseOpenMode::ReadOnly;
	}
}

FMonolithSQLitePragmaPreset CapPragmaPresetByAvailableMemory(FMonolithSQLitePragmaPreset Preset, uint64 AvailableRAM_MB)
{
	if (AvailableRAM_MB == 0)
	{
		return Preset;
	}

	if (AvailableRAM_MB < 1024)
	{
		Preset.MmapSizeBytes = FMath::Min<int64>(Preset.MmapSizeBytes, 33554432);
		Preset.CacheSizeKiB = FMath::Max<int64>(Preset.CacheSizeKiB, -8000);
		Preset.bUseMemoryTempStore = false;
		return Preset;
	}

	const int64 AvailableBytes = static_cast<int64>(AvailableRAM_MB * 1024ULL * 1024ULL);
	const int64 AvailableKiB = static_cast<int64>(AvailableRAM_MB * 1024ULL);
	const int64 MaxMmapBytes = FMath::Max<int64>(33554432, AvailableBytes / 8);
	const int64 MaxCacheKiB = FMath::Max<int64>(8000, AvailableKiB / 16);

	Preset.MmapSizeBytes = FMath::Min<int64>(Preset.MmapSizeBytes, MaxMmapBytes);
	Preset.CacheSizeKiB = FMath::Max<int64>(Preset.CacheSizeKiB, -MaxCacheKiB);
	if (AvailableRAM_MB < 2048)
	{
		Preset.bUseMemoryTempStore = false;
	}
	return Preset;
}

FMonolithSQLitePragmaPreset SelectMonolithSQLitePragmaPreset(uint64 TotalRAM_MB, bool bIs64Bit, uint64 AvailableRAM_MB)
{
	FMonolithSQLitePragmaPreset Preset;
	if (!bIs64Bit)
	{
		Preset = {33554432, -8000, false};
		return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
	}

	if (TotalRAM_MB >= 65536)
	{
		Preset = {2147483648, -512000, true};
		return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
	}
	if (TotalRAM_MB >= 32768)
	{
		Preset = {1073741824, -256000, true};
		return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
	}
	if (TotalRAM_MB >= 16384)
	{
		Preset = {536870912, -128000, true};
		return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
	}
	if (TotalRAM_MB >= 8192)
	{
		Preset = {268435456, -64000, true};
		return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
	}
	Preset = {67108864, -16000, false};
	return CapPragmaPresetByAvailableMemory(Preset, AvailableRAM_MB);
}

bool OpenMonolithSQLiteDatabase(
	FSQLiteDatabase& Database,
	const FString& Path,
	const FMonolithSQLiteOpenPolicy& Policy,
	FMonolithSQLiteTuningResult* OutObserved)
{
	FMonolithSQLiteTuningResult Observed;
	Observed.OpenMode = GetMonolithSQLiteOpenMode(Policy.Intent);

	const bool bWriteCapable = Observed.OpenMode != ESQLiteDatabaseOpenMode::ReadOnly;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	Observed.bFreshDatabase = !PlatformFile.FileExists(*Path);

	if (Observed.OpenMode == ESQLiteDatabaseOpenMode::ReadWriteCreate)
	{
		const FString Dir = FPaths::GetPath(Path);
		if (!Dir.IsEmpty() && !PlatformFile.DirectoryExists(*Dir))
		{
			PlatformFile.CreateDirectoryTree(*Dir);
		}
	}

	if (!Database.Open(*Path, Observed.OpenMode))
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to open SQLite database: mode=%s path=%s error=%s"),
			LexOpenMode(Observed.OpenMode), *Path, *Database.GetLastError());
		if (OutObserved)
		{
			*OutObserved = Observed;
		}
		return false;
	}

	if (bWriteCapable && Policy.bApplyCreateOnlyPragmas && Observed.bFreshDatabase)
	{
		ExecutePragma(Database, TEXT("PRAGMA page_size=4096;"), TEXT("page_size"));
		ExecutePragma(Database, TEXT("PRAGMA auto_vacuum=INCREMENTAL;"), TEXT("auto_vacuum"));
	}

	if (bWriteCapable)
	{
		ExecutePragma(Database, TEXT("PRAGMA journal_mode=DELETE;"), TEXT("journal_mode"));
		ExecutePragma(Database, TEXT("PRAGMA synchronous=NORMAL;"), TEXT("synchronous"));

		const int32 Threads = FMath::Clamp(FPlatformMisc::NumberOfCoresIncludingHyperthreads(), 0, 4);
		ExecutePragma(Database, FString::Printf(TEXT("PRAGMA threads=%d;"), Threads), TEXT("threads"));

		if (Policy.bEnableForeignKeys)
		{
			ExecutePragma(Database, TEXT("PRAGMA foreign_keys=ON;"), TEXT("foreign_keys"));
		}
	}

	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	const uint64 TotalRAM_MB = MemoryStats.TotalPhysical / (1024ULL * 1024ULL);
	const uint64 AvailableRAM_MB = MemoryStats.AvailablePhysical / (1024ULL * 1024ULL);
	const FMonolithSQLitePragmaPreset Preset = SelectMonolithSQLitePragmaPreset(TotalRAM_MB, PLATFORM_64BITS != 0, AvailableRAM_MB);
	ExecutePragma(Database, FString::Printf(TEXT("PRAGMA mmap_size=%lld;"), Preset.MmapSizeBytes), TEXT("mmap_size"));
	ExecutePragma(Database, FString::Printf(TEXT("PRAGMA cache_size=%lld;"), Preset.CacheSizeKiB), TEXT("cache_size"));
	if (Preset.bUseMemoryTempStore)
	{
		ExecutePragma(Database, TEXT("PRAGMA temp_store=2;"), TEXT("temp_store"));
	}

	int32 ApplicationId = 0;
	if (Database.GetApplicationId(ApplicationId))
	{
		Observed.ApplicationId = ApplicationId;
		if (ApplicationId == 0)
		{
			if (bWriteCapable && Policy.bAllowLegacyApplicationIdStamp)
			{
				Observed.bApplicationIdStamped = Database.SetApplicationId(MonolithSQLiteApplicationId);
				if (!Observed.bApplicationIdStamped)
				{
					UE_LOG(LogMonolith, Warning, TEXT("SQLite legacy application_id stamp failed: %s"), *Path);
				}
				else
				{
					Observed.ApplicationId = MonolithSQLiteApplicationId;
				}
			}
			else
			{
				Observed.bLegacyApplicationIdPendingStamp = true;
				UE_LOG(LogMonolith, Display, TEXT("SQLite legacy application_id pending writable open: %s"), *Path);
			}
		}
		else if (ApplicationId != MonolithSQLiteApplicationId)
		{
			UE_LOG(LogMonolith, Error, TEXT("Refusing foreign SQLite database: path=%s application_id=0x%08x expected=0x%08x"),
				*Path, ApplicationId, MonolithSQLiteApplicationId);
			Database.Close();
			if (OutObserved)
			{
				*OutObserved = Observed;
			}
			return false;
		}
	}

	if (Policy.bVerifyIntegrity)
	{
		Observed.bIntegrityCheckRan = true;
		Observed.bIntegrityOk = Database.PerformQuickIntegrityCheck();
		if (!Observed.bIntegrityOk)
		{
			UE_LOG(LogMonolith, Warning, TEXT("SQLite quick_check failed: %s"), *Path);
			Database.Close();
			if (OutObserved)
			{
				*OutObserved = Observed;
			}
			return false;
		}
	}

	int64 ReadbackInt = 0;
	ReadPragmaString(Database, TEXT("journal_mode"), Observed.JournalMode);
	if (ReadPragmaInt(Database, TEXT("mmap_size"), ReadbackInt)) Observed.MmapSize = ReadbackInt;
	if (ReadPragmaInt(Database, TEXT("cache_size"), ReadbackInt)) Observed.CacheSize = ReadbackInt;
	if (ReadPragmaInt(Database, TEXT("temp_store"), ReadbackInt)) Observed.TempStore = static_cast<int32>(ReadbackInt);
	if (ReadPragmaInt(Database, TEXT("foreign_keys"), ReadbackInt)) Observed.ForeignKeys = static_cast<int32>(ReadbackInt);
	if (ReadPragmaInt(Database, TEXT("threads"), ReadbackInt)) Observed.Threads = static_cast<int32>(ReadbackInt);
	if (ReadPragmaInt(Database, TEXT("page_size"), ReadbackInt)) Observed.PageSize = static_cast<int32>(ReadbackInt);
	if (ReadPragmaInt(Database, TEXT("auto_vacuum"), ReadbackInt)) Observed.AutoVacuum = static_cast<int32>(ReadbackInt);

	UE_LOG(LogMonolith, Log,
		TEXT("SQLite tuning applied: role=%s, mode=%s, journal=%s, mmap_size=%lld, cache_size=%lld, temp_store=%d, foreign_keys=%d, threads=%d, page_size=%d, auto_vacuum=%d, app_id=0x%08x"),
		LexRole(Policy.Role),
		LexOpenMode(Observed.OpenMode),
		*Observed.JournalMode,
		Observed.MmapSize,
		Observed.CacheSize,
		Observed.TempStore,
		Observed.ForeignKeys,
		Observed.Threads,
		Observed.PageSize,
		Observed.AutoVacuum,
		Observed.ApplicationId);

	if (OutObserved)
	{
		*OutObserved = Observed;
	}
	return true;
}
