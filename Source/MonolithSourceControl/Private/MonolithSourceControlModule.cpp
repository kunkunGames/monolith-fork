#include "MonolithSourceControlModule.h"

#include "MonolithJsonUtils.h"
#include "MonolithSourceControlActions.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithSourceControlModule"

void FMonolithSourceControlModule::StartupModule()
{
	FMonolithSourceControlActions::RegisterActions();
	UE_LOG(LogMonolith, Log, TEXT("Monolith - SourceControl module loaded"));
}

void FMonolithSourceControlModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source_control"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithSourceControlModule, MonolithSourceControl)
