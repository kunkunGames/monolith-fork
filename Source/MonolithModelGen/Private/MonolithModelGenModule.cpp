#include "MonolithModelGenModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithMeshTechArtActions.h"

DEFINE_LOG_CATEGORY(LogMonolithModelGen);

void FMonolithModelGenModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithModelGen, Log, TEXT("Monolith — ModelGen module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithMeshTechArtActions::RegisterModelGenActions(Registry);
	UE_LOG(LogMonolithModelGen, Log, TEXT("Monolith — ModelGen module loaded (%d modelgen actions)"),
		Registry.GetNamespaceActionCount(TEXT("modelgen")));
}

void FMonolithModelGenModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("modelgen"));
}

IMPLEMENT_MODULE(FMonolithModelGenModule, MonolithModelGen)
