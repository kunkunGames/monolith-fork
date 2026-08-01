#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.generated.h"

class FMonolithSourceIndexer;
// UE 5.7: declared without underlying type at UObject/UObjectGlobals.h:3216 — match exactly.
enum class EReloadCompleteReason;

struct MONOLITHSOURCE_API FMonolithSourceDatabaseStatus
{
	FString State = TEXT("unavailable");
	FString DatabasePath;
	bool bDatabaseExists = false;
	bool bDatabaseOpen = false;
	bool bIndexing = false;
	bool bRequiresSuccessfulReindex = false;
	FString LastIndexContext;
	FString LastFailureStage;
	FString LastFailureDetail;
	int32 LastFilesProcessed = 0;
	int32 LastSymbolsExtracted = 0;
	int32 LastErrors = 0;
};

/**
 * Editor subsystem that owns the engine source DB and triggers C++ source indexing.
 */
UCLASS()
class MONOLITHSOURCE_API UMonolithSourceSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual ~UMonolithSourceSubsystem();
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Get the open source database, or nullptr while indexing/after a failed reindex. */
	FMonolithSourceDatabase* GetDatabase();

	/** Full reindex: engine + shaders + project source (clean build). */
	bool TriggerReindex();

	/** Incremental project-only reindex: loads existing engine symbols, indexes only project C++ source. */
	bool TriggerProjectReindex();

	/**
	 * Start project-only indexing when the DB exists. A missing DB is
	 * bootstrapped only for an explicit request.
	 */
	bool StartPreferredIndex(bool bAllowFullBootstrap);

	/** Enable/disable hot-reload auto-index hooks without closing the read database. */
	void SetAutomaticIndexingEnabled(bool bEnabled);

	/** Is indexing currently running? */
	bool IsIndexing() const { return bIsIndexing; }

	/** Whether the current process-local writer can accept a new request. */
	bool CanAcceptIndexRequest() const;

	/** Absolute path of the authoritative EngineSource database, including any configured override. */
	FString GetDatabasePath() const;

	/** Fail-closed database/index state used by source actions for structured recovery errors. */
	FMonolithSourceDatabaseStatus GetDatabaseStatus() const;

private:
	FString GetEngineSourcePath() const;
	FString GetEngineShaderPath() const;
	FString GetProjectPath() const;
	bool EnsureDatabaseOpen();
	bool IsIndexingWorkEnabled() const;
	bool TriggerReindexInternal();
	bool TriggerProjectReindexInternal();
	bool TryOpenDatabaseWithRetry(const FString& DbPath, const TCHAR* Context);
	void ReopenDatabase(const FString& DbPath);
	void FinishIndexingOnGameThread(const FString& DbPath, const FString& Context,
		int32 Files, int32 Symbols, int32 Errors, bool bSucceeded,
		const FString& FailureStage, const FString& FailureDetail,
		bool bRequiresFullCrgRebuild);

	/**
	 * F17 (2026-04-26): Auto-reindex hook. Fires when Live Coding / hot-reload completes.
	 * Kicks an incremental project-only reindex so newly-shipped C++ symbols become
	 * queryable via source_query without requiring a manual `source.trigger_project_reindex` call.
	 *
	 * Cooldown-guarded (LastReindexTimeSeconds + 5s) and idempotency-guarded
	 * (bIsIndexing) to prevent storming when UBT fires multiple reload signals back-to-back.
	 */
	void OnReloadComplete(EReloadCompleteReason Reason);

	TUniquePtr<FMonolithSourceDatabase> Database;
	FMonolithSourceIndexer* Indexer = nullptr;
	TAtomic<bool> bIsIndexing{false};
	TAtomic<bool> bIsDeinitializing{false};
	TAtomic<bool> bDatabaseRequiresSuccessfulReindex{false};
	bool bAutomaticIndexingEnabled = false;

	/** F17: Handle into FCoreUObjectDelegates::ReloadCompleteDelegate; cleared on Deinitialize. */
	FDelegateHandle ReloadCompleteHandle;

	/** F17: FPlatformTime::Seconds() at last successful auto-kick — used for the 5s cooldown. */
	double LastReindexTimeSeconds = 0.0;

	/** Last failed lazy DB open attempt; throttles repeated source_query failures. */
	double LastDatabaseOpenFailureTimeSeconds = 0.0;
	FString LastDatabaseOpenFailureDetail;

	FString LastIndexContext;
	FString LastIndexFailureStage;
	FString LastIndexFailureDetail;
	int32 LastIndexFilesProcessed = 0;
	int32 LastIndexSymbolsExtracted = 0;
	int32 LastIndexErrors = 0;
};
