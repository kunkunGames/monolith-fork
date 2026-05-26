#include "MonolithMeshModule.h"
#include "MonolithMeshInspectionActions.h"
#include "MonolithMeshPerformanceActions.h"
#include "MonolithMeshTechArtActions.h"
#include "MonolithMeshQualityActions.h"
#include "MonolithMeshValidationActions.h"
#include "MonolithLevelInstanceActions.h"
#include "MonolithHlodActions.h"
#include "MonolithActorMergeActions.h"
#include "MonolithMeshBulkFillAdapter.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Misc/CoreDelegates.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshOperationActions.h"
#include "MonolithMeshProceduralActions.h"
#include "MonolithMeshHandlePool.h"
#endif

#define LOCTEXT_NAMESPACE "FMonolithMeshModule"

void FMonolithMeshModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith — Mesh module disabled via settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithMesh"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMeshInspectionActions::RegisterActions(OwnedRegistry);
		FMonolithMeshPerformanceActions::RegisterActions(OwnedRegistry);
		FMonolithMeshTechArtActions::RegisterActions(OwnedRegistry);
		FMonolithMeshQualityActions::RegisterActions(OwnedRegistry);
		FMonolithMeshValidationActions::RegisterActions(OwnedRegistry);
		FMonolithLevelInstanceActions::RegisterActions(OwnedRegistry);
		FMonolithHlodActions::RegisterActions(OwnedRegistry);
		FMonolithActorMergeActions::RegisterActions(OwnedRegistry);
	});

#if WITH_GEOMETRYSCRIPT
	HandlePool = NewObject<UMonolithMeshHandlePool>();
	HandlePool->AddToRoot();
	HandlePool->Initialize();
	FMonolithMeshOperationActions::SetHandlePool(HandlePool);
	FMonolithMeshProceduralActions::SetHandlePool(HandlePool);
	Registry.RegisterOwnedActions(TEXT("MonolithMesh"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMeshOperationActions::RegisterActions(OwnedRegistry);
		FMonolithMeshProceduralActions::RegisterActions(OwnedRegistry);
	});

	FMonolithMeshTechArtActions::SetHandlePool(HandlePool);

	// Clean up handle pool on PreExit — before GC destroys UObjects.
	// ShutdownModule runs too late; by then the UObject array may be torn down.
	FCoreDelegates::OnPreExit.AddLambda([this]()
	{
		if (HandlePool && HandlePool->IsValidLowLevelFast())
		{
			HandlePool->Teardown();
			HandlePool->RemoveFromRoot();
			FMonolithMeshOperationActions::SetHandlePool(nullptr);
			FMonolithMeshProceduralActions::SetHandlePool(nullptr);
			FMonolithMeshTechArtActions::SetHandlePool(nullptr);
			HandlePool = nullptr;
		}
	});

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Mesh operations enabled (GeometryScript available)"));
#endif

	// Phase 5 Step 5 (MCP Ergonomics, 2026-05-11) — register the mesh adapter
	// OUTSIDE the WITH_GEOMETRYSCRIPT gate so bulk_fill is available regardless
	// of GeometryScript availability. SurfaceDataTable + ActorProperties
	// fill_kinds are reflection-bound, not GeometryScript-bound.
	FMonolithMeshBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Mesh module loaded (%d actions)"),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("mesh")));
}

void FMonolithMeshModule::ShutdownModule()
{
	// Handle pool cleanup happens in OnPreExit (before GC destroys UObjects).
	// By the time ShutdownModule runs, the UObject array may already be torn down.
	// Just null our pointer defensively.
#if WITH_GEOMETRYSCRIPT
	HandlePool = nullptr;
#endif

	FMonolithMeshBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithMesh"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMeshModule, MonolithMesh)
