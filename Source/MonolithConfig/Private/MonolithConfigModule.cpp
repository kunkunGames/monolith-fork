#include "MonolithConfigModule.h"
#include "MonolithConfigActions.h"
#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithConfigModule"

void FMonolithConfigModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLocalizationActions::RegisterActions(Registry);

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableConfig)
	{
		UE_LOG(
			LogMonolith,
			Log,
			TEXT("Monolith - Config integration disabled; %d localization actions remain available"),
			Registry.GetActions(TEXT("localization")).Num());
		return;
	}

	FMonolithConfigActions::RegisterActions(Registry);
	UE_LOG(
		LogMonolith,
		Log,
		TEXT("Monolith - Config module loaded (%d config actions, %d localization actions)"),
		Registry.GetActions(TEXT("config")).Num(),
		Registry.GetActions(TEXT("localization")).Num());
}

void FMonolithConfigModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("config"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("localization"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithConfigModule, MonolithConfig)
