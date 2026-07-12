#include "MonolithEditorModule.h"
#include "MonolithEditorActions.h"
#include "MonolithEditorMapActions.h"
#include "MonolithEditorSelectionActions.h"
#include "MonolithEditorLevelMetadataActions.h"
#include "MonolithEditorCrashActions.h"
#include "MonolithBuildArtifactActions.h"
#include "MonolithPieInputActions.h"
#include "MonolithPieObjectActions.h"
#include "MonolithPieTimeseries.h"
#include "MonolithStatActions.h"
#include "MonolithSettingsCustomization.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "PropertyEditorModule.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"
#include "UObject/UObjectGlobals.h"

// PART C — passive modal watcher.
#include "Misc/CoreDelegates.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Layout/Children.h"

// Headless layout-save guard.
#include "Containers/Ticker.h"
#include "Framework/Docking/TabManager.h"
#include "LevelEditor.h"
#include "Misc/App.h"
#include "Subsystems/AssetEditorSubsystem.h"

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

// Headless (-NullRHI / no-render) editors create Slate windows whose native window is
// the base FGenericWindow. FTabManager's 5s deferred layout save then fatals in
// SDockingArea::GatherPersistentLayout -> SWindow::GetNonMaximizedRectInScreen ->
// FGenericWindow::GetRestoredDimensions ("not expected to be called on this platform",
// GenericWindow.cpp:113), killing the editor ~10s after boot and taking the Monolith
// MCP server with it. The engine is an installed build and the per-manager
// bCanDoDeferredLayoutSave switch sits behind FTabManager::FPrivateApi (protected), so
// this guard uses the two public surfaces instead:
//   1. FGlobalTabmanager::SetCanSavePersistentLayouts(false) — layout ini/json writes
//      are meaningless without real window dimensions.
//   2. ClearPendingLayoutSave() on the global + level-editor tab managers, once at
//      OnPostEngineInit and then every second, which always cancels the 5s deferred
//      save ticker before it can fire (FTSTicker does not tick during engine init, so
//      saves requested mid-init are cleared by the immediate pass).
// Open asset editors are covered too: their host tab managers are enumerated
// through UAssetEditorSubsystem on every clearing pass.
class FMonolithHeadlessLayoutSaveGuard
{
public:
	void Register()
	{
		if (bRegistered || FApp::CanEverRender())
		{
			return;
		}
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
			this, &FMonolithHeadlessLayoutSaveGuard::HandlePostEngineInit);
		bRegistered = true;
	}

	void Unregister()
	{
		if (!bRegistered)
		{
			return;
		}
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}
		if (TickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
		bRegistered = false;
	}

private:
	void HandlePostEngineInit()
	{
		FGlobalTabmanager::Get()->SetCanSavePersistentLayouts(false);
		ClearPendingLayoutSaves();
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float /*DeltaTime*/)
			{
				ClearPendingLayoutSaves();
				return true;
			}),
			1.0f);
		UE_LOG(LogMonolith, Log,
			TEXT("HeadlessLayoutSaveGuard: CanEverRender=false — persistent layout saves disabled and deferred layout-save tickers are being cleared (FGenericWindow::GetRestoredDimensions fatals under null windows)."));
	}

	static void ClearPendingLayoutSaves()
	{
		FGlobalTabmanager::Get()->ClearPendingLayoutSave();
		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			if (TSharedPtr<FTabManager> LevelEditorTabManager = LevelEditor.GetLevelEditorTabManager())
			{
				LevelEditorTabManager->ClearPendingLayoutSave();
			}
		}
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
				{
					IAssetEditorInstance* Instance = Asset
						? AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false)
						: nullptr;
					if (Instance)
					{
						if (TSharedPtr<FTabManager> AssetTabManager = Instance->GetAssociatedTabManager())
						{
							AssetTabManager->ClearPendingLayoutSave();
						}
					}
				}
			}
		}
	}

	FDelegateHandle PostEngineInitHandle;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bRegistered = false;
};

FMonolithHeadlessLayoutSaveGuard GMonolithHeadlessLayoutSaveGuard;

	// Recursively walk a Slate widget subtree, appending the text of every STextBlock
	// found to OutText (newline-joined). Best-effort and depth-bounded so a pathological
	// tree can't stall the broadcast. The broadcasting thread is the game thread; this
	// runs before the modal's nested loop starts.
	void HarvestTextBlocks(const TSharedPtr<SWidget>& Widget, FString& OutText, int32 Depth)
	{
		if (!Widget.IsValid() || Depth > 12)
		{
			return;
		}
		// STextBlock is a SLeafWidget — identify by widget type name (no RTTI dependency).
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			const FText Text = StaticCastSharedPtr<STextBlock>(Widget)->GetText();
			if (!Text.IsEmpty())
			{
				if (!OutText.IsEmpty()) { OutText.Append(TEXT(" | ")); }
				OutText.Append(Text.ToString());
			}
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			const int32 Num = Children->Num();
			for (int32 Index = 0; Index < Num; ++Index)
			{
				HarvestTextBlocks(Children->GetChildAt(Index), OutText, Depth + 1);
			}
		}
	}
}

void FMonolithEditorModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableEditor) return;

	bPreviousRunningUnattendedScript = GIsRunningUnattendedScript;
	if (!FApp::CanEverRender() && !GIsRunningUnattendedScript)
	{
		// FSlateApplication::AddModalWindow cancels non-slow-task modals on this
		// engine-supported guard. FApp::IsUnattended() alone is not consulted there.
		GIsRunningUnattendedScript = true;
		bOwnsHeadlessUnattendedScriptGuard = true;
		UE_LOG(
			LogMonolith,
			Log,
			TEXT("HeadlessUnattendedScriptGuard: CanEverRender=false — non-slow-task Slate modals will be canceled"));
	}

	LogCapture = new FMonolithLogCapture();
	GLog->AddOutputDevice(LogCapture);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithEditor"), [this](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithEditorActions::RegisterActions(LogCapture);
		FMonolithEditorMapActions::RegisterActions(OwnedRegistry);  // F8: create_empty_map + get_module_status
		FMonolithEditorSelectionActions::RegisterActions();
		FMonolithEditorLevelMetadataActions::RegisterActions();
		FMonolithEditorCrashActions::RegisterActions();  // CrashRecovery: get_last_crash_reason / list_recent_crashes / get_crash_stats
		FMonolithPieObjectActions::RegisterActions(OwnedRegistry);
		FMonolithPieTimeseries::RegisterActions(OwnedRegistry);
		FMonolithPieInputActions::RegisterActions(OwnedRegistry);
		FMonolithStatActions::RegisterActions(OwnedRegistry);
		FMonolithBuildArtifactActions::RegisterActions(OwnedRegistry);
	});
	GMonolithPieTransactionBufferGuard.Register();
	GMonolithHeadlessLayoutSaveGuard.Register();

	// Register settings detail customization
	FPropertyEditorModule& PropModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropModule.RegisterCustomClassLayout(
		UMonolithSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMonolithSettingsCustomization::MakeInstance)
	);

	const int32 EditorActionCount = Registry.GetNamespaceActionCount(TEXT("editor"));
	UE_LOG(LogMonolith, Log, TEXT("Monolith — Editor module loaded (%d editor actions)"), EditorActionCount);

	// PART C — subscribe to the pre-Slate-modal broadcast so we can log modal context
	// just before the blocking nested loop starves the in-process MCP server.
#if WITH_EDITOR
	PreSlateModalHandle = FCoreDelegates::PreSlateModalWithContext.AddRaw(this, &FMonolithEditorModule::OnPreSlateModal);
#endif
}

void FMonolithEditorModule::OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context)
{
	// PreSlateModalWithContext fires before Slate pushes the window onto its active-modal
	// stack. Use the supplied stable identifier instead of harvesting the unrelated active
	// top-level window, which can report background dock-hint text as the modal message.
	FString Title;
	FString Text;

	if (FSlateApplication::IsInitialized())
	{
		SWindow* ContextWindow = reinterpret_cast<SWindow*>(Context.WindowIdentifier);
		if (ContextWindow)
		{
			Title = ContextWindow->GetTitle().ToString();
			HarvestTextBlocks(ContextWindow->GetContent(), Text, 0);
		}
		else
		{
			TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
			if (ActiveWindow.IsValid())
			{
				Title = ActiveWindow->GetTitle().ToString();
			}
		}
	}

	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_OPEN ts='%s' title='%s' text='%s' — game thread is about to enter a blocking modal loop; MCP will be unresponsive until dismissed."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")), *Title, *Text);
}

void FMonolithEditorModule::ShutdownModule()
{
	GMonolithHeadlessLayoutSaveGuard.Unregister();
	GMonolithPieTransactionBufferGuard.Unregister();

	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithEditor"));
#if WITH_EDITOR
	if (PreSlateModalHandle.IsValid())
	{
		FCoreDelegates::PreSlateModalWithContext.Remove(PreSlateModalHandle);
		PreSlateModalHandle.Reset();
	}
#endif

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

	if (bOwnsHeadlessUnattendedScriptGuard)
	{
		GIsRunningUnattendedScript = bPreviousRunningUnattendedScript;
		bOwnsHeadlessUnattendedScriptGuard = false;
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithEditorModule, MonolithEditor)
