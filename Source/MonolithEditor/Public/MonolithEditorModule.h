#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Delegates/IDelegateInstance.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"

class FMonolithLogCapture;

class FMonolithEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FMonolithLogCapture* LogCapture = nullptr;

	// Slate cancels non-slow-task modal windows only while
	// GIsRunningUnattendedScript is true. -Unattended does not set that global,
	// so a NullRHI MCP editor otherwise enters an unserviceable nested modal loop.
	bool bOwnsHeadlessUnattendedScriptGuard = false;
	bool bPreviousRunningUnattendedScript = false;

	// PART C — passive modal watcher. Emits paired MODAL_OPEN / MODAL_CLOSE log records
	// around every blocking Slate modal (whose nested game-thread loop starves the
	// in-process MCP HTTP server). An external agent tailing the log can pair the
	// records to distinguish a healthy open→close from a modal that is still blocking,
	// and can recover the modal's context mid-hang to decide kill/relaunch. On UE 5.8+
	// the records carry the engine's WindowIdentifier (guaranteed to match between the
	// pre and post broadcasts) plus the slow-task flag, so long-running-but-legitimate
	// windows (builds, shader compiles) are classifiable by the consumer.
	FDelegateHandle PreSlateModalHandle;
	FDelegateHandle PostSlateModalHandle;

#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	// bIsSlowTaskWindow is only supplied on the PRE broadcast — cache it (plus the
	// harvested title and open time) per WindowIdentifier so the CLOSE record can
	// echo them. Keyed map handles nested modals. Game-thread only; no locking.
	struct FOpenModalRecord
	{
		FString Title;
		FString SlowTask; // "true" | "false" | "unknown"
		FDateTime OpenedAt;
	};
	TMap<int64, FOpenModalRecord> OpenModals;

	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
	void OnPostSlateModal(const FCoreDelegates::FModalWindowContext& Context);
#else
	// Pre-5.8 engines lack the WithContext delegates: records are still paired but
	// carry id=0 and slow_task=unknown (consumers pair by LIFO nesting order).
	void OnPreSlateModal();
	void OnPostSlateModal();
#endif
#endif
};
