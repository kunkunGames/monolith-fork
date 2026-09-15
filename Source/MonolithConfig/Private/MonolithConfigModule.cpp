#include "MonolithConfigModule.h"
#include "MonolithConfigActions.h"
#include "MonolithLocalizationActions.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithConfigModule"

void FMonolithConfigModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Localization is read-only and stays available when config authoring is off.
	FMonolithLocalizationActions::RegisterActions(Registry);
	FMonolithActionExecutionGuard::Get().RegisterHandlerOwnedSourceControlActions(
		TEXT("localization"),
		{TEXT("set_target_text_search_directories")});

	if (!GetDefault<UMonolithSettings>()->bEnableConfig)
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith - Config actions disabled (%d localization actions still registered)"),
			Registry.GetNamespaceActionCount(TEXT("localization")));
		return;
	}

	FMonolithConfigActions::RegisterActions(Registry);
	UE_LOG(LogMonolith, Log, TEXT("Monolith - Config module loaded (%d config actions, %d localization actions)"),
		Registry.GetNamespaceActionCount(TEXT("config")),
		Registry.GetNamespaceActionCount(TEXT("localization")));
}

void FMonolithConfigModule::ShutdownModule()
{
	FMonolithLocalizationActions::ShutdownActions();
	FMonolithActionExecutionGuard::Get().UnregisterHandlerOwnedSourceControlActions(
		TEXT("localization"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("config"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("localization"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithConfigModule, MonolithConfig)
