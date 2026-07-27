#include "MonolithSourceModule.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithSourceActions.h"
#include "MonolithSourceContextActions.h"
#include "MonolithSourceResourceProvider.h"
#include "MonolithSourceSubsystem.h"
#include "MonolithResourceRegistry.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FMonolithSourceModule"

static FAutoConsoleCommand GMonolithStartIndexingCommand(
	TEXT("Monolith.StartIndexing"),
	TEXT("Persistently enable and start Monolith source and asset indexing."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithSourceModule::StartIndexingCommand));

static FAutoConsoleCommand GMonolithStopIndexingCommand(
	TEXT("Monolith.StopIndexing"),
	TEXT("Persistently disable Monolith source and asset indexing; active jobs drain safely."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithSourceModule::StopIndexingCommand));

void FMonolithSourceModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableSource) return;

	FMonolithSourceActions::RegisterAll();
	FMonolithSourceContextActions::RegisterAll();

	// Register the source-namespace MCP resource provider (P3b), reusing the existing
	// bEnableMcpResources gate (no new flag). When resources are disabled the provider is
	// never created and the resource registry is untouched.
	if (GetDefault<UMonolithSettings>()->bEnableMcpResources)
	{
		SourceResourceProvider = MakeShared<FMonolithSourceResourceProvider>();
		FMonolithResourceRegistry::Get().RegisterProvider(SourceResourceProvider.ToSharedRef());
	}

	UE_LOG(LogMonolith, Log, TEXT("Monolith - Source module loaded (source + bridge actions)"));
}

void FMonolithSourceModule::ShutdownModule()
{
	if (SourceResourceProvider.IsValid())
	{
		FMonolithResourceRegistry::Get().UnregisterProvider(SourceResourceProvider.ToSharedRef());
		SourceResourceProvider.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("bridge"));
}

void FMonolithSourceModule::StartIndexingCommand()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const bool bAssetPolicyEnabled = !Settings || Settings->bEnableIndex;
	const bool bSourcePolicyEnabled = !Settings || Settings->bEnableSource;
	if (!bAssetPolicyEnabled && !bSourcePolicyEnabled)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartIndexing: both bEnableIndex and bEnableSource are false; activation was not changed"));
		return;
	}

	FString Error;
	if (!UMonolithSettings::SetIndexingActivated(true, &Error))
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.StartIndexing: %s"), *Error);
		return;
	}

	if (!GEditor)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartIndexing: activation persisted, but GEditor is unavailable; indexing will start on the next editor launch"));
		return;
	}

	bool bSourceAccepted = false;
	bool bAssetAccepted = false;

	if (bSourcePolicyEnabled)
	{
		if (UMonolithSourceSubsystem* Source =
			GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>())
		{
			Source->SetAutomaticIndexingEnabled(true);
			bSourceAccepted = Source->StartPreferredIndex();
		}
	}

	if (bAssetPolicyEnabled)
	{
		if (UMonolithIndexSubsystem* Asset =
			GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
		{
			Asset->SetAutomaticIndexingEnabled(true);
			bAssetAccepted = Asset->StartPreferredIndex();
		}
	}

	UE_LOG(LogMonolith, Log,
		TEXT("Monolith.StartIndexing: activation enabled persistently (source_accepted=%s asset_accepted=%s state=%s)"),
		bSourceAccepted ? TEXT("true") : TEXT("false"),
		bAssetAccepted ? TEXT("true") : TEXT("false"),
		*UMonolithSettings::GetUserActivationPath());
}

void FMonolithSourceModule::StopIndexingCommand()
{
	FString Error;
	const bool bPersisted = UMonolithSettings::SetIndexingActivated(false, &Error);

	bool bSourceDraining = false;
	bool bAssetDraining = false;
	if (GEditor)
	{
		if (UMonolithSourceSubsystem* Source =
			GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>())
		{
			bSourceDraining = Source->IsIndexing();
			Source->SetAutomaticIndexingEnabled(false);
		}
		if (UMonolithIndexSubsystem* Asset =
			GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
		{
			bAssetDraining = Asset->IsIndexing();
			Asset->SetAutomaticIndexingEnabled(false);
		}
	}

	if (bPersisted)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StopIndexing: activation disabled persistently (source_draining=%s asset_draining=%s state=%s)"),
			bSourceDraining ? TEXT("true") : TEXT("false"),
			bAssetDraining ? TEXT("true") : TEXT("false"),
			*UMonolithSettings::GetUserActivationPath());
	}
	else
	{
		UE_LOG(LogMonolith, Error,
			TEXT("Monolith.StopIndexing: automatic hooks stopped for this process, but persistent deactivation failed: %s"),
			*Error);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithSourceModule, MonolithSource)
