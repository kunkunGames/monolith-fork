#include "MonolithMaterialModule.h"
#include "MonolithMaterialActions.h"
#include "MonolithSpecializedAssetActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithMaterialModule"

void FMonolithMaterialModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMaterial) return;

	FMonolithMaterialActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithSpecializedAssetActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolith, Log, TEXT("Monolith - Material module loaded (%d material actions, %d asset actions)"),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("material")),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("asset")));
}

void FMonolithMaterialModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("material"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("asset"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMaterialModule, MonolithMaterial)
