#include "MonolithEditorModule.h"
#include "MonolithEditorActions.h"
#include "MonolithEditorMapActions.h"
#include "MonolithEditorSelectionActions.h"
#include "MonolithEditorLevelMetadataActions.h"
#include "MonolithEditorCrashActions.h"
#include "MonolithSettingsCustomization.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "PropertyEditorModule.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "FMonolithEditorModule"

namespace
{
class FMonolithPieTransactionBufferGuard
{
public:
	void Register()
	{
		if (bRegistered)
		{
			return;
		}

		FEditorDelegates::PreBeginPIE.AddRaw(this, &FMonolithPieTransactionBufferGuard::HandlePreBeginPIE);
		FEditorDelegates::EndPIE.AddRaw(this, &FMonolithPieTransactionBufferGuard::HandleEndPIE);
		FCoreUObjectDelegates::PreLoadMap.AddRaw(this, &FMonolithPieTransactionBufferGuard::HandlePreLoadMap);
		bRegistered = true;
	}

	void Unregister()
	{
		if (!bRegistered)
		{
			return;
		}

		FEditorDelegates::PreBeginPIE.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);
		FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
		bRegistered = false;
	}

private:
	static void ResetTransactionBuffer(const FText& Reason)
	{
		if (GEditor)
		{
			GEditor->ResetTransaction(Reason);
		}
	}

	void HandlePreBeginPIE(bool /*bIsSimulating*/)
	{
		ResetTransactionBuffer(
			NSLOCTEXT("MonolithEditor", "ResetTransactionsBeforePIE", "Reset transactions before PIE"));
	}

	void HandleEndPIE(bool /*bIsSimulating*/)
	{
		ResetTransactionBuffer(
			NSLOCTEXT("MonolithEditor", "ResetTransactionsAfterPIE", "Reset transactions after PIE"));
	}

	void HandlePreLoadMap(const FString& /*MapName*/)
	{
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			ResetTransactionBuffer(
				NSLOCTEXT("MonolithEditor", "ResetTransactionsBeforePIEMapLoad", "Reset transactions before PIE map load"));
		}
	}

	bool bRegistered = false;
};

FMonolithPieTransactionBufferGuard GMonolithPieTransactionBufferGuard;
}

void FMonolithEditorModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableEditor) return;

	LogCapture = new FMonolithLogCapture();
	GLog->AddOutputDevice(LogCapture);

	FMonolithEditorActions::RegisterActions(LogCapture);
	FMonolithEditorMapActions::RegisterActions(FMonolithToolRegistry::Get());  // F8: create_empty_map + get_module_status
	FMonolithEditorSelectionActions::RegisterActions();
	FMonolithEditorLevelMetadataActions::RegisterActions();
	FMonolithEditorCrashActions::RegisterActions();  // CrashRecovery: get_last_crash_reason / list_recent_crashes / get_crash_stats
	GMonolithPieTransactionBufferGuard.Register();

	// Register settings detail customization
	FPropertyEditorModule& PropModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropModule.RegisterCustomClassLayout(
		UMonolithSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMonolithSettingsCustomization::MakeInstance)
	);

	const int32 EditorActionCount = FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("editor"));
	UE_LOG(LogMonolith, Log, TEXT("Monolith — Editor module loaded (%d editor actions)"), EditorActionCount);
}

void FMonolithEditorModule::ShutdownModule()
{
	GMonolithPieTransactionBufferGuard.Unregister();

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("editor"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("scene"));

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropModule.UnregisterCustomClassLayout(UMonolithSettings::StaticClass()->GetFName());
	}

	if (LogCapture)
	{
		GLog->RemoveOutputDevice(LogCapture);
		delete LogCapture;
		LogCapture = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithEditorModule, MonolithEditor)
