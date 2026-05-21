#include "MonolithImageGenModule.h"

#include "MonolithImageGenActions.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithImageGen);

void FMonolithImageGenModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableImageGen)
	{
		UE_LOG(LogMonolithImageGen, Log, TEXT("Monolith - ImageGen module disabled via imagegen settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithImageGen"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithImageGenActions::RegisterActions(OwnedRegistry);
	});

	UE_LOG(LogMonolithImageGen, Log, TEXT("Monolith - ImageGen module loaded (%d imagegen actions)"),
		Registry.GetNamespaceActionCount(TEXT("imagegen")));
}

void FMonolithImageGenModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithImageGen"));
}

IMPLEMENT_MODULE(FMonolithImageGenModule, MonolithImageGen)
