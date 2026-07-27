#include "MonolithEditorModule.h"
#include "MonolithEditorActions.h"
#include "MonolithEditorMapActions.h"
#include "MonolithEditorSelectionActions.h"
#include "MonolithEditorLevelMetadataActions.h"
#include "MonolithEditorCrashActions.h"
#include "MonolithEditorModalDiagnostics.h"
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
#include "Misc/EngineVersionComparison.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

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

// Best-effort title + message harvest of the active (or about-to-be-active) modal
// window. The window may not yet be on the modal stack at PRE broadcast time, so
// fall back to the active top-level window. Text extraction is delegated to
// MonolithEditorModalDiagnostics::HarvestWidgetTree, which bounds the walk by depth,
// widget count, and text length and reports truncation explicitly.
void HarvestActiveModalWindow(FString& OutTitle, FMonolithModalWidgetSnapshot& OutSnapshot)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SWindow> Window = Slate.GetActiveModalWindow();
	if (!Window.IsValid())
	{
		Window = Slate.GetActiveTopLevelWindow();
	}
	if (Window.IsValid())
	{
		OutTitle = Window->GetTitle().ToString();
		MonolithEditorModalDiagnostics::HarvestWidgetTree(Window->GetContent(), OutSnapshot);
	}
}
}

void HarvestLegacyModalWindow(
	FString& OutTitle,
	FMonolithModalWidgetSnapshot& OutSnapshot,
	bool& bOutWindowAvailable)
{
	bOutWindowAvailable = false;
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SWindow> Window = Slate.GetActiveModalWindow();
	if (!Window.IsValid())
	{
		Window = Slate.GetActiveTopLevelWindow();
	}
	bOutWindowAvailable = Window.IsValid();
	HarvestModalWindow(Window, OutTitle, OutSnapshot);
}

FString FormatOpenAge(const FMonolithModalCloseRecord& Closed)
{
	return Closed.bMatched
		? FString::Printf(TEXT("%.3f"), Closed.OpenAgeSeconds)
		: FString(TEXT("unknown"));
}

void EmitModalClose(const FMonolithModalCloseRecord& Closed)
{
	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S"));
	const FString OpenAge = FormatOpenAge(Closed);
	const TCHAR* Matched = Closed.bMatched ? TEXT("true") : TEXT("false");
	if (Closed.bMatched && Closed.OpenEvent == TEXT("MODAL_PROGRESS"))
	{
		UE_LOG(LogMonolith, Log,
			TEXT("MODAL_CLOSE ts='%s' id=%lld matched=%s open_event=%s slow_task=%s title='%s' open_age_s=%s — progress modal dismissed; game thread resumed."),
			*Timestamp,
			Closed.Identifier,
			Matched,
			*Closed.OpenEvent,
			*Closed.SlowTask,
			*Closed.Title,
			*OpenAge);
		return;
	}

	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_CLOSE ts='%s' id=%lld matched=%s open_event=%s slow_task=%s title='%s' open_age_s=%s — modal dismissed; game thread resumed."),
		*Timestamp,
		Closed.Identifier,
		Matched,
		*Closed.OpenEvent,
		*Closed.SlowTask,
		*Closed.Title,
		*OpenAge);
}

}

FMonolithEditorModule::FMonolithEditorModule() = default;
FMonolithEditorModule::~FMonolithEditorModule() = default;

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

	// PART C — subscribe to the paired pre/post Slate-modal broadcasts so an external
	// consumer tailing the log can pair MODAL_OPEN with MODAL_CLOSE: an unmatched OPEN
	// past a grace period means the game thread is still inside the blocking nested
	// loop (and the in-process MCP server is starved).
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	PreSlateModalHandle = FCoreDelegates::PreSlateModalWithContext.AddRaw(this, &FMonolithEditorModule::OnPreSlateModal);
	PostSlateModalHandle = FCoreDelegates::PostSlateModalWithContext.AddRaw(this, &FMonolithEditorModule::OnPostSlateModal);
#else
	PreSlateModalHandle = FCoreDelegates::PreSlateModal.AddRaw(this, &FMonolithEditorModule::OnPreSlateModal);
	PostSlateModalHandle = FCoreDelegates::PostSlateModal.AddRaw(this, &FMonolithEditorModule::OnPostSlateModal);
#endif
#endif
}

#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8

void FMonolithEditorModule::OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context)
{
	// Always emit at least a timestamped record — text extraction is best-effort
	// (the window may not yet be on the modal stack at broadcast time).
	FString Title;
	FMonolithModalWidgetSnapshot Snapshot;
	HarvestActiveModalWindow(Title, Snapshot);

	FOpenModalRecord Record;
	Record.Title = Title;
	Record.SlowTask = Context.bIsSlowTaskWindow.IsSet()
		? (Context.bIsSlowTaskWindow.GetValue() ? TEXT("true") : TEXT("false"))
		: TEXT("unknown");
	Record.OpenedAt = FDateTime::Now();

	const int64 Id = static_cast<int64>(Context.WindowIdentifier);
	OpenModals.Add(Id, Record);

	// Same verbosity split as the CLOSE record: an engine-classified auto-dismiss
	// progress window is routine and must not drown the log, while a modal that will
	// actually block the game thread (and therefore the in-process MCP server) warns.
	if (MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(Context.bIsSlowTaskWindow))
	{
		UE_LOG(LogMonolith, Log,
			TEXT("MODAL_OPEN ts='%s' id=%lld slow_task=%s widgets=%d truncated=%s title='%s' text='%s' — auto-dismiss progress window; the current editor action stays synchronous until it closes."),
			*Record.OpenedAt.ToString(TEXT("%Y-%m-%dT%H:%M:%S")), Id, *Record.SlowTask,
			Snapshot.VisitedWidgetCount, Snapshot.bTruncated ? TEXT("true") : TEXT("false"),
			*Title, *Snapshot.Text);
		return;
	}

	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_OPEN ts='%s' id=%lld slow_task=%s widgets=%d truncated=%s title='%s' text='%s' — game thread is about to enter a blocking modal loop; MCP will be unresponsive until dismissed."),
		*Record.OpenedAt.ToString(TEXT("%Y-%m-%dT%H:%M:%S")), Id, *Record.SlowTask,
		Snapshot.VisitedWidgetCount, Snapshot.bTruncated ? TEXT("true") : TEXT("false"),
		*Title, *Snapshot.Text);
}

void FMonolithEditorModule::OnPostSlateModal(const FCoreDelegates::FModalWindowContext& Context)
{
	const int64 Id = static_cast<int64>(Context.WindowIdentifier);

	// Echo the cached open-record fields; an unmatched close (subscribed mid-modal,
	// or an open we never saw) still emits a parseable record.
	FString Title;
	FString SlowTask = TEXT("unknown");
	FString OpenAge = TEXT("unknown");
	if (const FOpenModalRecord* Record = OpenModals.Find(Id))
	{
		Title = Record->Title;
		SlowTask = Record->SlowTask;
		OpenAge = FString::Printf(TEXT("%.1f"), (FDateTime::Now() - Record->OpenedAt).GetTotalSeconds());
		OpenModals.Remove(Id);
	}

	// Auto-dismiss progress windows are expected and self-clearing, so they stay at Log
	// while a genuinely blocking modal warns. Verbosity is picked from the cached open
	// record because bIsSlowTaskWindow is only supplied on the PRE broadcast. The record
	// itself is still emitted either way, so OPEN/CLOSE pairing is never broken.
	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S"));
	if (SlowTask == TEXT("true"))
	{
		UE_LOG(LogMonolith, Log,
			TEXT("MODAL_CLOSE ts='%s' id=%lld slow_task=%s title='%s' open_age_s=%s — auto-dismiss progress window closed; game thread resumed."),
			*Timestamp, Id, *SlowTask, *Title, *OpenAge);
		return;
	}

	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_CLOSE ts='%s' id=%lld slow_task=%s title='%s' open_age_s=%s — modal dismissed; game thread resumed."),
		*Timestamp, Id, *SlowTask, *Title, *OpenAge);
}

#else // pre-5.8: no WithContext delegates — paired records without id/slow-task.

void FMonolithEditorModule::OnPreSlateModal()
{
	FString Title;
	FMonolithModalWidgetSnapshot Snapshot;
	HarvestActiveModalWindow(Title, Snapshot);

	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_OPEN ts='%s' id=0 slow_task=unknown widgets=%d truncated=%s title='%s' text='%s' — game thread is about to enter a blocking modal loop; MCP will be unresponsive until dismissed."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")),
		Snapshot.VisitedWidgetCount, Snapshot.bTruncated ? TEXT("true") : TEXT("false"),
		*Title, *Snapshot.Text);
}

void FMonolithEditorModule::OnPostSlateModal()
{
	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_CLOSE ts='%s' id=0 slow_task=unknown title='' open_age_s=unknown — modal dismissed; game thread resumed."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")));
}

#endif // engine version
#endif // WITH_EDITOR

void FMonolithEditorModule::ShutdownModule()
{
	// The automation observer is module-owned code registered in the engine-lifetime
	// core ticker and shared AutomationController delegates. Tear it down before
	// unregistering actions or unloading this module.
	FMonolithEditorActions::ShutdownAutomationSessions();

	GMonolithHeadlessLayoutSaveGuard.Unregister();
	GMonolithPieTransactionBufferGuard.Unregister();

	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithEditor"));
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	if (PreSlateModalHandle.IsValid())
	{
		FCoreDelegates::PreSlateModalWithContext.Remove(PreSlateModalHandle);
		PreSlateModalHandle.Reset();
	}
	if (PostSlateModalHandle.IsValid())
	{
		FCoreDelegates::PostSlateModalWithContext.Remove(PostSlateModalHandle);
		PostSlateModalHandle.Reset();
	}
	OpenModals.Empty();
#else
	if (PreSlateModalHandle.IsValid())
	{
		FCoreDelegates::PreSlateModalWithContext.Remove(PreSlateModalHandle);
		PreSlateModalHandle.Reset();
	}
	if (PostSlateModalHandle.IsValid())
	{
		FCoreDelegates::PostSlateModal.Remove(PostSlateModalHandle);
		PostSlateModalHandle.Reset();
	}
#endif
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
