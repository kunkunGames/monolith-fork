#include "MonolithMeshModule.h"
#include "MonolithMeshInspectionActions.h"
#include "MonolithMeshPerformanceActions.h"
#include "MonolithMeshTechArtActions.h"
#include "MonolithMeshQualityActions.h"
#include "MonolithLevelInstanceActions.h"
#include "MonolithHlodActions.h"
#include "MonolithActorMergeActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Misc/CoreDelegates.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshOperationActions.h"
#include "MonolithMeshProceduralActions.h"
#include "MonolithMeshBuildingActions.h"
#include "MonolithMeshFacadeActions.h"
#include "MonolithMeshRoofActions.h"
#include "MonolithMeshCityBlockActions.h"
#include "MonolithMeshTerrainActions.h"
#include "MonolithMeshArchFeatureActions.h"
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

	FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithMeshPerformanceActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithMeshTechArtActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithMeshQualityActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithLevelInstanceActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithHlodActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithActorMergeActions::RegisterActions(FMonolithToolRegistry::Get());

#if WITH_GEOMETRYSCRIPT
	HandlePool = NewObject<UMonolithMeshHandlePool>();
	HandlePool->AddToRoot();
	HandlePool->Initialize();
	FMonolithMeshOperationActions::SetHandlePool(HandlePool);
	FMonolithMeshOperationActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithMeshProceduralActions::SetHandlePool(HandlePool);
	FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());

	// --- Town gen GeometryScript actions (experimental, off by default) ---
	if (GetDefault<UMonolithSettings>()->bEnableProceduralTownGen)
	{
		FMonolithMeshBuildingActions::SetHandlePool(HandlePool);
		FMonolithMeshBuildingActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMeshFacadeActions::SetHandlePool(HandlePool);
		FMonolithMeshFacadeActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMeshRoofActions::SetHandlePool(HandlePool);
		FMonolithMeshRoofActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMeshCityBlockActions::SetHandlePool(HandlePool);
		FMonolithMeshCityBlockActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMeshTerrainActions::SetHandlePool(HandlePool);
		FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMeshArchFeatureActions::SetHandlePool(HandlePool);
		FMonolithMeshArchFeatureActions::RegisterActions(FMonolithToolRegistry::Get());
	}

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
			FMonolithMeshBuildingActions::SetHandlePool(nullptr);
			FMonolithMeshFacadeActions::SetHandlePool(nullptr);
			FMonolithMeshRoofActions::SetHandlePool(nullptr);
			FMonolithMeshCityBlockActions::SetHandlePool(nullptr);
			FMonolithMeshTerrainActions::SetHandlePool(nullptr);
			FMonolithMeshArchFeatureActions::SetHandlePool(nullptr);
			FMonolithMeshTechArtActions::SetHandlePool(nullptr);
			HandlePool = nullptr;
		}
	});

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Mesh operations enabled (GeometryScript available)"));
#endif

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

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("mesh"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("scene"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("leveldesign"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("worldgen"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("modelgen"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("level_instance"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("hlod"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMeshModule, MonolithMesh)
