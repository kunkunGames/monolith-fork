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

	// PART C — passive modal watcher. Subscribed before Slate modal windows so
	// that, right before a blocking Slate modal runs its nested game-thread loop (which
	// starves the in-process MCP HTTP server), we emit a structured log line. An external
	// agent tailing the log can recover the modal's context mid-hang to decide kill/relaunch.
	FDelegateHandle PreSlateModalHandle;

	// Best-effort harvest of the about-to-open modal's title + message text, then emit
	// the MODAL_OPEN log line. Always emits at least a timestamped "modal opening" line.
	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
};
