#include "MonolithChaosFractureModule.h"

#include "MonolithChaosFractureActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithChaosFracture);

void FMonolithChaosFractureModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithChaosFractureActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("chaos_fracture"));
	UE_LOG(LogMonolithChaosFracture, Log, TEXT("MonolithChaosFracture: Loaded (%d actions)"), ActionCount);
}

void FMonolithChaosFractureModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("chaos_fracture"));
}

IMPLEMENT_MODULE(FMonolithChaosFractureModule, MonolithChaosFracture)
