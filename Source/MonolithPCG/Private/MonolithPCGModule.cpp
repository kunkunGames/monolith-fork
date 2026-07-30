#include "MonolithPCGModule.h"

#include "MonolithPCGActions.h"
#include "MonolithPCGComponentActions.h"
#include "MonolithPCGGraphAuthoringActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithPCG);

void FMonolithPCGModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	FMonolithPCGGraphAuthoringActions::RegisterActions(Registry);
	FMonolithPCGComponentActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetActions(TEXT("pcg")).Num();
	UE_LOG(LogMonolithPCG, Log, TEXT("MonolithPCG: Loaded (%d actions)"), ActionCount);
}

void FMonolithPCGModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pcg"));
}

IMPLEMENT_MODULE(FMonolithPCGModule, MonolithPCG)
