#include "MonolithIndexModule.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"
#include "Actions/ProjectActionRegistration.h"

#define LOCTEXT_NAMESPACE "FMonolithIndexModule"

void FMonolithIndexModule::StartupModule()
{
	UE_LOG(LogMonolithIndex, Verbose, TEXT("Monolith -- Index module loaded (19 actions, SQLite+FTS5)"));

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithIndex::FProjectActionRegistration::Register(Registry);
}

void FMonolithIndexModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("project"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("collection"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithIndexModule, MonolithIndex)
