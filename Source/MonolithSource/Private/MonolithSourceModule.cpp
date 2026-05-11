#include "MonolithSourceModule.h"
#include "MonolithSourceActions.h"
#include "MonolithSourceContextActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"

#define LOCTEXT_NAMESPACE "FMonolithSourceModule"

void FMonolithSourceModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableSource) return;

	FMonolithSourceActions::RegisterAll();
	FMonolithSourceContextActions::RegisterAll();
	UE_LOG(LogMonolith, Log, TEXT("Monolith - Source module loaded (source + context actions)"));
}

void FMonolithSourceModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("context"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithSourceModule, MonolithSource)
