#include "MonolithNDisplayModule.h"

#include "MonolithNDisplayActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithNDisplay);

void FMonolithNDisplayModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithNDisplayActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("ndisplay"));
	UE_LOG(LogMonolithNDisplay, Log, TEXT("MonolithNDisplay: Loaded (%d actions)"), ActionCount);
}

void FMonolithNDisplayModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("ndisplay"));
}

IMPLEMENT_MODULE(FMonolithNDisplayModule, MonolithNDisplay)
