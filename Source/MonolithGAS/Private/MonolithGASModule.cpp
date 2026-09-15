#include "MonolithGASModule.h"
#include "Modules/ModuleManager.h"
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
#include "MonolithGASDataAssetProfileActions.h"
#include "MonolithGASInputAssetActions.h"
#include "MonolithGASInputAuthoringActions.h"
#include "MonolithGASInspectActions.h"
#include "MonolithGASScaffoldActions.h"
#include "MonolithGASUIBindingActions.h"
#include "MonolithGASBulkFillAdapter.h"

DEFINE_LOG_CATEGORY(LogMonolithGAS);

void FMonolithGASModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	// Enhanced Input is engine-owned, not GAS. Register before the GAS gate.
	FMonolithGASInputAssetActions::RegisterActions(Registry);
	FMonolithGASInputAuthoringActions::RegisterActions(Registry);
	FMonolithDispatcherAnnotations InputAnnotations;
	InputAnnotations.bReadOnlyHint = false;
	InputAnnotations.bDestructiveHint = true;
	InputAnnotations.bIdempotentHint = false;
	InputAnnotations.Title = TEXT("Enhanced Input Assets");
	Registry.SetDispatcherAnnotations(TEXT("input"), InputAnnotations);

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableGAS)
	{
		const int32 InputActionCount = Registry.GetNamespaceActionCount(TEXT("input"));
		UE_LOG(LogMonolithGAS, Log,
			TEXT("MonolithGAS: GAS integration disabled in settings; %d input actions remain available"),
			InputActionCount);
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
		FMonolithGASDataAssetProfileActions::RegisterActions(OwnedRegistry);
		FMonolithGASInspectActions::RegisterActions(OwnedRegistry);
		FMonolithGASScaffoldActions::RegisterActions(OwnedRegistry);
		FMonolithGASUIBindingActions::RegisterActions(OwnedRegistry);
	});

	FMonolithGASBulkFillAdapter::Register();

	const int32 GasActionCount = Registry.GetNamespaceActionCount(TEXT("gas"));
	const int32 InputActionCount = Registry.GetNamespaceActionCount(TEXT("input"));
	const TCHAR* GbaStatus =
#if WITH_GBA
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(LogMonolithGAS, Log, TEXT("MonolithGAS: Loaded (%d gas actions, %d input actions, GBA=%s)"),
		GasActionCount, InputActionCount, GbaStatus);
}

void FMonolithGASModule::ShutdownModule()
{
	FMonolithGASBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithGAS"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("input"));
}

IMPLEMENT_MODULE(FMonolithGASModule, MonolithGAS)
