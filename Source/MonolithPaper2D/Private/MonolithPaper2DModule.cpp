#include "MonolithPaper2DModule.h"

#include "MonolithPaper2DActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithPaper2D);

void FMonolithPaper2DModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPaper2DActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("paper2d"));
	UE_LOG(LogMonolithPaper2D, Log, TEXT("MonolithPaper2D: Loaded (%d actions)"), ActionCount);
}

void FMonolithPaper2DModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("paper2d"));
}

IMPLEMENT_MODULE(FMonolithPaper2DModule, MonolithPaper2D)
