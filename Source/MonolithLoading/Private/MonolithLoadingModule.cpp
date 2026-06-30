#include "MonolithLoadingModule.h"

#include "MonolithLoadingActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithLoading);

void FMonolithLoadingModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLoadingActions::RegisterActions(Registry);
	const int32 ActionCount = Registry.GetActions(TEXT("loading")).Num();
	UE_LOG(LogMonolithLoading, Log, TEXT("MonolithLoading: Loaded (%d actions)"), ActionCount);
}

void FMonolithLoadingModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("loading"));
}

IMPLEMENT_MODULE(FMonolithLoadingModule, MonolithLoading)
