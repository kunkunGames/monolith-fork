#include "MonolithWorldConditionsModule.h"

#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "MonolithWorldConditionsActions.h"

DEFINE_LOG_CATEGORY(LogMonolithWorldConditions);

void FMonolithWorldConditionsModule::StartupModule()
{
	FMonolithWorldConditionsActions::RegisterActions(FMonolithToolRegistry::Get());

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const bool bEnabled = Settings && Settings->bEnableWorldConditionsInspection;
	const int32 ActionCount = FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("world_conditions"));
	UE_LOG(LogMonolithWorldConditions, Log, TEXT("MonolithWorldConditions: Loaded (%d actions, enabled=%s)"),
		ActionCount,
		bEnabled ? TEXT("true") : TEXT("false"));
}

void FMonolithWorldConditionsModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("world_conditions"));
}

IMPLEMENT_MODULE(FMonolithWorldConditionsModule, MonolithWorldConditions)
