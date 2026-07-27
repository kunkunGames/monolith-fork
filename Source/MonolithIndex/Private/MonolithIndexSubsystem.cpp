#include "MonolithIndexSubsystem.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexReview.h"
#include "MonolithSQLiteMaintenance.h"
#include "MonolithSQLiteSearchText.h"
#include "Utility/MonolithSearchValueWriter.h"
#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
#include "MonolithCompilerSafeDispatch.h"
#include "Misc/AsyncTaskNotification.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/RunnableThread.h"
#include "IO/IoHash.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"

// Indexers
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/WidgetBlueprintIndexer.h"
#include "Indexers/MaterialIndexer.h"
#include "Indexers/GenericAssetIndexer.h"
#include "Indexers/Paper2DIndexer.h"
#include "Indexers/PaperZDIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/LevelIndexer.h"
#include "Indexers/ConfigIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/CppIndexer.h"
#include "Indexers/AnimationIndexer.h"
#include "Indexers/NiagaraIndexer.h"
#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/UserDefinedStructIndexer.h"
#include "Indexers/InputActionIndexer.h"
#include "Indexers/DataAssetIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/MetaSoundIndexer.h"
#include "Indexers/DomainAssetIndexer.h"

// ============================================================
// Incremental-reachability GC override (RAII)
// ============================================================
// UE 5.7's INCREMENTAL reachability GC (gc.AllowIncrementalReachability=1,
// gc.IncrementalReachabilityTimeLimit=0.002 by editor default) leaks GC
// worker-context bits from the process-global GWorkerIndices bitmask: when an
// incremental pass hits its 2ms budget it SUSPENDS and retains its worker
// contexts across frames; ReleaseAsyncProcessingContexts only frees them when
// no pass is suspended (GarbageCollection.cpp:7310-7313). The deep-index run
// drives GC continuously (forced collects per batch + per-asset GetAsset() ->
// LoadPackage -> FlushAsyncLoading), so suspended passes accumulate and exhaust
// the 64-slot pool -> "Exceeded max active GC worker contexts" assert.
//
// Forcing gc.AllowIncrementalReachability=0 for the run's duration makes every
// GC a BLOCKING collection, which always runs ReleaseAsyncProcessingContexts to
// completion in a single call -> worker bits are always freed -> the leak is
// structurally impossible. The original value is captured at run start and
// restored on the dtor (game thread), covering normal completion, error, and
// abort/cancel because all of those converge on OnIndexingFinished() (and the
// editor-shutdown-mid-index path resets it in Deinitialize()).
//
// File-static + single-flight (bIsIndexing) means at most one override is ever
// live, so a file-static TUniquePtr is a safe owner and keeps the fix .cpp-only.
namespace
{
	class FIncrementalReachabilityGCOverride
	{
	public:
		FIncrementalReachabilityGCOverride()
		{
			CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("gc.AllowIncrementalReachability"));
			if (CVar)
			{
				OriginalValue = CVar->GetInt();
				CVar->Set(0, ECVF_SetByCode);
				UE_LOG(LogMonolithIndex, Log,
					TEXT("Deep index: forced gc.AllowIncrementalReachability=0 (was %d) to prevent GC worker-context leak"),
					OriginalValue);
			}
			else
			{
				UE_LOG(LogMonolithIndex, Warning,
					TEXT("Deep index: gc.AllowIncrementalReachability CVar not found — cannot disable incremental reachability GC"));
			}
		}

		~FIncrementalReachabilityGCOverride()
		{
			if (CVar)
			{
				CVar->Set(OriginalValue, ECVF_SetByCode);
				UE_LOG(LogMonolithIndex, Log,
					TEXT("Deep index: restored gc.AllowIncrementalReachability=%d"), OriginalValue);
			}
		}

	private:
		IConsoleVariable* CVar = nullptr;
		int32 OriginalValue = 1;
	};

	// At most one full-index run is live at a time (guarded by bIsIndexing), so a
	// single file-static owner is sufficient. Reset on the game thread only.
	static TUniquePtr<FIncrementalReachabilityGCOverride> GIncrementalGCOverride;

	static void RefreshProjectCrgCacheForChangedAssets(
		FMonolithIndexDatabase* Database,
		const TSet<FString>& ChangedAssetPaths,
		const TCHAR* Context)
	{
		if (!Database || !Database->IsOpen() || ChangedAssetPaths.Num() == 0)
		{
			return;
		}

		TSharedPtr<FJsonObject> CrgResult = FMonolithIndexReview::RefreshCrgCacheForAssets(
			*Database,
			ChangedAssetPaths,
			FString(Context));

		FString Status;
		FString Summary;
		FString RefreshMode;
		if (CrgResult.IsValid())
		{
			CrgResult->TryGetStringField(TEXT("status"), Status);
			CrgResult->TryGetStringField(TEXT("summary"), Summary);
			CrgResult->TryGetStringField(TEXT("refresh_mode"), RefreshMode);
		}

		if (Status == TEXT("ok"))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("%s; project CRG scoped refresh (%s): %s"),
				Context, *RefreshMode, *Summary);
		}
		else
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("%s; project CRG scoped refresh did not complete cleanly: %s"),
				Context, *Summary);
		}
	}
}

// Legacy single-database trigger retained for compatibility. It intentionally
// does not mutate durable activation; callers must first use
// Monolith.StartIndexing, which enables both source and asset indexing.
static FAutoConsoleCommand GMonolithStartIndexCommand(
	TEXT("Monolith.StartIndex"),
	TEXT("Legacy alias: starts a full project asset index when Monolith indexing is already enabled."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (!GEditor)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex: GEditor not available — cannot start index"));
			return;
		}

		UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (!Subsystem)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex: MonolithIndex subsystem not available — cannot start index"));
			return;
		}

		UE_LOG(LogMonolithIndex, Log,
			TEXT("Monolith.StartIndex: legacy full asset index requested; prefer Monolith.StartIndexing"));
		Subsystem->StartFullIndex();
	}));

void UMonolithIndexSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Commandlet mode (cook/compile): skip DB open entirely. The running editor holds a WAL lock
	// on ProjectIndex.db and a second open surfaces as "disk I/O error" → UAT ExitCode=1.
	// The commandlet has no consumer of the index anyway.
	if (IsRunningCommandlet())
	{
		return;
	}

	Database = MakeUnique<FMonolithIndexDatabase>();
	FString DbPath = GetDatabasePath();

	if (!Database->Open(DbPath))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database at %s"), *DbPath);
		return;
	}

	RegisterDefaultIndexers();

	// Hard project policy and resolved project-default/per-user activation gate
	// only indexing work.
	// Database initialization remains above these returns so existing DB reads
	// continue to work while indexing is disabled.
	if (!GetDefault<UMonolithSettings>()->bEnableIndex)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("MonolithIndex: indexing disabled via bEnableIndex=false; skipping index run"));
		return;
	}

	const FMonolithActivation Activation = UMonolithSettings::GetActivation();
	if (!Activation.bIndexingEnabled)
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("MonolithIndex: durable indexing activation is off; existing ProjectIndex.db remains available for reads"));
		return;
	}

	SetAutomaticIndexingEnabled(true);
	StartPreferredIndex(Activation.bIndexingUserSet);
}

void UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded()
{
	// Unbind ourselves — this is a one-shot callback
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnFilesLoaded().RemoveAll(this);

	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("Asset Registry loaded after indexing was disabled; queued index request discarded"));
		return;
	}

	if (ShouldAutoIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry fully loaded -- starting full project index"));
		StartFullIndexInternal(FString());
	}
	else if (CanDoIncrementalIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry fully loaded -- starting incremental project index"));
		StartIncrementalIndexInternal(FString());
	}
	else
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry fully loaded -- incompatible index requires a full rebuild"));
		StartFullIndexInternal(FString());
	}
}

void UMonolithIndexSubsystem::Deinitialize()
{
	bAutomaticIndexingEnabled = false;
	UnregisterLiveCallbacks();

	// Unbind from Asset Registry delegate if still bound
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
	}

	// Stop any running indexing
	if (IndexingTaskPtr.IsValid())
	{
		if (bIsIndexing)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing was still in progress during shutdown — force-stopped"));
		}
		IndexingTaskPtr->Stop();
		if (IndexingThread)
		{
			IndexingThread->WaitForCompletion();
			IndexingThread.Reset();
		}
		IndexingTaskPtr.Reset();
	}

	FinishActiveAsyncJob(false);
	bIsIndexing = false;

	// Restore GC setting if the editor is shutting down mid-index (this abort
	// path force-stops the worker without routing through OnIndexingFinished).
	GIncrementalGCOverride.Reset();

	TaskNotification.Reset();

	if (Database.IsValid())
	{
		Database->Close();
	}

	Super::Deinitialize();
}

void UMonolithIndexSubsystem::RegisterIndexer(TSharedPtr<IMonolithIndexer> Indexer)
{
	if (!Indexer.IsValid()) return;

	Indexers.Add(Indexer);
	for (const FString& ClassName : Indexer->GetSupportedClasses())
	{
		ClassToIndexer.Add(ClassName, Indexer);
	}

	UE_LOG(LogMonolithIndex, Verbose, TEXT("Registered indexer: %s (%d classes)"),
		*Indexer->GetName(), Indexer->GetSupportedClasses().Num());
}

TSharedPtr<IMonolithIndexer> UMonolithIndexSubsystem::ResolveDeepIndexer(
	const FString& LeafClassName, const FTopLevelAssetPath& ClassPath, IAssetRegistry* AssetRegistry) const
{
	// 1) Exact leaf-class-name match — unchanged dispatch behavior.
	if (const TSharedPtr<IMonolithIndexer>* Exact = ClassToIndexer.Find(LeafClassName))
	{
		if (Exact->IsValid())
		{
			return *Exact;
		}
	}

	// 2) Inheritance parent-walk fallback. Runs ONLY on an exact miss, so every exact hit is
	//    byte-for-byte unchanged. GetAncestorClassNames returns ancestors most-derived-first
	//    from the AssetRegistry inheritance tree WITHOUT loading any UClass; we take the first
	//    ancestor whose indexer explicitly opts into derived-class dispatch. This routes the
	//    UGo*DataAsset : UPrimaryDataAsset family to FDataAssetIndexer while preventing narrower
	//    exact-class contracts (for example BlueprintIndexer) from deep-loading unrelated
	//    subclasses such as ControlRigBlueprint.
	if (AssetRegistry && ClassPath.IsValid())
	{
		TArray<FTopLevelAssetPath> Ancestors;
		if (AssetRegistry->GetAncestorClassNames(ClassPath, Ancestors))
		{
			for (const FTopLevelAssetPath& Ancestor : Ancestors)
			{
				const FString AncestorLeaf = Ancestor.GetAssetName().ToString();
				if (const TSharedPtr<IMonolithIndexer>* Found = ClassToIndexer.Find(AncestorLeaf))
				{
					if (Found->IsValid()
						&& !(*Found)->IsSentinel()
						&& (*Found)->SupportsDerivedClassDispatch())
					{
						return *Found;
					}
				}
			}
		}
	}

	return nullptr;
}

FString UMonolithIndexSubsystem::ComputeIndexerFleetSignature() const
{
	// Fold every registered indexer's name + version into an order-stable string (sorted, so the
	// signature is independent of registration order). Bumping any IMonolithIndexer::GetIndexerVersion()
	// changes this; CanDoIncrementalIndex() then refuses incremental and forces a full reindex.
	TArray<FString> Parts;
	Parts.Reserve(Indexers.Num());
	for (const TSharedPtr<IMonolithIndexer>& Indexer : Indexers)
	{
		if (Indexer.IsValid())
		{
			Parts.Add(FString::Printf(
				TEXT("%s:%d:D%d"),
				*Indexer->GetName(),
				Indexer->GetIndexerVersion(),
				Indexer->SupportsDerivedClassDispatch() ? 1 : 0));
		}
	}
	Parts.Sort();
	return FString::Join(Parts, TEXT("|"));
}

void UMonolithIndexSubsystem::RegisterDefaultIndexers()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();

	if (Settings->bIndexBlueprints)
	{
		RegisterIndexer(MakeShared<FBlueprintIndexer>());
		// C2 (PRD AssetSearchSemanticSearch): WidgetBlueprint assets are routed to the
		// UMG-aware indexer instead of the generic graph-only FBlueprintIndexer so widget
		// tree labels/classes + FDelegateRuntimeBinding rows are indexed.
		RegisterIndexer(MakeShared<FWidgetBlueprintIndexer>());
	}
	if (Settings->bIndexMaterials)
		RegisterIndexer(MakeShared<FMaterialIndexer>());
	if (Settings->bIndexGenericAssets)
	{
		RegisterIndexer(MakeShared<FGenericAssetIndexer>());
		// PRD AssetSearchSemanticSearch UE5.8 #4: Paper2D (Flipbook/Sprite) — these fell to the
		// shallow generic indexer; the dedicated indexer adds the flipbook frame graph + sprite
		// atlas/material edges (Paper2D is ~70% of this project's assets).
		RegisterIndexer(MakeShared<FPaper2DIndexer>());
#if WITH_PAPERZD
		// PaperZD 2D-animation assets (UPaperZDAnimSequence_Flipbook ~1665, UPaperZDAnimBP ~114)
		// otherwise fall to name-only indexing; the dedicated indexer adds the animation summary
		// (frames/fps/duration/category/source/notifies) and the AnimBP state-machine/source edges.
		RegisterIndexer(MakeShared<FPaperZDIndexer>());
#endif
	}
	if (Settings->bIndexDependencies)
		RegisterIndexer(MakeShared<FDependencyIndexer>());
	if (Settings->bIndexLevels)
		RegisterIndexer(MakeShared<FLevelIndexer>());
	if (Settings->bIndexDataTables)
		RegisterIndexer(MakeShared<FDataTableIndexer>());
	if (Settings->bIndexGameplayTags)
		RegisterIndexer(MakeShared<FGameplayTagIndexer>());
	if (Settings->bIndexConfigs)
		RegisterIndexer(MakeShared<FConfigIndexer>());
	if (Settings->bIndexCppSymbols)
		RegisterIndexer(MakeShared<FCppIndexer>());
	if (Settings->bIndexAnimations)
		RegisterIndexer(MakeShared<FAnimationIndexer>());
	if (Settings->bIndexNiagara)
		RegisterIndexer(MakeShared<FNiagaraIndexer>());
	if (Settings->bIndexUserDefinedEnums)
		RegisterIndexer(MakeShared<FUserDefinedEnumIndexer>());
	if (Settings->bIndexUserDefinedStructs)
		RegisterIndexer(MakeShared<FUserDefinedStructIndexer>());
	if (Settings->bIndexInputActions)
		RegisterIndexer(MakeShared<FInputActionIndexer>());
	if (Settings->bIndexDataAssets)
		RegisterIndexer(MakeShared<FDataAssetIndexer>());
	if (Settings->bIndexMeshCatalog)
		RegisterIndexer(MakeShared<FMeshCatalogIndexer>());
	if (Settings->bIndexGAS)
		RegisterIndexer(MakeShared<FGASIndexer>());
	if (Settings->bIndexDomainAssets)
		RegisterIndexer(MakeShared<FDomainAssetIndexer>());
#if WITH_METASOUND
	if (Settings->bIndexMetaSounds)
		RegisterIndexer(MakeShared<FMetaSoundIndexer>());
#endif

	UE_LOG(LogMonolithIndex, Log, TEXT("Registered %d indexers"), Indexers.Num());
}

bool UMonolithIndexSubsystem::StartPreferredIndex(bool bExplicitRequest)
{
	check(IsInGameThread());

	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Project indexing is disabled. Run Monolith.StartIndexing to enable source and asset indexing persistently."));
		return false;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("Preferred project index request accepted: an index run is already in progress"));
		return true;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Cannot start preferred project index because ProjectIndex.db is not open"));
		return false;
	}

	const bool bFirstTimeIndex = ShouldAutoIndex();
	const bool bNeedsFullIndex = bFirstTimeIndex || !CanDoIncrementalIndex();
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (bFirstTimeIndex
		&& !bExplicitRequest
		&& Settings
		&& Settings->bDeferFirstTimeIndex)
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("MonolithIndex: first-time index deferred via bDeferFirstTimeIndex; run Monolith.StartIndexing or Monolith.StartIndex to begin"));
		return false;
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
		AssetRegistry.OnFilesLoaded().AddUObject(
			this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded);
		UE_LOG(LogMonolithIndex, Log,
			TEXT("Project index activation queued until the Asset Registry finishes loading"));
		return true;
	}

	if (bNeedsFullIndex)
	{
		return StartFullIndexInternal(FString());
	}
	return StartIncrementalIndexInternal(FString());
}

void UMonolithIndexSubsystem::SetAutomaticIndexingEnabled(bool bEnabled)
{
	check(IsInGameThread());

	if (!bEnabled)
	{
		bAutomaticIndexingEnabled = false;
		if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
		{
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
			AssetRegistry.OnFilesLoaded().RemoveAll(this);
		}
		UnregisterLiveCallbacks();
		PendingChanges.Reset();

		if (bIsIndexing)
		{
			UE_LOG(LogMonolithIndex, Log,
				TEXT("Project indexing deactivated; automatic hooks stopped and the active run will drain safely"));
		}
		else
		{
			UE_LOG(LogMonolithIndex, Log,
				TEXT("Project indexing deactivated; automatic hooks stopped"));
		}
		return;
	}

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if ((Settings && !Settings->bEnableIndex)
		|| !UMonolithSettings::IsIndexingActivated())
	{
		bAutomaticIndexingEnabled = false;
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Project indexing automatic hooks were not enabled because activation or project policy is off"));
		return;
	}

	bAutomaticIndexingEnabled = true;

	// A preferred catch-up run owns callback registration. Full/incremental
	// completion converges on RegisterLiveCallbacks(), which re-checks activation
	// so a StopIndexing command during the run cannot re-arm the hooks.
}

bool UMonolithIndexSubsystem::StartFullIndex()
{
	return StartFullIndexInternal(FString());
}

bool UMonolithIndexSubsystem::StartFullIndexWithAsyncJob(const FString& JobId)
{
	return StartFullIndexInternal(JobId);
}

bool UMonolithIndexSubsystem::StartIncrementalIndexWithAsyncJob(const FString& JobId)
{
	return StartIncrementalIndexInternal(JobId);
}

bool UMonolithIndexSubsystem::StartFullIndexInternal(const FString& JobId)
{
	check(IsInGameThread());
	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Full project indexing is disabled; run Monolith.StartIndexing first"));
		FailSubmittedAsyncJob(
			JobId,
			TEXT("Monolith indexing is disabled. Run Monolith.StartIndexing in the editor console."));
		return false;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing already in progress"));
		FailSubmittedAsyncJob(JobId, TEXT("Project indexing is already in progress."));
		return false;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Cannot start full index because ProjectIndex.db is not open"));
		FailSubmittedAsyncJob(JobId, TEXT("Project index database is not open."));
		return false;
	}

	bIsIndexing = true;
	BeginActiveAsyncJob(JobId, TEXT("full"), TEXT("Full re-index starting."));
	UnregisterLiveCallbacks();
	PendingChanges.Reset();

	// Force blocking (non-incremental) reachability GC for the whole run so the
	// engine cannot leak GC worker-context bits across suspended incremental
	// passes. Restored in OnIndexingFinished() (all worker exit paths) and
	// defensively in Deinitialize() (editor shutdown mid-index). Game thread.
	GIncrementalGCOverride = MakeUnique<FIncrementalReachabilityGCOverride>();

	// Reset the database for a full re-index
	Database->ResetDatabase();

	// Gather marketplace plugin paths for indexing
	IndexedPlugins = GatherMarketplacePluginPaths();

	// Show notification
	FAsyncTaskNotificationConfig NotifConfig;
	NotifConfig.TitleText = FText::FromString(TEXT("Monolith"));
	NotifConfig.ProgressText = FText::FromString(TEXT("Indexing project..."));
	NotifConfig.bCanCancel = true;
	NotifConfig.LogCategory = &LogMonolithIndex;
	TaskNotification = MakeUnique<FAsyncTaskNotification>(NotifConfig);

	// Launch background thread
	IndexingTaskPtr = MakeUnique<FIndexingTask>(this);
	IndexingTaskPtr->PluginsToIndex = IndexedPlugins;
	IndexingTaskPtr->AsyncJobId = JobId;
	IndexingThread.Reset(FRunnableThread::Create(
		IndexingTaskPtr.Get(),
		TEXT("MonolithIndexing"),
		0,
		TPri_BelowNormal
	));
	if (!IndexingThread)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create MonolithIndexing worker thread"));
		bIsIndexing = false;
		GIncrementalGCOverride.Reset();
		IndexingTaskPtr.Reset();
		if (TaskNotification)
		{
			TaskNotification->SetComplete(
				FText::FromString(TEXT("Monolith")),
				FText::FromString(TEXT("Project indexing failed to start")),
				false);
			TaskNotification.Reset();
		}
		FinishActiveAsyncJob(false);
		return false;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Background indexing started"));
	return true;
}

void UMonolithIndexSubsystem::BeginActiveAsyncJob(const FString& JobId, const FString& IndexMode, const FString& Message)
{
	ActiveAsyncJobId = JobId;
	ActiveAsyncJobMode = IndexMode;
	if (!ActiveAsyncJobId.IsEmpty())
	{
		FMonolithAsyncJobRegistry::Get().UpdateProgress(ActiveAsyncJobId, 0.0, TEXT("starting"), Message);
	}
}

bool UMonolithIndexSubsystem::IsActiveAsyncJobCancellationRequested() const
{
	return !ActiveAsyncJobId.IsEmpty() && FMonolithAsyncJobRegistry::Get().IsCancelRequested(ActiveAsyncJobId);
}

void UMonolithIndexSubsystem::UpdateActiveAsyncJobProgress(double Percent, const FString& Stage, const FString& Message)
{
	if (!ActiveAsyncJobId.IsEmpty())
	{
		FMonolithAsyncJobRegistry::Get().UpdateProgress(ActiveAsyncJobId, Percent, Stage, Message);
	}
}

void UMonolithIndexSubsystem::UpdateActiveAsyncJobProgress(int32 Current, int32 Total, const FString& Stage)
{
	const double Percent = Total > 0
		? FMath::Clamp((static_cast<double>(Current) / static_cast<double>(Total)) * 100.0, 0.0, 99.0)
		: 0.0;
	UpdateActiveAsyncJobProgress(
		Percent,
		Stage,
		FString::Printf(TEXT("Indexed %d / %d project index units."), Current, Total));
}

void UMonolithIndexSubsystem::FinishActiveAsyncJob(bool bSuccess)
{
	const FString JobId = ActiveAsyncJobId;
	const FString IndexMode = ActiveAsyncJobMode;
	ActiveAsyncJobId.Empty();
	ActiveAsyncJobMode.Empty();

	if (JobId.IsEmpty())
	{
		return;
	}

	FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
	if (JobRegistry.IsCancelRequested(JobId))
	{
		JobRegistry.CancelJob(JobId, TEXT("Project index cancellation was acknowledged before completion."));
		return;
	}

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("completed"));
		Result->SetStringField(TEXT("index_mode"), IndexMode.IsEmpty() ? TEXT("unknown") : IndexMode);
		Result->SetStringField(TEXT("message"), TEXT("Project index completed."));
		JobRegistry.CompleteJob(JobId, Result);
	}
	else
	{
		JobRegistry.FailJob(JobId, TEXT("Project index failed or was cancelled before completion."));
	}
}

void UMonolithIndexSubsystem::FailSubmittedAsyncJob(const FString& JobId, const FString& Error) const
{
	if (!JobId.IsEmpty())
	{
		FMonolithAsyncJobRegistry::Get().FailJob(JobId, Error);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void UMonolithIndexSubsystem::SetActiveAsyncJobForTests(const FString& JobId, const FString& IndexMode)
{
	BeginActiveAsyncJob(JobId, IndexMode, TEXT("Test async index job starting."));
}

void UMonolithIndexSubsystem::CompleteActiveAsyncJobForTests(bool bSuccess)
{
	FinishActiveAsyncJob(bSuccess);
}
#endif

float UMonolithIndexSubsystem::GetProgress() const
{
	if (!IndexingTaskPtr.IsValid() || IndexingTaskPtr->TotalAssets == 0) return 0.0f;
	return static_cast<float>(IndexingTaskPtr->CurrentIndex) / static_cast<float>(IndexingTaskPtr->TotalAssets);
}

// ============================================================
// Query API wrappers
// ============================================================

TArray<FSearchResult> UMonolithIndexSubsystem::Search(const FString& Query, int32 Limit)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FullTextSearch(Query, Limit);
}

TArray<FSearchResult> UMonolithIndexSubsystem::Search(const FString& Query, int32 Limit, const FProjectSearchOptions& Options)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FullTextSearch(Query, Limit, Options);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::FindReferences(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->FindReferences(PackagePath);
}

TArray<FIndexedAsset> UMonolithIndexSubsystem::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FindByType(AssetClass, Limit, Offset);
}

TArray<FIndexedAsset> UMonolithIndexSubsystem::FindByType(
	const FString& AssetClass,
	const FString& ModuleFilter,
	int32 Limit,
	int32 Offset)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FindByType(AssetClass, ModuleFilter, Limit, Offset);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetStats()
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->GetStats();
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetAssetDetails(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->GetAssetDetails(PackagePath);
}

TArray<FIndexedPluginInfo> UMonolithIndexSubsystem::GatherMarketplacePluginPaths() const
{
    TArray<FIndexedPluginInfo> Result;

    const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
    if (!Settings->bIndexMarketplacePlugins)
    {
        return Result;
    }

    TArray<TSharedRef<IPlugin>> ContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
    for (const TSharedRef<IPlugin>& Plugin : ContentPlugins)
    {
        // Skip engine plugins — keep project/marketplace plugins that have content directories
        if (Plugin->GetType() == EPluginType::Engine)
        {
            continue;
        }
        FString PluginContentDir = Plugin->GetContentDir();
        if (!FPaths::DirectoryExists(PluginContentDir))
        {
            continue;
        }

        FIndexedPluginInfo Info;
        Info.PluginName = Plugin->GetName();
        Info.MountPath = Plugin->GetMountedAssetPath();
        Info.ContentDir = Plugin->GetContentDir();
        Info.FriendlyName = Plugin->GetDescriptor().FriendlyName;

        UE_LOG(LogMonolithIndex, Log, TEXT("Marketplace plugin found: %s (mount: %s)"),
            *Info.FriendlyName, *Info.MountPath);

        Result.Add(MoveTemp(Info));
    }

    UE_LOG(LogMonolithIndex, Log, TEXT("Found %d marketplace plugins to index"), Result.Num());
    return Result;
}

// ============================================================
// Background indexing task
// ============================================================

UMonolithIndexSubsystem::FIndexingTask::FIndexingTask(UMonolithIndexSubsystem* InOwner)
	: Owner(InOwner)
{
}

uint32 UMonolithIndexSubsystem::FIndexingTask::Run()
{
	const UMonolithSettings* GlobalSettings = GetDefault<UMonolithSettings>();
	const bool bLogMemory = GlobalSettings ? GlobalSettings->bLogMemoryStats : true;

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index starting"));
	}

	// Asset Registry enumeration MUST happen on the game thread
	TArray<FAssetData> AllAssets;
	FEvent* RegistryEvent = FPlatformProcess::GetSynchEventFromPool(true);
	AsyncTask(ENamedThreads::GameThread, [this, &AllAssets, RegistryEvent]()
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		if (!AssetRegistry.IsSearchAllAssets())
		{
			AssetRegistry.SearchAllAssets(true);
		}
		AssetRegistry.WaitForCompletion();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		// Add marketplace plugin mount paths
		for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
		{
			FString CleanPath = PluginInfo.MountPath;
			if (CleanPath.EndsWith(TEXT("/")))
			{
				CleanPath.LeftChopInline(1);
			}
			Filter.PackagePaths.Add(FName(*CleanPath));
		}
		// Add user-configured additional content paths
		{
			const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
			if (Settings)
			{
				for (const FString& CustomPath : Settings->AdditionalContentPaths)
				{
					if (!CustomPath.IsEmpty())
					{
						FString CleanPath = CustomPath;
						if (CleanPath.EndsWith(TEXT("/")))
						{
							CleanPath.LeftChopInline(1);
						}
						Filter.PackagePaths.AddUnique(FName(*CleanPath));
					}
				}
			}
		}
		Filter.bRecursivePaths = true;
		AssetRegistry.GetAssets(Filter, AllAssets);

		RegistryEvent->Trigger();
	});
	RegistryEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(RegistryEvent);

	TotalAssets = AllAssets.Num();
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Scanning %d assets..."), TotalAssets.Load());
	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %d assets..."), TotalAssets.Load());

	FMonolithIndexDatabase* DB = Owner->Database.Get();
	if (!DB || !DB->IsOpen())
	{
		TWeakObjectPtr<UMonolithIndexSubsystem> WeakOwner(Owner);
		AsyncTask(ENamedThreads::GameThread, [WeakOwner]()
		{
			if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
			{
				Subsystem->OnIndexingFinished(false);
			}
		});
		return 1;
	}

	auto IsAsyncJobCancellationRequested = [this]() -> bool
	{
		if (!AsyncJobId.IsEmpty() && FMonolithAsyncJobRegistry::Get().IsCancelRequested(AsyncJobId))
		{
			bShouldStop = true;
			return true;
		}
		return false;
	};

	DB->BeginTransaction();

	int32 BatchSize = 100;
	int32 Indexed = 0;
	int32 Errors = 0;

	// Collect assets that have deep indexers for a second pass
	struct FDeepIndexEntry
	{
		FAssetData AssetData;
		int64 AssetId;
		TSharedPtr<IMonolithIndexer> Indexer;
	};
	TArray<FDeepIndexEntry> DeepIndexQueue;

	TMap<FString, int32> ClassDistribution;
	TMap<FString, int32> QueuedClassDistribution;

	IAssetRegistry* AssetRegistryPtr = IAssetRegistry::Get();

	for (int32 i = 0; i < AllAssets.Num(); ++i)
	{
		if (bShouldStop || IsAsyncJobCancellationRequested()) break;

		if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
		{
			if (!AsyncJobId.IsEmpty())
			{
				FMonolithAsyncJobRegistry::Get().RequestCancel(AsyncJobId);
			}
			bShouldStop = true;
			break;
		}

		const FAssetData& AssetData = AllAssets[i];
		CurrentIndex = i + 1;

		// Insert the base asset record
		FIndexedAsset IndexedAsset;
		IndexedAsset.PackagePath = AssetData.PackageName.ToString();
		IndexedAsset.AssetName = AssetData.AssetName.ToString();
		IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;

		// Determine module name from package path
		if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
			{
				if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
				{
					IndexedAsset.ModuleName = PluginInfo.PluginName;
					break;
				}
			}
		}

		// If not matched to a marketplace plugin, check additional content paths
		if (IndexedAsset.ModuleName.IsEmpty() && !IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (SecondSlash > 1)
			{
				IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
			}
		}

		// Get disk file modification time for incremental change detection
		{
			FString PackageFilename;
			if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFilename))
			{
				FDateTime FileTime = IFileManager::Get().GetTimeStamp(*PackageFilename);
				IndexedAsset.LastModified = FileTime.ToIso8601();
			}
		}

		// Get Blake3 hash for move detection (available from AR without loading the package)
		if (AssetRegistryPtr)
		{
			TOptional<FAssetPackageData> PackageData = AssetRegistryPtr->GetAssetPackageDataCopy(AssetData.PackageName);
			if (PackageData.IsSet())
			{
				FIoHash Hash = PackageData->GetPackageSavedHash();
				IndexedAsset.SavedHash = LexToString(Hash);
			}
		}

		int64 AssetId = DB->InsertAsset(IndexedAsset);
		if (AssetId < 0)
		{
			Errors++;
			continue;
		}

		// Q1 (PRD AssetSearchSemanticSearch): emit a CamelCase/snake sub-word split of the
		// asset name as a supplemental search value so partial-identifier queries recall the
		// asset ("fireball" -> BP_FireballProjectile). The splitter appends sub-tokens to the
		// original, so exact/prefix hits still outrank sub-word hits. Tagged 'identifier_split'
		// for provenance honesty (match_source stays truthful). Runs for EVERY asset (the main
		// loop), independent of whether the asset has a per-type deep indexer.
		{
			const FString NameSplit = BuildMonolithSQLiteSearchText(IndexedAsset.AssetName);
			if (!NameSplit.IsEmpty() && NameSplit != IndexedAsset.AssetName)
			{
				FMonolithSearchValueWriter SplitWriter(*DB);
				SplitWriter.AddValue(AssetId, TEXT("identifier_split"), IndexedAsset.AssetName,
					IndexedAsset.PackagePath, IndexedAsset.AssetClass, TEXT("name_split"),
					TEXT("name_split"), NameSplit, FString());
			}
		}

		// Queue assets that have deep indexers (Blueprint, Material, etc.). Exact leaf-class
		// dispatch first; on a miss the resolver walks the parent class chain (most-derived
		// first) only through indexers that opt into derived-class dispatch - e.g. a
		// project-specific U*DataAsset : UPrimaryDataAsset type reaches FDataAssetIndexer via
		// its "PrimaryDataAsset" ancestor. Distribution stays keyed on the leaf class for
		// honest per-type telemetry (for example, "MonsterDataAsset: 83").
		TSharedPtr<IMonolithIndexer> FoundIndexer = Owner->ResolveDeepIndexer(
			IndexedAsset.AssetClass, AssetData.AssetClassPath, AssetRegistryPtr);
		if (FoundIndexer.IsValid())
		{
			DeepIndexQueue.Add({ AssetData, AssetId, FoundIndexer });
			QueuedClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;
		}

		Indexed++;

		// Commit in batches
		if (Indexed % BatchSize == 0)
		{
			DB->CommitTransaction();
			DB->BeginTransaction();

			UE_LOG(LogMonolithIndex, Log, TEXT("Indexed %d / %d assets (%d errors)"),
				Indexed, TotalAssets.Load(), Errors);

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Indexing %d / %d assets..."), CurrentIndex.Load(), TotalAssets.Load())));
			}

			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				Owner->UpdateActiveAsyncJobProgress(CurrentIndex.Load(), TotalAssets.Load(), TEXT("indexing_assets"));
				Owner->OnProgress.Broadcast(CurrentIndex.Load(), TotalAssets.Load());
			});
		}
	}

	// Log class distribution summary
	UE_LOG(LogMonolithIndex, Log, TEXT("Asset class distribution (top 20):"));
	ClassDistribution.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 Shown = 0;
	for (const auto& Pair : ClassDistribution)
	{
		if (Shown++ >= 20) break;
		UE_LOG(LogMonolithIndex, Log, TEXT("  %s: %d"), *Pair.Key, Pair.Value);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep index queue: %d assets across %d classes"),
		DeepIndexQueue.Num(), QueuedClassDistribution.Num());
	for (const auto& Pair : QueuedClassDistribution)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("  Queued %s: %d"), *Pair.Key, Pair.Value);
	}

	DB->CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("Metadata pass complete: %d assets indexed, %d errors"), Indexed, Errors);

	// ============================================================
	// Deep indexing pass — load assets on game thread in time-budgeted batches
	// Assets must be loaded on the game thread to avoid texture compiler crashes.
	// We process in small batches with GC and memory management to prevent OOM.
	// ============================================================
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Deep indexing %d assets..."), DeepIndexQueue.Num());

	if (!bShouldStop && DeepIndexQueue.Num() > 0)
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		FMonolithMemoryHelper::LogTierStartupOnce();
		const int32 DeepBatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedDeepIndexBatchSize());
		const int32 GCFrequency = FMath::Max(1, Settings->GCFrequencyBatches);
		const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
		const float YieldTime = Settings->YieldTimeSeconds;

		UE_LOG(LogMonolithIndex, Log, TEXT("Starting deep indexing pass for %d assets (batch size: %d, GC every %d batches, memory budget: %llu MB)..."),
			DeepIndexQueue.Num(), DeepBatchSize, GCFrequency, MemoryBudgetMB);

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index start"));
		}

		constexpr double FrameBudgetSeconds = 0.016; // ~16ms per batch to stay interactive
		TAtomic<int32> DeepIndexed{0};
		TAtomic<int32> DeepErrors{0};
		int32 TotalDeep = DeepIndexQueue.Num();
		int32 BatchNumber = 0;

		for (int32 BatchStart = 0; BatchStart < TotalDeep && !bShouldStop; BatchStart += DeepBatchSize)
		{
			if (IsAsyncJobCancellationRequested())
			{
				break;
			}

			// Check for cancellation from notification
			if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
			{
				if (!AsyncJobId.IsEmpty())
				{
					FMonolithAsyncJobRegistry::Get().RequestCancel(AsyncJobId);
				}
				bShouldStop = true;
				break;
			}

			// Memory budget check - throttle if over budget
			if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Memory budget exceeded, forcing GC and yielding..."));
				
				FEvent* GCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [GCEvent, YieldTime]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(true);
					FMonolithMemoryHelper::YieldToEditor();
					if (YieldTime > 0.0f)
					{
						FPlatformProcess::Sleep(YieldTime);
					}
					GCEvent->Trigger();
				});
				GCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(GCEvent);

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(TEXT("After throttle GC"));
				}
			}

			// Check for critical memory situation
			if (FMonolithMemoryHelper::IsMemoryCritical())
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("Critical memory situation detected (<2GB available). Pausing indexing..."));
				
				FEvent* CriticalGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [CriticalGCEvent]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(true);
					FPlatformProcess::Sleep(1.0f); // Longer yield for critical situation
					CriticalGCEvent->Trigger();
				});
				CriticalGCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(CriticalGCEvent);
			}

			int32 BatchEnd = FMath::Min(BatchStart + DeepBatchSize, TotalDeep);

			// Capture the slice for this batch
			TArray<FDeepIndexEntry> BatchSlice;
			BatchSlice.Reserve(BatchEnd - BatchStart);
			for (int32 j = BatchStart; j < BatchEnd; ++j)
			{
				BatchSlice.Add(DeepIndexQueue[j]);
			}

			FEvent* BatchEvent = FPlatformProcess::GetSynchEventFromPool(true);

			// CRITICAL: Dispatch via FTSTicker (not AsyncTask(GT)) so our work only
			// fires once the asset compiler reports idle (GetNumRemainingAssets() == 0).
			// AsyncTask(GT) can be drained inside FTextureCompilingManager::PostCompilation's
			// bIsRoutingPostCompilation guard, and any asset load from there would fatal
			// in FinishAllCompilation (TextureCompiler.cpp:454). The previous fix
			// (calling FinishAllCompilation inside the lambda) was the exact trigger —
			// see GitHub issue #19, regression from commit 168c087.
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, BatchSlice = MoveTemp(BatchSlice), &DeepIndexed, &DeepErrors, FrameBudgetSeconds]()
			{
				DB->BeginTransaction();
				double BatchStartTime = FPlatformTime::Seconds();

				for (const FDeepIndexEntry& Entry : BatchSlice)
				{
					// Load asset on game thread — the dispatcher guarantees the asset
					// compiler is idle before this runs, so GetAsset() won't reenter
					// the texture compiler's PostCompilation guard.
					const FMonolithPackageResidency Residency =
						FMonolithMemoryHelper::CapturePackageResidency(Entry.AssetData.PackageName);
					UObject* LoadedAsset = Entry.AssetData.GetAsset();
					if (LoadedAsset)
					{
						if (Entry.Indexer->IndexAsset(Entry.AssetData, LoadedAsset, *DB, Entry.AssetId))
						{
							DeepIndexed++;
						}
						else
						{
							DeepErrors++;
							UE_LOG(LogMonolithIndex, Warning, TEXT("Deep indexer '%s' failed for: %s"),
								*Entry.Indexer->GetName(),
								*Entry.AssetData.PackageName.ToString());
						}

						// Mark asset for unloading to help GC
						FMonolithMemoryHelper::TryUnloadPackage(LoadedAsset, Residency);
					}
					else
					{
						DeepErrors++;
						UE_LOG(LogMonolithIndex, Warning, TEXT("Failed to load asset for deep indexing: %s (class: %s)"),
							*Entry.AssetData.PackageName.ToString(),
							*Entry.AssetData.AssetClassPath.GetAssetName().ToString());
					}

					// If we've exceeded our frame budget, commit what we have and yield
					double Elapsed = FPlatformTime::Seconds() - BatchStartTime;
					if (Elapsed > FrameBudgetSeconds)
					{
						DB->CommitTransaction();
						FMonolithMemoryHelper::YieldToEditor();
						DB->BeginTransaction();
						BatchStartTime = FPlatformTime::Seconds();
					}
				}

				DB->CommitTransaction();
			},
			BatchEvent);

			BatchEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(BatchEvent);

			BatchNumber++;

			// Periodic GC based on configured frequency
			if (BatchNumber % GCFrequency == 0)
			{
				FEvent* PeriodicGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [PeriodicGCEvent]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(false);
					FMonolithMemoryHelper::YieldToEditor();
					PeriodicGCEvent->Trigger();
				});
				PeriodicGCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(PeriodicGCEvent);
			}

			// Update progress — report deep pass as second half of overall progress
			CurrentIndex = Indexed + BatchEnd;
			TotalAssets = Indexed + TotalDeep;

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Deep indexing %d / %d assets..."), BatchEnd, TotalDeep)));
			}

			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				Owner->UpdateActiveAsyncJobProgress(CurrentIndex.Load(), TotalAssets.Load(), TEXT("deep_indexing"));
				Owner->OnProgress.Broadcast(CurrentIndex.Load(), TotalAssets.Load());
			});

			// Log progress and memory periodically
			if (BatchNumber % 10 == 0)
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexed %d / %d assets (%d ok, %d errors)"),
					BatchEnd, TotalDeep, DeepIndexed.Load(), DeepErrors.Load());

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("After batch %d"), BatchNumber));
				}
			}
		}

		// Final GC after deep indexing
		FEvent* FinalGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [FinalGCEvent]()
		{
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FinalGCEvent->Trigger();
		});
		FinalGCEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(FinalGCEvent);

		UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexing complete: %d indexed, %d errors"),
			DeepIndexed.Load(), DeepErrors.Load());

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index complete"));
		}
	}

	// Build indexed paths list for post-pass indexers
	TArray<FName> IndexedPaths;
	IndexedPaths.Add(FName(TEXT("/Game")));
	for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
	{
		FString CleanPath = PluginInfo.MountPath;
		if (CleanPath.EndsWith(TEXT("/")))
		{
			CleanPath.LeftChopInline(1);
		}
		IndexedPaths.Add(FName(*CleanPath));
	}
	// Add user-configured additional content paths
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (Settings)
		{
			for (const FString& CustomPath : Settings->AdditionalContentPaths)
			{
				if (!CustomPath.IsEmpty())
				{
					FString CleanPath = CustomPath;
					if (CleanPath.EndsWith(TEXT("/")))
					{
						CleanPath.LeftChopInline(1);
					}
					IndexedPaths.AddUnique(FName(*CleanPath));
				}
			}
		}
	}

	// Helper lambda to run GC and yield between indexers
	auto GCBetweenIndexers = [this]()
	{
		FEvent* GCEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [GCEvent]()
		{
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();
			GCEvent->Trigger();
		});
		GCEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(GCEvent);
	};

	// Helper to check for cancellation
	auto CheckCancellation = [this]() -> bool
	{
		if (bShouldStop) return true;
		if (!AsyncJobId.IsEmpty() && FMonolithAsyncJobRegistry::Get().IsCancelRequested(AsyncJobId))
		{
			bShouldStop = true;
			return true;
		}
		if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
		{
			if (!AsyncJobId.IsEmpty())
			{
				FMonolithAsyncJobRegistry::Get().RequestCancel(AsyncJobId);
			}
			bShouldStop = true;
			return true;
		}
		return false;
	};

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting post-pass indexers..."));

	// Run dependency indexer on game thread (Asset Registry requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Analyzing dependencies...");
		TSharedPtr<IMonolithIndexer>* DepIndexer = Owner->ClassToIndexer.Find(TEXT("__Dependencies__"));
		if (DepIndexer && DepIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running dependency indexer..."));
			TSharedPtr<IMonolithIndexer> DepIndexerCopy = *DepIndexer;
			if (FDependencyIndexer* DepRaw = static_cast<FDependencyIndexer*>(DepIndexerCopy.Get()))
			{
				DepRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* DepEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, DepIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				DepIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			DepEvent);
			DepEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DepEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Dependency indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run level indexer on game thread (asset loading requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing level actors...");
		TSharedPtr<IMonolithIndexer>* LevelIndexer = Owner->ClassToIndexer.Find(TEXT("__Levels__"));
		if (LevelIndexer && LevelIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running level indexer..."));
			TSharedPtr<IMonolithIndexer> LevelIndexerCopy = *LevelIndexer;
			if (FLevelIndexer* LevelRaw = static_cast<FLevelIndexer*>(LevelIndexerCopy.Get()))
			{
				LevelRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* LevelEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, LevelIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				LevelIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			LevelEvent);
			LevelEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(LevelEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Level indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run DataTable indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing DataTable rows...");
		TSharedPtr<IMonolithIndexer>* DTIndexer = Owner->ClassToIndexer.Find(TEXT("__DataTables__"));
		if (DTIndexer && DTIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running DataTable indexer..."));
			TSharedPtr<IMonolithIndexer> DTIndexerCopy = *DTIndexer;
			FEvent* DTEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, DTIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				DTIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			DTEvent);
			DTEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DTEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("DataTable indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run config indexer (file I/O only, no game thread needed)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing config files...");
		TSharedPtr<IMonolithIndexer>* CfgIndexer = Owner->ClassToIndexer.Find(TEXT("__Configs__"));
		if (CfgIndexer && CfgIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running config indexer..."));
			DB->BeginTransaction();
			FAssetData DummyCfgData;
			(*CfgIndexer)->IndexAsset(DummyCfgData, nullptr, *DB, 0);
			DB->CommitTransaction();
			UE_LOG(LogMonolithIndex, Log, TEXT("Config indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
		}
	}

	// Run C++ symbol indexer (file I/O only, no game thread needed)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing C++ symbols...");
		TSharedPtr<IMonolithIndexer>* CppIndexer = Owner->ClassToIndexer.Find(TEXT("__CppSymbols__"));
		if (CppIndexer && CppIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running C++ symbol indexer..."));
			DB->BeginTransaction();
			FAssetData DummyCppData;
			(*CppIndexer)->IndexAsset(DummyCppData, nullptr, *DB, 0);
			DB->CommitTransaction();
			UE_LOG(LogMonolithIndex, Log, TEXT("C++ symbol indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
		}
	}

	// Run animation indexer on game thread (asset loading requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing animations...");
		TSharedPtr<IMonolithIndexer>* AnimIndexer = Owner->ClassToIndexer.Find(TEXT("__Animations__"));
		if (AnimIndexer && AnimIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running animation indexer..."));
			TSharedPtr<IMonolithIndexer> AnimIndexerCopy = *AnimIndexer;
			FEvent* AnimEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, AnimIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				AnimIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			AnimEvent);
			AnimEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(AnimEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Animation indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run gameplay tag indexer on game thread (GameplayTagsManager requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing gameplay tags...");
		TSharedPtr<IMonolithIndexer>* TagIndexer = Owner->ClassToIndexer.Find(TEXT("__GameplayTags__"));
		if (TagIndexer && TagIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running gameplay tag indexer..."));
			TSharedPtr<IMonolithIndexer> TagIndexerCopy = *TagIndexer;
			FEvent* TagEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, TagIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				TagIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			TagEvent);
			TagEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(TagEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Gameplay tag indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run Niagara indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing Niagara systems...");
		TSharedPtr<IMonolithIndexer>* NiagaraIndexerPtr = Owner->ClassToIndexer.Find(TEXT("__Niagara__"));
		if (NiagaraIndexerPtr && NiagaraIndexerPtr->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running Niagara indexer..."));
			TSharedPtr<IMonolithIndexer> NiagaraIndexerCopy = *NiagaraIndexerPtr;
			FEvent* NiagaraEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, NiagaraIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				NiagaraIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			NiagaraEvent);
			NiagaraEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(NiagaraEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Niagara indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run mesh catalog indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Building mesh catalog...");
		TSharedPtr<IMonolithIndexer>* MeshCatIndexer = Owner->ClassToIndexer.Find(TEXT("__MeshCatalog__"));
		if (MeshCatIndexer && MeshCatIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running mesh catalog indexer..."));
			TSharedPtr<IMonolithIndexer> MeshCatIndexerCopy = *MeshCatIndexer;
			if (FMeshCatalogIndexer* MeshCatRaw = static_cast<FMeshCatalogIndexer*>(MeshCatIndexerCopy.Get()))
			{
				MeshCatRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* MeshCatEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, MeshCatIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				MeshCatIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			MeshCatEvent);
			MeshCatEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(MeshCatEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Mesh catalog indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run domain asset indexer on game thread. It is AssetRegistry-only and intentionally
	// avoids loading ControlRig/RigVM/StateTree/Chooser assets during full indexing.
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing domain asset metadata...");
		TSharedPtr<IMonolithIndexer>* DomainIndexer = Owner->ClassToIndexer.Find(TEXT("__DomainAssets__"));
		if (DomainIndexer && DomainIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running domain asset indexer..."));
			TSharedPtr<IMonolithIndexer> DomainIndexerCopy = *DomainIndexer;
			FEvent* DomainEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, DomainIndexerCopy]()
			{
				DB->BeginTransaction();
				FAssetData DummyData;
				DomainIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
				DB->CommitTransaction();
			},
			DomainEvent);
			DomainEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DomainEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Domain asset indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Post-pass indexers complete"));

	// Write index timestamp to meta (only if not cancelled and asset count looks valid)
	if (!bShouldStop)
	{
		constexpr int32 MinAssetCountThreshold = 500;
		if (Indexed < MinAssetCountThreshold)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Index only found %d assets — Asset Registry may not have been fully loaded. Skipping last_full_index write so next launch will re-index."), Indexed);
		}
		else
		{
			DB->WriteMeta(TEXT("last_full_index"), FDateTime::UtcNow().ToString());
			// I2 (PRD AssetSearchSemanticSearch): stamp the indexer-fleet signature this full index
			// was produced with, so a later indexer-version bump invalidates incremental.
			DB->WriteMeta(TEXT("indexer_fleet_signature"), Owner->ComputeIndexerFleetSignature());
			UE_LOG(LogMonolithIndex, Log, TEXT("Wrote last_full_index timestamp (%d assets indexed)"), Indexed);
		}
	}

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index complete"));
	}

	TWeakObjectPtr<UMonolithIndexSubsystem> WeakOwner(Owner);
	const bool bFinishedSuccessfully = !bShouldStop;
	AsyncTask(ENamedThreads::GameThread, [WeakOwner, bFinishedSuccessfully]()
	{
		if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
		{
			Subsystem->OnIndexingFinished(bFinishedSuccessfully);
		}
	});

	return 0;
}

void UMonolithIndexSubsystem::OnIndexingFinished(bool bSuccess)
{
	bIsIndexing = false;
	IndexingStatusMessage.Empty();

	// Restore the incremental-reachability GC setting captured at run start. This
	// is the single completion point for ALL worker exit paths (normal, DB-open
	// failure, and bShouldStop cancel/abort), so the dtor always runs here.
	GIncrementalGCOverride.Reset();

	if (IndexingThread)
	{
		IndexingThread->WaitForCompletion();
		IndexingThread.Reset();
	}

	IndexingTaskPtr.Reset();

	if (TaskNotification)
	{
		TaskNotification->SetComplete(
			FText::FromString(TEXT("Monolith")),
			FText::FromString(bSuccess ? TEXT("Project indexing complete") : TEXT("Project indexing failed")),
			bSuccess);
		TaskNotification.Reset();
	}

	if (bSuccess && Database.IsValid() && Database->IsOpen())
	{
		TSharedPtr<FJsonObject> CrgResult = FMonolithIndexReview::RepairCrgCache(*Database, true);
		FString Status;
		FString Summary;
		if (CrgResult.IsValid())
		{
			CrgResult->TryGetStringField(TEXT("status"), Status);
			CrgResult->TryGetStringField(TEXT("summary"), Summary);
		}

		if (Status == TEXT("ok"))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("Project CRG projection/cache rebuilt after indexing: %s"), *Summary);
		}
		else
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Project CRG projection/cache rebuild after indexing did not complete cleanly: %s"), *Summary);
		}

		// Q5 (PRD AssetSearchSemanticSearch): compact the project FTS5 indexes once per
		// full reindex. Every asset/node/.../value write fires ai/ad/au triggers, leaving
		// segment fragmentation the per-write path never compacts; an end-of-bulk
		// 'optimize' merge + refreshed planner stats (PRAGMA optimize) improve query
		// latency and DB size. Gated by this bSuccess full-reindex completion point — the
		// same gate as the CRG repair above, so the live-incremental path never runs it.
		if (FSQLiteDatabase* RawDb = Database->GetRawDatabase())
		{
			FMonolithSQLiteMaintenanceOptions MaintOpts;
			MaintOpts.bRunPragmaOptimize = true;
			MaintOpts.bRunIncrementalVacuum = false;
			MaintOpts.FtsTablesToOptimize = {
				TEXT("fts_assets"), TEXT("fts_nodes"), TEXT("fts_variables"),
				TEXT("fts_parameters"), TEXT("fts_datatable_rows"), TEXT("fts_actors"),
				TEXT("fts_asset_search_values") };
			if (RunMonolithSQLiteMaintenance(*RawDb, MaintOpts))
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Project FTS maintenance (optimize) complete after indexing"));
			}
		}
	}

	// Full indexing detaches live callbacks while the writer owns the database.
	// Re-arm on every outcome; the helper itself remains activation-, state-,
	// and database-aware, so a genuine stop still leaves callbacks detached.
	RegisterLiveCallbacks();
	OnComplete.Broadcast(bSuccess);
	OnProgress.Clear();
	FinishActiveAsyncJob(bSuccess);

	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %s"),
		bSuccess ? TEXT("completed successfully") : TEXT("failed or was cancelled"));
}

FString UMonolithIndexSubsystem::GetDatabasePath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("ProjectIndex.db");
	}
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("ProjectIndex.db");
}

bool UMonolithIndexSubsystem::ShouldAutoIndex() const
{
	if (!Database.IsValid() || !Database->IsOpen()) return false;

	FSQLiteDatabase* RawDB = Database->GetRawDatabase();
	if (!RawDB) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*RawDB, TEXT("SELECT value FROM meta WHERE key = 'last_full_index';"));
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		return false; // Already indexed before
	}
	return true;
}

bool UMonolithIndexSubsystem::IsIndexingWorkEnabled() const
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	return bAutomaticIndexingEnabled
		&& (!Settings || Settings->bEnableIndex)
		&& UMonolithSettings::IsIndexingActivated();
}

bool UMonolithIndexSubsystem::CanAcceptIndexRequest() const
{
	return IsIndexingWorkEnabled()
		&& !bIsIndexing
		&& Database.IsValid()
		&& Database->IsOpen();
}

bool UMonolithIndexSubsystem::CanDoIncrementalIndex() const
{
	if (!Database || !Database->IsOpen()) return false;
	FString SchemaVersion = Database->ReadMeta(TEXT("schema_version"));
	if (SchemaVersion.IsEmpty() || FCString::Atoi(*SchemaVersion) < 2)
		return false;
	FString LastFullIndex = Database->ReadMeta(TEXT("last_full_index"));
	if (LastFullIndex.IsEmpty())
		return false;
	// I2 (PRD AssetSearchSemanticSearch): if any registered indexer's version changed since the
	// last full index, the stored rows may be stale (a logic change an unchanged content-hash
	// would skip) — force a full reindex. Pre-I2 DBs have no stored signature, so the first launch
	// after this ships re-baselines once.
	const FString StoredFleetSignature = Database->ReadMeta(TEXT("indexer_fleet_signature"));
	if (StoredFleetSignature != ComputeIndexerFleetSignature())
		return false;
	return true;
}

bool UMonolithIndexSubsystem::StartIncrementalIndex()
{
	return StartIncrementalIndexInternal(FString());
}

bool UMonolithIndexSubsystem::StartIncrementalIndexInternal(const FString& JobId)
{
	check(IsInGameThread());
	if (!IsIndexingWorkEnabled())
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Incremental project indexing is disabled; run Monolith.StartIndexing first"));
		FailSubmittedAsyncJob(
			JobId,
			TEXT("Monolith indexing is disabled. Run Monolith.StartIndexing in the editor console."));
		return false;
	}

	if (bIsIndexing)
	{
		FailSubmittedAsyncJob(JobId, TEXT("Project indexing is already in progress."));
		return false;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		FailSubmittedAsyncJob(JobId, TEXT("Project index database is not open."));
		return false;
	}

	bIsIndexing = true;
	BeginActiveAsyncJob(JobId, TEXT("incremental"), TEXT("Incremental project index starting."));
	UnregisterLiveCallbacks();
	PendingChanges.Reset();

	IndexedPlugins = GatherMarketplacePluginPaths();

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting incremental index..."));

	// PHASE 1: Build current AR state
	TSet<FName> CurrentPackages;
	TMap<FName, FIoHash> CurrentHashes;
	IAssetRegistry& AR = IAssetRegistry::GetChecked();

	TSet<FString> ValidPrefixes;
	ValidPrefixes.Add(TEXT("/Game/"));
	for (const FIndexedPluginInfo& Plugin : IndexedPlugins)
	{
		ValidPrefixes.Add(Plugin.MountPath);
	}
	// Include AdditionalContentPaths from settings
	if (const UMonolithSettings* Settings = GetDefault<UMonolithSettings>())
	{
		for (const FString& CustomPath : Settings->AdditionalContentPaths)
		{
			if (!CustomPath.IsEmpty())
				ValidPrefixes.Add(CustomPath);
		}
	}

	AR.EnumerateAllPackages([&](FName PackageName, const FAssetPackageData& PkgData)
	{
		FString PkgStr = PackageName.ToString();
		for (const FString& Prefix : ValidPrefixes)
		{
			if (PkgStr.StartsWith(Prefix))
			{
				CurrentPackages.Add(PackageName);
				CurrentHashes.Add(PackageName, PkgData.GetPackageSavedHash());
				break;
			}
		}
	});
	UpdateActiveAsyncJobProgress(15.0, TEXT("scanning_assets"), TEXT("Scanned current Asset Registry package state."));

	// PHASE 2: Build DB state
	TMap<FString, FString> DBPathsAndHashes = Database->GetAllPathsAndHashes();
	TSet<FName> DBPackages;
	TMap<FName, FIoHash> DBHashes;
	for (const auto& [Path, Hash] : DBPathsAndHashes)
	{
		FName PathName(*Path);
		DBPackages.Add(PathName);
		if (!Hash.IsEmpty())
		{
			FIoHash IoHash;
			LexFromString(IoHash, *Hash);
			DBHashes.Add(PathName, IoHash);
		}
	}

	// PHASE 3: Compute deltas
	TArray<FName> AddedPaths, DeletedPaths, ExistingPaths;
	AddedPaths.Reserve(CurrentPackages.Num());
	ExistingPaths.Reserve(CurrentPackages.Num());
	for (FName Pkg : CurrentPackages)
	{
		if (!DBPackages.Contains(Pkg)) AddedPaths.Add(Pkg);
		else ExistingPaths.Add(Pkg);
	}
	DeletedPaths.Reserve(DBPackages.Num());
	for (FName Pkg : DBPackages)
	{
		if (!CurrentPackages.Contains(Pkg)) DeletedPaths.Add(Pkg);
	}

	// PHASE 4: Move detection
	TMultiMap<FIoHash, FName> DeletedHashMap;
	for (FName Deleted : DeletedPaths)
	{
		if (FIoHash* Hash = DBHashes.Find(Deleted))
		{
			if (!Hash->IsZero()) DeletedHashMap.Add(*Hash, Deleted);
		}
	}

	TArray<TPair<FName, FName>> Moves;
	Moves.Reserve(AddedPaths.Num());
	TArray<FName> TrueAdds;
	TrueAdds.Reserve(AddedPaths.Num());
	for (FName Added : AddedPaths)
	{
		FIoHash* NewHash = CurrentHashes.Find(Added);
		if (NewHash && !NewHash->IsZero())
		{
			// TMultiMap::RemoveSingle(Key, Value) requires BOTH to match.
			// Must MultiFind first, then RemoveSingle with the found value.
			TArray<FName> FoundOldPaths;
			DeletedHashMap.MultiFind(*NewHash, FoundOldPaths);
			if (FoundOldPaths.Num() > 0)
			{
				FName MatchedOldPath = FoundOldPaths[0];
				DeletedHashMap.RemoveSingle(*NewHash, MatchedOldPath);
				Moves.Add({MatchedOldPath, Added});
				continue;
			}
		}
		TrueAdds.Add(Added);
	}

	TSet<FName> MovedOldPaths;
	for (const auto& [OldPath, NewPath] : Moves) MovedOldPaths.Add(OldPath);

	TArray<FName> TrueDeletes;
	TrueDeletes.Reserve(DeletedPaths.Num());
	for (FName Deleted : DeletedPaths)
	{
		if (!MovedOldPaths.Contains(Deleted)) TrueDeletes.Add(Deleted);
	}

	// PHASE 5: Modification detection
	TArray<FName> ModifiedPaths;
	ModifiedPaths.Reserve(ExistingPaths.Num());
	for (FName Existing : ExistingPaths)
	{
		FIoHash* CurrentHash = CurrentHashes.Find(Existing);
		FIoHash* StoredHash = DBHashes.Find(Existing);
		if (CurrentHash && StoredHash && *CurrentHash != *StoredHash)
			ModifiedPaths.Add(Existing);
		else if (CurrentHash && !StoredHash)
			ModifiedPaths.Add(Existing);  // Pre-v2 asset with no stored hash
	}

	TSet<FString> CrgTouchedPaths;
	CrgTouchedPaths.Reserve(TrueAdds.Num() + TrueDeletes.Num() + Moves.Num() * 2 + ModifiedPaths.Num());
	for (FName Path : TrueAdds) CrgTouchedPaths.Add(Path.ToString());
	for (FName Path : TrueDeletes) CrgTouchedPaths.Add(Path.ToString());
	for (FName Path : ModifiedPaths) CrgTouchedPaths.Add(Path.ToString());
	for (const auto& [OldPath, NewPath] : Moves)
	{
		CrgTouchedPaths.Add(OldPath.ToString());
		CrgTouchedPaths.Add(NewPath.ToString());
	}

	UE_LOG(LogMonolithIndex, Log,
		TEXT("Incremental delta: %d added, %d deleted, %d moved, %d modified, %d unchanged"),
		TrueAdds.Num(), TrueDeletes.Num(), Moves.Num(), ModifiedPaths.Num(),
		ExistingPaths.Num() - ModifiedPaths.Num());
	UpdateActiveAsyncJobProgress(35.0, TEXT("computed_delta"), TEXT("Computed incremental project index delta."));

	auto AbortIncrementalIndexIfCancelled = [this](const TCHAR* Reason, bool bRollbackTransaction) -> bool
	{
		if (!IsActiveAsyncJobCancellationRequested())
		{
			return false;
		}

		UE_LOG(LogMonolithIndex, Log, TEXT("Incremental index cancelled: %s"), Reason);
		if (bRollbackTransaction)
		{
			Database->RollbackTransaction();
		}
		bIsIndexing = false;
		RegisterLiveCallbacks();
		OnComplete.Broadcast(false);
		OnProgress.Clear();
		FinishActiveAsyncJob(false);
		return true;
	};

	if (AbortIncrementalIndexIfCancelled(TEXT("before applying deltas"), false))
	{
		return false;
	}

	// Early return if no changes
	if (TrueDeletes.Num() == 0 && TrueAdds.Num() == 0 && Moves.Num() == 0 && ModifiedPaths.Num() == 0)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("No changes detected. Incremental index complete."));
		bIsIndexing = false;
		RegisterLiveCallbacks();
		OnComplete.Broadcast(true);
		OnProgress.Clear();
		FinishActiveAsyncJob(true);
		return true;
	}

	// PHASE 6: Apply deltas
	Database->BeginTransaction();

	// 6a: Deletions
	for (FName Path : TrueDeletes)
		Database->DeleteAssetByPath(Path.ToString());
	if (AbortIncrementalIndexIfCancelled(TEXT("after deletion phase"), true))
	{
		return false;
	}

	// 6b: Moves
	for (const auto& [OldPath, NewPath] : Moves)
	{
		Database->UpdateAssetPath(OldPath.ToString(), NewPath.ToString());
		if (FIoHash* Hash = CurrentHashes.Find(NewPath))
			Database->UpdateSavedHash(NewPath.ToString(), LexToString(*Hash));
	}
	if (AbortIncrementalIndexIfCancelled(TEXT("after move phase"), true))
	{
		return false;
	}

	// 6c: Build paths needing (re-)indexing
	TSet<FName> PathsToIndex;
	PathsToIndex.Reserve(TrueAdds.Num() + ModifiedPaths.Num() + Moves.Num());
	for (FName Path : TrueAdds) PathsToIndex.Add(Path);
	for (FName Path : ModifiedPaths) PathsToIndex.Add(Path);
	for (const auto& [OldPath, NewPath] : Moves)
	{
		FIoHash* CurrentHash = CurrentHashes.Find(NewPath);
		FIoHash* StoredHash = DBHashes.Find(OldPath);
		if (CurrentHash && StoredHash && *CurrentHash != *StoredHash)
			PathsToIndex.Add(NewPath);
	}

	// 6d: Insert/update asset metadata for paths needing indexing
	for (FName Path : PathsToIndex)
	{
		FString PathStr = Path.ToString();
		int64 AssetId = Database->GetAssetId(PathStr);

		// Build FIndexedAsset from AR
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(Path, Assets);
		if (Assets.Num() == 0) continue;

		const FAssetData& AssetData = Assets[0];
		FIndexedAsset IndexedAsset;
		IndexedAsset.PackagePath = PathStr;
		IndexedAsset.AssetName = AssetData.AssetName.ToString();
		IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();

		// Determine module name (same logic as full index path)
		if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
			{
				if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
				{
					IndexedAsset.ModuleName = PluginInfo.PluginName;
					break;
				}
			}
			// Fallback: extract from path
			if (IndexedAsset.ModuleName.IsEmpty())
			{
				int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
				if (SecondSlash > 1)
				{
					IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}
		}

		// Populate LastModified
		FString PackageFilename;
		if (FPackageName::DoesPackageExist(PathStr, &PackageFilename))
		{
			FDateTime FileTime = IFileManager::Get().GetTimeStamp(*PackageFilename);
			IndexedAsset.LastModified = FileTime.ToIso8601();
		}
		// Don't populate SavedHash yet — deferred to Phase 10 for crash recovery

		if (AssetId > 0)
		{
			// Existing asset — update metadata, clear children
			Database->UpdateAssetMetadata(IndexedAsset);
			Database->DeleteChildDataForAsset(AssetId);
		}
		else
		{
			// New asset
			Database->InsertAsset(IndexedAsset);
		}
	}
	if (AbortIncrementalIndexIfCancelled(TEXT("after metadata upsert phase"), true))
	{
		return false;
	}

	// PHASE 7: Deep-index
	TSet<FString> PathStrings;
	PathStrings.Reserve(PathsToIndex.Num());
	for (FName Path : PathsToIndex) PathStrings.Add(Path.ToString());
	if (AbortIncrementalIndexIfCancelled(TEXT("before deep indexing"), true))
	{
		return false;
	}
	ProcessDeepIndexQueue(PathStrings);
	UpdateActiveAsyncJobProgress(75.0, TEXT("deep_indexing"), TEXT("Incremental deep indexing complete."));
	if (AbortIncrementalIndexIfCancelled(TEXT("after deep indexing"), true))
	{
		return false;
	}

	// PHASE 8: Commit
	UpdateActiveAsyncJobProgress(90.0, TEXT("committing"), TEXT("Committing incremental project index changes."));
	Database->CommitTransaction();
	FinishActiveAsyncJob(true);

	// PHASE 9: Sentinels (stub — implemented in Task 6)
	// TSet<FString> RemovedPathStrings;
	// for (FName Path : TrueDeletes) RemovedPathStrings.Add(Path.ToString());
	// RunScopedSentinels(PathStrings, RemovedPathStrings);

	// PHASE 10: Update hashes (deferred for crash recovery)
	Database->BeginTransaction();
	for (FName Path : PathsToIndex)
	{
		if (FIoHash* Hash = CurrentHashes.Find(Path))
			Database->UpdateSavedHash(Path.ToString(), LexToString(*Hash));
	}
	Database->CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("Incremental index complete."));
	bIsIndexing = false;
	RefreshProjectCrgCacheForChangedAssets(Database.Get(), CrgTouchedPaths, TEXT("Incremental project indexing complete"));
	RegisterLiveCallbacks();
	OnComplete.Broadcast(true);
	OnProgress.Clear();
	return true;
}

// ============================================================
// Stubs for Tasks 5-6
// ============================================================

void UMonolithIndexSubsystem::ProcessDeepIndexQueue(const TSet<FString>& PathsToIndex)
{
	if (PathsToIndex.Num() == 0) return;

	IAssetRegistry& AR = IAssetRegistry::GetChecked();
	int32 Indexed = 0;

	for (const FString& PackagePath : PathsToIndex)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(FName(*PackagePath), Assets);

		for (const FAssetData& AssetData : Assets)
		{
			FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
			// Same exact-then-parent-walk resolution as the full pass, so incrementally
			// re-indexed UGo*DataAsset assets reach FDataAssetIndexer via their ancestors.
			TSharedPtr<IMonolithIndexer> Indexer = ResolveDeepIndexer(ClassName, AssetData.AssetClassPath, &AR);
			if (!Indexer.IsValid()) continue;

			int64 AssetId = Database->GetAssetId(PackagePath);
			if (AssetId <= 0) continue;

			// Load the asset (must be game thread)
			UObject* LoadedAsset = AssetData.GetAsset();
			if (!LoadedAsset) continue;

			Indexer->IndexAsset(AssetData, LoadedAsset, *Database, AssetId);
			++Indexed;
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep-indexed %d assets from %d paths"), Indexed, PathsToIndex.Num());
}

void UMonolithIndexSubsystem::RunScopedSentinels(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths)
{
	if (ChangedPaths.Num() == 0 && RemovedPaths.Num() == 0) return;

	for (const auto& Indexer : Indexers)
	{
		if (Indexer->IsSentinel() && Indexer->SupportsIncrementalIndex())
		{
			double StartTime = FPlatformTime::Seconds();
			Indexer->IndexScoped(ChangedPaths, RemovedPaths, *Database);
			double Duration = FPlatformTime::Seconds() - StartTime;
			UE_LOG(LogMonolithIndex, Log, TEXT("Scoped sentinel %s completed in %.2fs"), *Indexer->GetName(), Duration);
		}
	}
}

void UMonolithIndexSubsystem::RegisterLiveCallbacks()
{
	if (!IsIndexingWorkEnabled() || bIsIndexing
		|| !Database.IsValid() || !Database->IsOpen())
	{
		return;
	}

	if (OnAssetsAddedHandle.IsValid()
		|| OnAssetsRemovedHandle.IsValid()
		|| OnAssetRenamedHandle.IsValid()
		|| OnAssetsUpdatedOnDiskHandle.IsValid())
	{
		return;
	}

	IAssetRegistry& AR = IAssetRegistry::GetChecked();

	OnAssetsAddedHandle = AR.OnAssetsAdded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsAddedCallback);
	OnAssetsRemovedHandle = AR.OnAssetsRemoved().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsRemovedCallback);
	OnAssetRenamedHandle = AR.OnAssetRenamed().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRenamedCallback);
	OnAssetsUpdatedOnDiskHandle = AR.OnAssetsUpdatedOnDisk().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback);

	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			LiveIndexTimerHandle,
			FTimerDelegate::CreateUObject(this, &UMonolithIndexSubsystem::ProcessPendingChanges),
			2.0f, /*bLoop=*/ true);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Live index callbacks registered."));
}

void UMonolithIndexSubsystem::UnregisterLiveCallbacks()
{
	const bool bHadCallbacks =
		OnAssetsAddedHandle.IsValid()
		|| OnAssetsRemovedHandle.IsValid()
		|| OnAssetRenamedHandle.IsValid()
		|| OnAssetsUpdatedOnDiskHandle.IsValid();

	if (IAssetRegistry* AR = IAssetRegistry::Get())
	{
		AR->OnAssetsAdded().Remove(OnAssetsAddedHandle);
		AR->OnAssetsRemoved().Remove(OnAssetsRemovedHandle);
		AR->OnAssetRenamed().Remove(OnAssetRenamedHandle);
		AR->OnAssetsUpdatedOnDisk().Remove(OnAssetsUpdatedOnDiskHandle);
	}
	OnAssetsAddedHandle.Reset();
	OnAssetsRemovedHandle.Reset();
	OnAssetRenamedHandle.Reset();
	OnAssetsUpdatedOnDiskHandle.Reset();

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(LiveIndexTimerHandle);
	}

	if (bHadCallbacks)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Live index callbacks unregistered."));
	}
}

// ============================================================
// Live AR callback handlers
// ============================================================

static bool IsRedirector(const FAssetData& AssetData)
{
	static const FTopLevelAssetPath RedirectorPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector"));
	return AssetData.AssetClassPath == RedirectorPath;
}

void UMonolithIndexSubsystem::OnAssetsAddedCallback(TConstArrayView<FAssetData> Assets)
{
	if (!IsIndexingWorkEnabled() || bIsIndexing) return;
	PendingChanges.Reserve(PendingChanges.Num() + Assets.Num());
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData))
			PendingChanges.Add({EIndexChangeType::Added, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetsRemovedCallback(TConstArrayView<FAssetData> Assets)
{
	if (!IsIndexingWorkEnabled() || bIsIndexing) return;
	PendingChanges.Reserve(PendingChanges.Num() + Assets.Num());
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData))
			PendingChanges.Add({EIndexChangeType::Removed, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetRenamedCallback(const FAssetData& AssetData, const FString& OldObjectPath)
{
	if (!IsIndexingWorkEnabled() || bIsIndexing) return;
	PendingChanges.Add({EIndexChangeType::Renamed, AssetData, OldObjectPath});
}

void UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback(TConstArrayView<FAssetData> Assets)
{
	if (!IsIndexingWorkEnabled() || bIsIndexing) return;
	PendingChanges.Reserve(PendingChanges.Num() + Assets.Num());
	for (const FAssetData& AssetData : Assets)
		PendingChanges.Add({EIndexChangeType::Updated, AssetData, {}});
}

void UMonolithIndexSubsystem::ProcessPendingChanges()
{
	if (!IsIndexingWorkEnabled())
	{
		PendingChanges.Reset();
		return;
	}

	if (PendingChanges.Num() == 0) return;

	TArray<FPendingIndexChange> RawChanges = MoveTemp(PendingChanges);
	PendingChanges.Reset();

	if (!Database || !Database->IsOpen()) return;

	// DEDUP: Collapse multiple changes to same path
	TMap<FName, int32> PathToLastIndex;
	PathToLastIndex.Reserve(RawChanges.Num());
	TArray<FPendingIndexChange> LocalChanges;
	LocalChanges.Reserve(RawChanges.Num());

	for (int32 i = 0; i < RawChanges.Num(); ++i)
	{
		FName PkgName = RawChanges[i].AssetData.PackageName;
		if (int32* ExistingIdx = PathToLastIndex.Find(PkgName))
		{
			EIndexChangeType PrevType = LocalChanges[*ExistingIdx].Type;
			EIndexChangeType NewType = RawChanges[i].Type;

			if (PrevType == EIndexChangeType::Renamed && NewType == EIndexChangeType::Updated)
			{
				// Keep the rename
			}
			else if (PrevType == EIndexChangeType::Removed && NewType == EIndexChangeType::Added)
			{
				RawChanges[i].Type = EIndexChangeType::Updated;
				LocalChanges[*ExistingIdx] = MoveTemp(RawChanges[i]);
			}
			else
			{
				LocalChanges[*ExistingIdx] = MoveTemp(RawChanges[i]);
			}
		}
		else
		{
			PathToLastIndex.Add(PkgName, LocalChanges.Num());
			LocalChanges.Add(MoveTemp(RawChanges[i]));
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Processing %d pending index changes (%d raw)"),
		LocalChanges.Num(), RawChanges.Num());

	Database->BeginTransaction();

	TSet<FString> PathsToDeepIndex;
	TSet<FString> PathsForScopedSentinels;
	TSet<FString> RemovedPaths;
	TSet<FString> CrgTouchedPaths;

	for (const FPendingIndexChange& Change : LocalChanges)
	{
		switch (Change.Type)
		{
		case EIndexChangeType::Added:
		{
			FIndexedAsset IndexedAsset;
			IndexedAsset.PackagePath = Change.AssetData.PackageName.ToString();
			IndexedAsset.AssetName = Change.AssetData.AssetName.ToString();
			IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			// Module name resolution
			if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
			{
				for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
				{
					if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
					{
						IndexedAsset.ModuleName = PluginInfo.PluginName;
						break;
					}
				}
				if (IndexedAsset.ModuleName.IsEmpty())
				{
					int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
					if (SecondSlash > 1)
						IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}

			Database->InsertAsset(IndexedAsset);
			CrgTouchedPaths.Add(IndexedAsset.PackagePath);
			PathsForScopedSentinels.Add(IndexedAsset.PackagePath);

			FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			if (ClassToIndexer.Contains(ClassName))
				PathsToDeepIndex.Add(IndexedAsset.PackagePath);
			break;
		}
		case EIndexChangeType::Updated:
		{
			FIndexedAsset IndexedAsset;
			IndexedAsset.PackagePath = Change.AssetData.PackageName.ToString();
			IndexedAsset.AssetName = Change.AssetData.AssetName.ToString();
			IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			// Module name resolution (same as Added)
			if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
			{
				for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
				{
					if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
					{
						IndexedAsset.ModuleName = PluginInfo.PluginName;
						break;
					}
				}
				if (IndexedAsset.ModuleName.IsEmpty())
				{
					int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
					if (SecondSlash > 1)
						IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}

			int64 AssetId = Database->GetAssetId(IndexedAsset.PackagePath);
			if (AssetId > 0)
			{
				Database->UpdateAssetMetadata(IndexedAsset);
				Database->DeleteChildDataForAsset(AssetId);
			}
			else
			{
				Database->InsertAsset(IndexedAsset);
			}
			CrgTouchedPaths.Add(IndexedAsset.PackagePath);
			PathsForScopedSentinels.Add(IndexedAsset.PackagePath);

			FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			if (ClassToIndexer.Contains(ClassName))
				PathsToDeepIndex.Add(IndexedAsset.PackagePath);
			break;
		}
		case EIndexChangeType::Removed:
		{
			FString Path = Change.AssetData.PackageName.ToString();
			Database->DeleteAssetByPath(Path);
			RemovedPaths.Add(Path);
			CrgTouchedPaths.Add(Path);
			break;
		}
		case EIndexChangeType::Renamed:
		{
			FString OldPackageName, OldAssetName;
			Change.OldObjectPath.Split(TEXT("."), &OldPackageName, &OldAssetName);
			FString NewPath = Change.AssetData.PackageName.ToString();
			FString NewAssetName = Change.AssetData.AssetName.ToString();
			CrgTouchedPaths.Add(OldPackageName);
			CrgTouchedPaths.Add(NewPath);
			PathsForScopedSentinels.Add(NewPath);

			if (Database->UpdateAssetPath(OldPackageName, NewPath, NewAssetName))
			{
				UE_LOG(LogMonolithIndex, Verbose, TEXT("Asset moved: %s -> %s"), *OldPackageName, *NewPath);
			}
			else
			{
				FIndexedAsset IndexedAsset;
				IndexedAsset.PackagePath = NewPath;
				IndexedAsset.AssetName = NewAssetName;
				IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
				Database->InsertAsset(IndexedAsset);
				PathsToDeepIndex.Add(NewPath);
			}
			break;
		}
		}
	}

	// Deep-index within same transaction
	if (PathsToDeepIndex.Num() > 0)
		ProcessDeepIndexQueue(PathsToDeepIndex);

	Database->CommitTransaction();

	// Sentinels after commit (they manage own transactions)
	if (PathsForScopedSentinels.Num() > 0 || RemovedPaths.Num() > 0)
		RunScopedSentinels(PathsForScopedSentinels, RemovedPaths);

	RefreshProjectCrgCacheForChangedAssets(Database.Get(), CrgTouchedPaths, TEXT("Live asset index changes processed"));
}
