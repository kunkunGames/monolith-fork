#pragma once

#include "Delegates/IDelegateInstance.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

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

	// PART C — passive modal watcher. Subscribed before Slate modal windows so
	// that, right before a blocking Slate modal runs its nested game-thread loop (which
	// starves the in-process MCP HTTP server), we emit a structured log line. An external
	// agent tailing the log can recover the modal's context mid-hang to decide kill/relaunch.
	FDelegateHandle PreSlateModalHandle;

	// Harvest the about-to-open modal identified by FModalWindowContext, then emit
	// MODAL_PROGRESS for an engine-classified slow-task window or MODAL_OPEN otherwise.
	// Missing context/classification data is surfaced explicitly and fails closed to warning.
	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
};
