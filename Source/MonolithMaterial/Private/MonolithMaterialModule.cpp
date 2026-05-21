#include "MonolithMaterialModule.h"
#include "MonolithMaterialActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithMaterialModule"

void FMonolithMaterialModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMaterial) return;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithMaterial"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMaterialActions::RegisterActions(OwnedRegistry);
	});
	UE_LOG(LogMonolith, Log, TEXT("Monolith - Material module loaded (%d material actions)"),
		Registry.GetNamespaceActionCount(TEXT("material")));
}

void FMonolithMaterialModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithMaterial"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMaterialModule, MonolithMaterial)
