#include "MonolithConfigModule.h"
#include "MonolithConfigActions.h"
#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithConfigModule"

void FMonolithConfigModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableConfig) return;

	FMonolithConfigActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolith, Log, TEXT("Monolith - Config module loaded (%d config actions, %d localization actions)"),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("config")),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("localization")));
}

void FMonolithConfigModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("config"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("localization"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithConfigModule, MonolithConfig)
