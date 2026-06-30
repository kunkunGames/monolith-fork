#include "MonolithGameplayMessageModule.h"

#include "MonolithGameplayMessageActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithGameplayMessage);

void FMonolithGameplayMessageModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithGameplayMessageActions::RegisterActions(Registry);
	const int32 ActionCount = Registry.GetActions(TEXT("gameplay_message")).Num();
	UE_LOG(LogMonolithGameplayMessage, Log, TEXT("MonolithGameplayMessage: Loaded (%d actions)"), ActionCount);
}

void FMonolithGameplayMessageModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("gameplay_message"));
}

IMPLEMENT_MODULE(FMonolithGameplayMessageModule, MonolithGameplayMessage)
