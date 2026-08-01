#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Delegates/IDelegateInstance.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Templates/UniquePtr.h"

class FMonolithLogCapture;
class FMonolithModalTelemetryState;

class FMonolithEditorModule : public IModuleInterface
{
public:
	FMonolithEditorModule();
	virtual ~FMonolithEditorModule() override;
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FMonolithLogCapture* LogCapture = nullptr;

	// Slate cancels non-slow-task modal windows only while
	// GIsRunningUnattendedScript is true. -Unattended does not set that global,
	// so a NullRHI MCP editor otherwise enters an unserviceable nested modal loop.
	bool bOwnsHeadlessUnattendedScriptGuard = false;
	bool bPreviousRunningUnattendedScript = false;

	// PART C — paired passive modal watcher. Every open/progress record receives a
	// matching close record with the same id and cached slow-task/title/open-age data.
	// UE 5.8+ supplies stable context ids; UE 5.7 uses the legacy delegate pair and
	// an internal LIFO id stack for nested modals.
	FDelegateHandle PreSlateModalHandle;
	FDelegateHandle PostSlateModalHandle;
	TUniquePtr<FMonolithModalTelemetryState> ModalTelemetry;

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
	void OnPostSlateModal(const FCoreDelegates::FModalWindowContext& Context);
#else
	void OnPreSlateModal();
	void OnPostSlateModal();
#endif
};
