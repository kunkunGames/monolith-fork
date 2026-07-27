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
	FMonolithGASInputAssetActions::RegisterActions(Registry);

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableGAS)
	{
		const int32 InputActionCount = Registry.GetActions(TEXT("input")).Num();
		UE_LOG(LogMonolithGAS, Log,
			TEXT("MonolithGAS: GAS integration disabled in settings; %d input asset actions remain available"),
			InputActionCount);
		return;
	}

	FMonolithGASAbilityActions::RegisterActions(Registry);
	FMonolithGASAttributeActions::RegisterActions(Registry);
	FMonolithGASEffectActions::RegisterActions(Registry);
	FMonolithGASASCActions::RegisterActions(Registry);
	FMonolithGASTagActions::RegisterActions(Registry);
	FMonolithGASCueActions::RegisterActions(Registry);
	FMonolithGASTargetActions::RegisterActions(Registry);
	FMonolithGASInputActions::RegisterActions(Registry);
	FMonolithGASInspectActions::RegisterActions(Registry);
	FMonolithGASScaffoldActions::RegisterActions(Registry);
	FMonolithGASUIBindingActions::RegisterActions(Registry);

	// Phase 2 (MCP Ergonomics) — register the gas adapter on the central
	// FMonolithBulkFillRegistry. The Register() call ALWAYS runs (H5 invariant)
	// regardless of WITH_GBA so `monolith_discover("gas")` action surface stays
	// identical across dev + release builds; the adapter BODY switches on
	// WITH_GBA and returns a clean "GAS not available" error when the optional
	// dep is absent. See MonolithGASBulkFillAdapter.cpp for the split.
	FMonolithGASBulkFillAdapter::Register();

	const int32 GasActionCount = Registry.GetActions(TEXT("gas")).Num();
	const int32 InputActionCount = Registry.GetActions(TEXT("input")).Num();
	const TCHAR* GbaStatus =
#if WITH_GBA
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(
		LogMonolithGAS,
		Log,
		TEXT("MonolithGAS: Loaded (%d gas actions, %d input actions, GBA=%s)"),
		GasActionCount,
		InputActionCount,
		GbaStatus);
}

void FMonolithGASModule::ShutdownModule()
{
	FMonolithGASBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("gas"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("input"));
}

IMPLEMENT_MODULE(FMonolithGASModule, MonolithGAS)
