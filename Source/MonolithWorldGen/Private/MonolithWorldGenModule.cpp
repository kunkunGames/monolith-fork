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

DEFINE_LOG_CATEGORY(LogMonolithWorldGen);

void FMonolithWorldGenModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithWorldGen, Log, TEXT("Monolith — WorldGen module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithMeshBlockoutActions::RegisterActions(Registry);
	FMonolithMeshTemplateActions::RegisterActions(Registry);
	FMonolithMeshPresetActions::RegisterActions(Registry);
	FMonolithMeshContextPropActions::RegisterActions(Registry);

	if (GetDefault<UMonolithSettings>()->bEnableProceduralTownGen)
	{
		FMonolithMeshFloorPlanGenerator::RegisterActions(Registry);
		FMonolithMeshFurnishingActions::RegisterActions(Registry);
		FMonolithMeshBuildingValidationActions::RegisterActions(Registry);
	}
	UE_LOG(LogMonolithWorldGen, Log, TEXT("Monolith — MonolithWorldGen module loaded (%d worldgen actions)"),
		Registry.GetNamespaceActionCount(TEXT("worldgen")));
}

void FMonolithWorldGenModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("worldgen"));
}

IMPLEMENT_MODULE(FMonolithWorldGenModule, MonolithWorldGen)
