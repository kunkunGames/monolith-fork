#include "MonolithIndexModule.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"
#include "Actions/ProjectActionRegistration.h"

#define LOCTEXT_NAMESPACE "FMonolithIndexModule"

void FMonolithIndexModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithIndex"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		MonolithIndex::FProjectActionRegistration::Register(OwnedRegistry);
	});

	UE_LOG(LogMonolithIndex, Verbose, TEXT("Monolith -- Index module loaded (%d project actions, %d collection actions, SQLite+FTS5)"),
		Registry.GetNamespaceActionCount(TEXT("project")),
		Registry.GetNamespaceActionCount(TEXT("collection")));
}

void FMonolithIndexModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithIndex"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithIndexModule, MonolithIndex)
