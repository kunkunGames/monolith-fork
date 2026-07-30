#include "MonolithGameplayMessageModule.h"

#include "MonolithGameplayMessageActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_MODULE(FMonolithGameplayMessageModule, MonolithGameplayMessage)

void FMonolithGameplayMessageModule::StartupModule()
{
	FMonolithGameplayMessageActions::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithGameplayMessageModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("gameplay_message"));
}
