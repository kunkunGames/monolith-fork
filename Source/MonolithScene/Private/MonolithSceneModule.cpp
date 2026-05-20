#include "MonolithSceneModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshSpatialActions.h"
#include "MonolithMeshVolumeActions.h"
#include "MonolithMeshLightingActions.h"
#include "MonolithMeshDecalActions.h"
#include "MonolithMeshSpatialRegistry.h"
#include "MonolithMeshAutoVolumeActions.h"
#include "MonolithMeshDebugViewActions.h"

DEFINE_LOG_CATEGORY(LogMonolithScene);

void FMonolithSceneModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithScene, Log, TEXT("Monolith — Scene module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithScene"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMeshSceneActions::RegisterActions(OwnedRegistry);
		FMonolithMeshSpatialActions::RegisterActions(OwnedRegistry);
		FMonolithMeshVolumeActions::RegisterActions(OwnedRegistry);
		FMonolithMeshLightingActions::RegisterActions(OwnedRegistry);
		FMonolithMeshDecalActions::RegisterActions(OwnedRegistry);
	});

	if (GetDefault<UMonolithSettings>()->bEnableProceduralTownGen)
	{
		Registry.RegisterOwnedActions(TEXT("MonolithScene"), [](FMonolithToolRegistry& OwnedRegistry)
		{
			FMonolithMeshSpatialRegistry::RegisterActions(OwnedRegistry);
			FMonolithMeshAutoVolumeActions::RegisterActions(OwnedRegistry);
			FMonolithMeshDebugViewActions::RegisterActions(OwnedRegistry);
		});
	}
	UE_LOG(LogMonolithScene, Log, TEXT("Monolith — MonolithScene module loaded (%d scene actions)"),
		Registry.GetNamespaceActionCount(TEXT("scene")));
}

void FMonolithSceneModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithScene"));
}

IMPLEMENT_MODULE(FMonolithSceneModule, MonolithScene)
