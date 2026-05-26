#include "MonolithGASModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithGASAbilityActions.h"
#include "MonolithGASAttributeActions.h"
#include "MonolithGASEffectActions.h"
#include "MonolithGASASCActions.h"
#include "MonolithGASTagActions.h"
#include "MonolithGASCueActions.h"
#include "MonolithGASTargetActions.h"
#include "MonolithGASInputActions.h"
#include "MonolithGASInputAssetActions.h"
#include "MonolithGASInspectActions.h"
#include "MonolithGASScaffoldActions.h"
#include "MonolithGASUIBindingActions.h"
#include "MonolithGASBulkFillAdapter.h"

DEFINE_LOG_CATEGORY(LogMonolithGAS);

void FMonolithGASModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithGAS"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithGASInputAssetActions::RegisterActions(OwnedRegistry);
	});

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableGAS)
	{
		UE_LOG(LogMonolithGAS, Log,
			TEXT("MonolithGAS: GAS integration disabled in settings; input asset actions remain available"));
		return;
	}

	Registry.RegisterOwnedActions(TEXT("MonolithGAS"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithGASAbilityActions::RegisterActions(OwnedRegistry);
		FMonolithGASAttributeActions::RegisterActions(OwnedRegistry);
		FMonolithGASEffectActions::RegisterActions(OwnedRegistry);
		FMonolithGASASCActions::RegisterActions(OwnedRegistry);
		FMonolithGASTagActions::RegisterActions(OwnedRegistry);
		FMonolithGASCueActions::RegisterActions(OwnedRegistry);
		FMonolithGASTargetActions::RegisterActions(OwnedRegistry);
		FMonolithGASInputActions::RegisterActions(OwnedRegistry);
		FMonolithGASInspectActions::RegisterActions(OwnedRegistry);
		FMonolithGASScaffoldActions::RegisterActions(OwnedRegistry);
		FMonolithGASUIBindingActions::RegisterActions(OwnedRegistry);
	});

	FMonolithGASBulkFillAdapter::Register();

	int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("gas"));
	const TCHAR* GbaStatus =
#if WITH_GBA
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(LogMonolithGAS, Log, TEXT("MonolithGAS: Loaded (%d actions, GBA=%s)"), ActionCount, GbaStatus);
}

void FMonolithGASModule::ShutdownModule()
{
	FMonolithGASBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithGAS"));
}

IMPLEMENT_MODULE(FMonolithGASModule, MonolithGAS)
