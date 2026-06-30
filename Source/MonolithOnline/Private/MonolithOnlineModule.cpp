#include "MonolithOnlineModule.h"

#include "MonolithOnlineActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithOnline);

void FMonolithOnlineModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithOnlineActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("online"));
	UE_LOG(LogMonolithOnline, Log, TEXT("MonolithOnline: Loaded (%d actions)"), ActionCount);
}

void FMonolithOnlineModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("online"));
}

IMPLEMENT_MODULE(FMonolithOnlineModule, MonolithOnline)
