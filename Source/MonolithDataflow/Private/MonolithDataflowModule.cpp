#include "MonolithDataflowModule.h"

#include "MonolithDataflowActions.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithDataflowModule"

void FMonolithDataflowModule::StartupModule()
{
	FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithDataflowModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("dataflow"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithDataflowModule, MonolithDataflow)
