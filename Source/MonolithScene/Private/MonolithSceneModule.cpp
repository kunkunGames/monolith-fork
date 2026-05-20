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
	FMonolithMeshSceneActions::RegisterActions(Registry);
	FMonolithMeshSpatialActions::RegisterActions(Registry);
	FMonolithMeshVolumeActions::RegisterActions(Registry);
	FMonolithMeshLightingActions::RegisterActions(Registry);
	FMonolithMeshDecalActions::RegisterActions(Registry);

	if (GetDefault<UMonolithSettings>()->bEnableProceduralTownGen)
	{
		FMonolithMeshSpatialRegistry::RegisterActions(Registry);
		FMonolithMeshAutoVolumeActions::RegisterActions(Registry);
		FMonolithMeshDebugViewActions::RegisterActions(Registry);
	}
	UE_LOG(LogMonolithScene, Log, TEXT("Monolith — MonolithScene module loaded (%d scene actions)"),
		Registry.GetNamespaceActionCount(TEXT("scene")));
}

void FMonolithSceneModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("scene"));
}

IMPLEMENT_MODULE(FMonolithSceneModule, MonolithScene)
