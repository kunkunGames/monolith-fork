#include "MonolithModelGenModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithModelGenActions.h"

DEFINE_LOG_CATEGORY(LogMonolithModelGen);

void FMonolithModelGenModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithModelGen, Log, TEXT("Monolith — ModelGen module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithModelGen"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithModelGenActions::RegisterActions(OwnedRegistry);
	});
	UE_LOG(LogMonolithModelGen, Log, TEXT("Monolith — ModelGen module loaded (%d modelgen actions)"),
		Registry.GetNamespaceActionCount(TEXT("modelgen")));
}

void FMonolithModelGenModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithModelGen"));
}

IMPLEMENT_MODULE(FMonolithModelGenModule, MonolithModelGen)
