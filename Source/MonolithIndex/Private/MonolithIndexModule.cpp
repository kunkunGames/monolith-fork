#include "MonolithIndexModule.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"
#include "Actions/ProjectActionRegistration.h"

#define LOCTEXT_NAMESPACE "FMonolithIndexModule"

void FMonolithIndexModule::StartupModule()
{
	UE_LOG(LogMonolithIndex, Verbose, TEXT("Monolith -- Index module loaded (19 actions, SQLite+FTS5)"));

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithIndex"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		MonolithIndex::FProjectActionRegistration::Register(OwnedRegistry);
	});
}

void FMonolithIndexModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithIndex"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithIndexModule, MonolithIndex)
