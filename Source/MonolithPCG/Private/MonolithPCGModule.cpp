#include "MonolithPCGModule.h"

#include "MonolithPCGActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithPCG);

void FMonolithPCGModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("pcg"));
	UE_LOG(LogMonolithPCG, Log, TEXT("MonolithPCG: Loaded (%d actions)"), ActionCount);
}

void FMonolithPCGModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pcg"));
}

IMPLEMENT_MODULE(FMonolithPCGModule, MonolithPCG)
