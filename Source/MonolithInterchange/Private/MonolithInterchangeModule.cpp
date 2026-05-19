#include "MonolithInterchangeModule.h"

#include "MonolithInterchangeActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithInterchange);

void FMonolithInterchangeModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithInterchangeActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("interchange"));
	UE_LOG(LogMonolithInterchange, Log, TEXT("MonolithInterchange: Loaded (%d actions)"), ActionCount);
}

void FMonolithInterchangeModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("interchange"));
}

IMPLEMENT_MODULE(FMonolithInterchangeModule, MonolithInterchange)
