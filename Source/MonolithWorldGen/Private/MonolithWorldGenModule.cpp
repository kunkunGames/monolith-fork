#include "MonolithWorldGenModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithMeshBlockoutActions.h"
#include "MonolithMeshTemplateActions.h"
#include "MonolithMeshPresetActions.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithMeshFloorPlanGenerator.h"
#include "MonolithMeshFurnishingActions.h"
#include "MonolithMeshBuildingValidationActions.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshBuildingActions.h"
#include "MonolithMeshFacadeActions.h"
#include "MonolithMeshRoofActions.h"
#include "MonolithMeshCityBlockActions.h"
#include "MonolithMeshTerrainActions.h"
#include "MonolithMeshArchFeatureActions.h"
#include "MonolithMeshHandlePool.h"
#include "Misc/CoreDelegates.h"
#endif

DEFINE_LOG_CATEGORY(LogMonolithWorldGen);

void FMonolithWorldGenModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithWorldGen, Log, TEXT("Monolith — WorldGen module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithWorldGen"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMeshBlockoutActions::RegisterActions(OwnedRegistry);
		FMonolithMeshTemplateActions::RegisterActions(OwnedRegistry);
		FMonolithMeshPresetActions::RegisterActions(OwnedRegistry);
		FMonolithMeshContextPropActions::RegisterActions(OwnedRegistry);
	});

	if (GetDefault<UMonolithSettings>()->bEnableProceduralTownGen)
	{
#if WITH_GEOMETRYSCRIPT
		HandlePool = NewObject<UMonolithMeshHandlePool>();
		HandlePool->AddToRoot();
		HandlePool->Initialize();
		FMonolithMeshBuildingActions::SetHandlePool(HandlePool);
		FMonolithMeshFacadeActions::SetHandlePool(HandlePool);
		FMonolithMeshRoofActions::SetHandlePool(HandlePool);
		FMonolithMeshCityBlockActions::SetHandlePool(HandlePool);
		FMonolithMeshTerrainActions::SetHandlePool(HandlePool);
		FMonolithMeshArchFeatureActions::SetHandlePool(HandlePool);

		Registry.RegisterOwnedActions(TEXT("MonolithWorldGen"), [](FMonolithToolRegistry& OwnedRegistry)
		{
			FMonolithMeshFloorPlanGenerator::RegisterActions(OwnedRegistry);
			FMonolithMeshFurnishingActions::RegisterActions(OwnedRegistry);
			FMonolithMeshBuildingValidationActions::RegisterActions(OwnedRegistry);
			FMonolithMeshBuildingActions::RegisterActions(OwnedRegistry);
			FMonolithMeshFacadeActions::RegisterActions(OwnedRegistry);
			FMonolithMeshRoofActions::RegisterActions(OwnedRegistry);
			FMonolithMeshCityBlockActions::RegisterActions(OwnedRegistry);
			FMonolithMeshTerrainActions::RegisterActions(OwnedRegistry);
			FMonolithMeshArchFeatureActions::RegisterActions(OwnedRegistry);
		});

		FCoreDelegates::OnPreExit.AddLambda([this]()
		{
			if (HandlePool && HandlePool->IsValidLowLevelFast())
			{
				HandlePool->Teardown();
				HandlePool->RemoveFromRoot();
				FMonolithMeshBuildingActions::SetHandlePool(nullptr);
				FMonolithMeshFacadeActions::SetHandlePool(nullptr);
				FMonolithMeshRoofActions::SetHandlePool(nullptr);
				FMonolithMeshCityBlockActions::SetHandlePool(nullptr);
				FMonolithMeshTerrainActions::SetHandlePool(nullptr);
				FMonolithMeshArchFeatureActions::SetHandlePool(nullptr);
				HandlePool = nullptr;
			}
		});
#else
		Registry.RegisterOwnedActions(TEXT("MonolithWorldGen"), [](FMonolithToolRegistry& OwnedRegistry)
		{
			FMonolithMeshFloorPlanGenerator::RegisterActions(OwnedRegistry);
			FMonolithMeshFurnishingActions::RegisterActions(OwnedRegistry);
			FMonolithMeshBuildingValidationActions::RegisterActions(OwnedRegistry);
		});
#endif
	}
	UE_LOG(LogMonolithWorldGen, Log, TEXT("Monolith — MonolithWorldGen module loaded (%d worldgen actions)"),
		Registry.GetNamespaceActionCount(TEXT("worldgen")));
}

void FMonolithWorldGenModule::ShutdownModule()
{
#if WITH_GEOMETRYSCRIPT
	HandlePool = nullptr;
#endif
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithWorldGen"));
}

IMPLEMENT_MODULE(FMonolithWorldGenModule, MonolithWorldGen)
