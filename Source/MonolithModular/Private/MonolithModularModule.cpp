#include "MonolithModularModule.h"

#include "MonolithModularActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithModular);

void FMonolithModularModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithModularActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("modular"));
	UE_LOG(LogMonolithModular, Log, TEXT("MonolithModular: Loaded (%d actions)"), ActionCount);
}

void FMonolithModularModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("modular"));
}

IMPLEMENT_MODULE(FMonolithModularModule, MonolithModular)
