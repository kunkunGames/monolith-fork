#include "MonolithSourceSubsystem.h"
#include "MonolithSourceIndexer.h"
#include "MonolithSettings.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Async/Async.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/UObjectGlobals.h" // F17: FCoreUObjectDelegates::ReloadCompleteDelegate
#include "UObject/Object.h"

namespace
{
	constexpr int32 SourceDbOpenRetryAttempts = 3;
	constexpr float SourceDbOpenRetryDelaySeconds = 0.10f;
	constexpr double SourceDbOpenFailureCooldownSeconds = 2.0;

	void RebuildSourceCrgCacheAfterIndexing(FMonolithSourceDatabase* Database, const TCHAR* Context)
	{
		if (!Database || !Database->IsOpen())
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("%s complete but EngineSource DB is not open; skipped source CRG projection/cache rebuild"), Context);
			return;
		}

		TSharedPtr<FJsonObject> CrgResult = Database->RepairCrgCache(true);
		FString Status;
		FString Summary;
		if (CrgResult.IsValid())
		{
			CrgResult->TryGetStringField(TEXT("status"), Status);
			CrgResult->TryGetStringField(TEXT("summary"), Summary);
		}

		if (Status == TEXT("ok"))
		{
			UE_LOG(LogMonolithSource, Log, TEXT("%s complete; source CRG projection/cache rebuilt: %s"), Context, *Summary);
		}
		else
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("%s complete but source CRG projection/cache rebuild did not complete cleanly: %s"), Context, *Summary);
		}
	}
}

UMonolithSourceSubsystem::~UMonolithSourceSubsystem()
{
	bIsDeinitializing = true;
	delete Indexer;
	Indexer = nullptr;
}

void UMonolithSourceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bIsDeinitializing = false;

	// Commandlet mode (cook/compile): skip DB open. Build/cook commandlets do not need the
	// editor-owned source index, and avoiding a second long-lived DB handle keeps source
	// indexing ownership in the editor or explicit reindex commandlet.
	if (IsRunningCommandlet())
	{
		return;
	}

	Database = MakeUnique<FMonolithSourceDatabase>();
	TryOpenDatabaseWithRetry(GetDatabasePath(), TEXT("Initialize"));

	// DB reads stay available regardless of indexing activation. Writer hooks
	// and the startup catch-up run are conditional.
	SetAutomaticIndexingEnabled(true);
	StartPreferredIndex();
}

void UMonolithSourceSubsystem::Deinitialize()
{
	bIsDeinitializing = true;

	// Unbind the hot-reload hook BEFORE we tear down anything else, so a
	// late-firing reload signal cannot re-enter a half-destroyed subsystem.
	SetAutomaticIndexingEnabled(false);

	// Stop any running indexer
	if (Indexer)
	{
		Indexer->RequestStop();
		delete Indexer;
		Indexer = nullptr;
	}

	if (Database.IsValid())
	{
		Database->Close();
	}
	Super::Deinitialize();
}

FMonolithSourceDatabase* UMonolithSourceSubsystem::GetDatabase()
{
	if (!EnsureDatabaseOpen() || !Database.IsValid() || !Database->IsOpen())
	{
		return nullptr;
	}
	return Database.Get();
}

bool UMonolithSourceSubsystem::IsIndexingWorkEnabled() const
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	return bAutomaticIndexingEnabled
		&& (!Settings || Settings->bEnableSource);
}

void UMonolithSourceSubsystem::SetAutomaticIndexingEnabled(bool bEnabled)
{
	check(IsInGameThread());

	if (!bEnabled)
	{
		bAutomaticIndexingEnabled = false;
		if (ReloadCompleteHandle.IsValid())
		{
			FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
			ReloadCompleteHandle.Reset();
			UE_LOG(LogMonolithSource, Log,
				TEXT("Source indexing hot-reload hook unregistered"));
		}
		return;
	}

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if ((Settings && !Settings->bEnableSource)
		|| !UMonolithSettings::IsIndexingActivationEnabled())
	{
		bAutomaticIndexingEnabled = false;
		UE_LOG(LogMonolithSource, Log,
			TEXT("MonolithSource: indexing activation or project policy is off; existing EngineSource.db remains available for reads"));
		return;
	}

	bAutomaticIndexingEnabled = true;
	if (!ReloadCompleteHandle.IsValid())
	{
		ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddUObject(
			this, &UMonolithSourceSubsystem::OnReloadComplete);
		UE_LOG(LogMonolithSource, Log,
			TEXT("Source indexing hot-reload hook registered"));
	}
}

bool UMonolithSourceSubsystem::StartPreferredIndex()
{
	check(IsInGameThread());

	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithSource, Warning,
			TEXT("Source indexing is disabled. Run Monolith.StartIndexing to enable source and asset indexing persistently."));
		return false;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Log,
			TEXT("Preferred source index request accepted: an index run is already in progress"));
		return true;
	}

	const FString DbPath = GetDatabasePath();
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*DbPath))
	{
		return TriggerProjectReindexInternal();
	}
	return TriggerReindexInternal();
}

// ============================================================
// F17: Hot-reload auto-reindex hook
// ============================================================

void UMonolithSourceSubsystem::OnReloadComplete(EReloadCompleteReason Reason)
{
	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: indexing activation is off — skipping auto-kick"));
		return;
	}

	// Idempotency guard #1: a reindex is already running. UBT can fire multiple
	// ReloadCompleteDelegate signals (one per loaded module) in quick succession —
	// without this, every additional fire would log a "Indexing already in progress"
	// warning from TriggerProjectReindex(). Cheap early-out.
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: indexing already in progress — skipping auto-kick"));
		return;
	}

	// Idempotency guard #2: cooldown. Even if indexing isn't currently running, a
	// freshly-completed reindex within the last 5s almost certainly already covers
	// the symbols this signal is reporting. 5s is comfortably longer than the
	// typical multi-module reload burst (~50–200 ms) but short enough that a real
	// second-edit kicks promptly.
	const double Now = FPlatformTime::Seconds();
	const double CooldownSeconds = 5.0;
	if (LastReindexTimeSeconds > 0.0 && (Now - LastReindexTimeSeconds) < CooldownSeconds)
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: cooldown active (%.2fs since last) — skipping auto-kick"),
			Now - LastReindexTimeSeconds);
		return;
	}

	// Sanity guard: the project DB must exist (TriggerProjectReindex is incremental and
	// errors out if the engine symbols aren't already in place). On the very first run
	// after install there is no DB — fall through silently and let the user run
	// `source.trigger_reindex` once to bootstrap. Don't surface a noisy warning here.
	const FString DbPath = GetDatabasePath();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: EngineSource.db not present — bootstrap first via source.trigger_reindex"));
		return;
	}

	UE_LOG(LogMonolithSource, Log,
		TEXT("[F17] Hot-reload detected — kicking incremental project reindex (auto)"));

	LastReindexTimeSeconds = Now;
	TriggerProjectReindex(); // Already async via Indexer->StartAsync(); returns immediately.
}

// ============================================================
// Full reindex: engine + shaders + project (clean build)
// ============================================================

void UMonolithSourceSubsystem::TriggerReindex()
{
	TriggerReindexInternal();
}

bool UMonolithSourceSubsystem::TriggerReindexInternal()
{
	check(IsInGameThread());
	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithSource, Warning,
			TEXT("Full source indexing is disabled; run Monolith.StartIndexing first"));
		return false;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexing already in progress"));
		return false;
	}

	FString DbPath = GetDatabasePath();

	// Ensure Saved dir exists
	FString SavedDir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*SavedDir))
	{
		PlatformFile.CreateDirectoryTree(*SavedDir);
	}

	// Close DB during reindex
	if (Database.IsValid() && Database->IsOpen())
	{
		Database->Close();
	}

	bIsIndexing = true;

	delete Indexer;
	Indexer = new FMonolithSourceIndexer();
	Indexer->SetSourcePath(GetEngineSourcePath());
	Indexer->SetShaderPath(GetEngineShaderPath());
	Indexer->SetProjectPath(GetProjectPath());
	Indexer->SetDatabasePath(DbPath);
	Indexer->SetCleanBuild(true);
	Indexer->SetIndexProjectSource(true);

	const TWeakObjectPtr<UMonolithSourceSubsystem> WeakThis(this);
	Indexer->OnComplete.AddLambda([WeakThis, DbPath](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, DbPath, Files, Symbols, Errors, bSucceeded]()
		{
			if (UMonolithSourceSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->FinishIndexingOnGameThread(
					DbPath, TEXT("Full source indexing"), Files, Symbols, Errors, bSucceeded,
					/*bRequiresFullCrgRebuild=*/true);
			}
		});
	});

	UE_LOG(LogMonolithSource, Log, TEXT("Starting full source indexing (engine + project) via C++ indexer"));
	if (!Indexer->StartAsync())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to start full source indexing thread"));
		if (!bDatabaseRequiresSuccessfulReindex)
		{
			ReopenDatabase(DbPath);
		}
		bIsIndexing = false;
		return false;
	}
	return true;
}

// ============================================================
// Incremental project-only reindex
// ============================================================

void UMonolithSourceSubsystem::TriggerProjectReindex()
{
	TriggerProjectReindexInternal();
}

bool UMonolithSourceSubsystem::TriggerProjectReindexInternal()
{
	check(IsInGameThread());
	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithSource, Warning,
			TEXT("Project source indexing is disabled; run Monolith.StartIndexing first"));
		return false;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexing already in progress"));
		return false;
	}

	FString DbPath = GetDatabasePath();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("EngineSource.db not found at %s — run full TriggerReindex() first"), *DbPath);
		return false;
	}

	// Close DB during reindex
	if (Database.IsValid() && Database->IsOpen())
	{
		Database->Close();
	}

	bIsIndexing = true;

	delete Indexer;
	Indexer = new FMonolithSourceIndexer();
	// No engine source path — project only
	Indexer->SetProjectPath(GetProjectPath());
	Indexer->SetDatabasePath(DbPath);
	Indexer->SetCleanBuild(false);   // Incremental — keep existing engine symbols
	Indexer->SetIndexProjectSource(true);

	const TWeakObjectPtr<UMonolithSourceSubsystem> WeakThis(this);
	Indexer->OnComplete.AddLambda([WeakThis, DbPath](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, DbPath, Files, Symbols, Errors, bSucceeded]()
		{
			if (UMonolithSourceSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->FinishIndexingOnGameThread(
					DbPath, TEXT("Project source indexing"), Files, Symbols, Errors, bSucceeded,
					/*bRequiresFullCrgRebuild=*/false);
			}
		});
	});

	UE_LOG(LogMonolithSource, Log, TEXT("Starting project source indexing (incremental) via C++ indexer"));
	if (!Indexer->StartAsync())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to start project source indexing thread"));
		if (!bDatabaseRequiresSuccessfulReindex)
		{
			ReopenDatabase(DbPath);
		}
		bIsIndexing = false;
		return false;
	}
	return true;
}

// ============================================================
// Helpers
// ============================================================

bool UMonolithSourceSubsystem::EnsureDatabaseOpen()
{
	if (IsRunningCommandlet())
	{
		return false;
	}

	if (bIsDeinitializing || bIsIndexing || bDatabaseRequiresSuccessfulReindex)
	{
		return false;
	}

	if (Database.IsValid() && Database->IsOpen())
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();
	if (LastDatabaseOpenFailureTimeSeconds > 0.0 &&
		(Now - LastDatabaseOpenFailureTimeSeconds) < SourceDbOpenFailureCooldownSeconds)
	{
		return false;
	}

	return TryOpenDatabaseWithRetry(GetDatabasePath(), TEXT("Lazy source DB reopen"));
}

void UMonolithSourceSubsystem::FinishIndexingOnGameThread(
	const FString& DbPath,
	const FString& Context,
	int32 Files,
	int32 Symbols,
	int32 Errors,
	bool bSucceeded,
	bool bRequiresFullCrgRebuild)
{
	if (bIsDeinitializing)
	{
		return;
	}

	if (!bSucceeded)
	{
		bDatabaseRequiresSuccessfulReindex = true;
		UE_LOG(LogMonolithSource, Error,
			TEXT("%s failed: %d files, %d symbols, %d errors. EngineSource DB remains closed until a successful reindex."),
			*Context, Files, Symbols, Errors);
		bIsIndexing = false;
		return;
	}

	ReopenDatabase(DbPath);
	if (!Database.IsValid() || !Database->IsOpen())
	{
		bDatabaseRequiresSuccessfulReindex = true;
		UE_LOG(LogMonolithSource, Error,
			TEXT("%s completed indexing but EngineSource DB could not be reopened; a successful reindex is required."),
			*Context);
		bIsIndexing = false;
		return;
	}

	if (bRequiresFullCrgRebuild)
	{
		RebuildSourceCrgCacheAfterIndexing(Database.Get(), *Context);
	}
	else
	{
		UE_LOG(LogMonolithSource, Log,
			TEXT("%s retained the indexer-owned scoped source CRG projection/cache refresh"),
			*Context);
	}
	bDatabaseRequiresSuccessfulReindex = false;
	UE_LOG(LogMonolithSource, Log, TEXT("%s complete: %d files, %d symbols, %d errors"),
		*Context, Files, Symbols, Errors);
	bIsIndexing = false;
}

bool UMonolithSourceSubsystem::TryOpenDatabaseWithRetry(const FString& DbPath, const TCHAR* Context)
{
	if (!Database.IsValid())
	{
		Database = MakeUnique<FMonolithSourceDatabase>();
	}

	if (Database->IsOpen())
	{
		return true;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB not found at %s — run source.trigger_reindex to create it"), *DbPath);
		LastDatabaseOpenFailureTimeSeconds = FPlatformTime::Seconds();
		return false;
	}

	for (int32 Attempt = 1; Attempt <= SourceDbOpenRetryAttempts; ++Attempt)
	{
		if (Database->Open(DbPath))
		{
			LastDatabaseOpenFailureTimeSeconds = 0.0;
			UE_LOG(LogMonolithSource, Log, TEXT("%s: Engine source DB opened from %s after %d attempt(s)"), Context, *DbPath, Attempt);
			return true;
		}

		if (Attempt < SourceDbOpenRetryAttempts)
		{
			FPlatformProcess::Sleep(SourceDbOpenRetryDelaySeconds * Attempt);
		}
	}

	LastDatabaseOpenFailureTimeSeconds = FPlatformTime::Seconds();
	UE_LOG(LogMonolithSource, Warning, TEXT("%s: failed to open EngineSource.db after %d attempt(s): %s"), Context, SourceDbOpenRetryAttempts, *DbPath);
	return false;
}

void UMonolithSourceSubsystem::ReopenDatabase(const FString& DbPath)
{
	TryOpenDatabaseWithRetry(DbPath, TEXT("Reopen source DB"));
}

FString UMonolithSourceSubsystem::GetDatabasePath() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->EngineSourceDBPathOverride.Path.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(Settings->EngineSourceDBPathOverride.Path / TEXT("EngineSource.db"));
	}

	// Use the actual plugin directory so the DB lands next to the plugin regardless
	// of where it is installed.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Saved") / TEXT("EngineSource.db"));
	}

	// Fallback — should not be reached when running inside the plugin itself
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("EngineSource.db"));
}

FString UMonolithSourceSubsystem::GetEngineSourcePath() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->EngineSourcePath.Path.IsEmpty())
	{
		return Settings->EngineSourcePath.Path;
	}
	return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source"));
}

FString UMonolithSourceSubsystem::GetEngineShaderPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Shaders"));
}

FString UMonolithSourceSubsystem::GetProjectPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}
