#include "MonolithGameSettingsModule.h"

#include "MonolithGameSettingsActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithGameSettings);

void FMonolithGameSettingsModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithGameSettingsActions::RegisterActions(Registry);
	const int32 ActionCount = Registry.GetActions(TEXT("settings")).Num();
	UE_LOG(LogMonolithGameSettings, Log, TEXT("MonolithGameSettings: Loaded (%d actions)"), ActionCount);
}

void FMonolithGameSettingsModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("settings"));
}

IMPLEMENT_MODULE(FMonolithGameSettingsModule, MonolithGameSettings)
