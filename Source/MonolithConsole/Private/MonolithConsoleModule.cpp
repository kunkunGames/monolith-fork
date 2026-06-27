#include "MonolithConsoleModule.h"
#include "MonolithConsoleActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithConsoleModule"

void FMonolithConsoleModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (Settings && !Settings->bEnableConsole)
	{
		return;
	}

	FMonolithToolRegistry::Get().RegisterOwnedActions(TEXT("MonolithConsole"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithConsoleActions::RegisterActions(Registry);
		});

	UE_LOG(LogMonolith, Log, TEXT("Monolith - Console module loaded (%d console actions)"),
		FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("console")));
}

void FMonolithConsoleModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithConsole"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithConsoleModule, MonolithConsole)
