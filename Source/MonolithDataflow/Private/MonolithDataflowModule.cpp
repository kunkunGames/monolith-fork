#include "MonolithDataflowModule.h"

#include "MonolithDataflowActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithDataflow);

void FMonolithDataflowModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithDataflowActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("dataflow"));
	UE_LOG(LogMonolithDataflow, Log, TEXT("MonolithDataflow: Loaded (%d actions)"), ActionCount);
}

void FMonolithDataflowModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("dataflow"));
}

IMPLEMENT_MODULE(FMonolithDataflowModule, MonolithDataflow)
