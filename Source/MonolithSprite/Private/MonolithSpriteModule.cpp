#include "MonolithSpriteModule.h"

#include "MonolithSettings.h"
#include "MonolithSpriteActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithSprite);

void FMonolithSpriteModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableSprite)
	{
		UE_LOG(LogMonolithSprite, Log, TEXT("Monolith - Sprite module disabled via settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithSprite"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithSpriteActions::RegisterActions(OwnedRegistry);
	});

	UE_LOG(LogMonolithSprite, Log, TEXT("Monolith - Sprite module loaded (%d sprite actions)"),
		Registry.GetNamespaceActionCount(TEXT("sprite")));
}

void FMonolithSpriteModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithSprite"));
}

IMPLEMENT_MODULE(FMonolithSpriteModule, MonolithSprite)
