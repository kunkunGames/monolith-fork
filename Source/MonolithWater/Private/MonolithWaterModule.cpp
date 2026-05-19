#include "MonolithWaterModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithWaterActions.h"

DEFINE_LOG_CATEGORY(LogMonolithWater);

void FMonolithWaterModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithWaterActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("water"));
	UE_LOG(LogMonolithWater, Log, TEXT("MonolithWater: Loaded (%d actions)"), ActionCount);
}

void FMonolithWaterModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("water"));
}

IMPLEMENT_MODULE(FMonolithWaterModule, MonolithWater)
