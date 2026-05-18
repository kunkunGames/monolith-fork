#pragma once

#include "CoreMinimal.h"
#include "MonolithActionExecutionGuard.h"
#include "Dom/JsonObject.h"
#include "Delegates/IDelegateInstance.h"

class FScopedTransaction;

/**
 * Crash Breadcrumb & Post-Mortem (PRD: MonolithCrashRecovery_PRD.md)
 *
 * Hooks FCoreDelegates::OnHandleSystemError / OnHandleSystemEnsure and writes
 * a lightweight, dependency-free breadcrumb file synchronously before the
 * editor terminates. On the next session the agent can read the breadcrumb
 * via editor_query("get_last_crash_reason") to learn which MCP tool caused
 * the disconnect.
 *
 * Thread / heap safety:
 *  - Fatal handler is called in a hostile context (heap potentially corrupt).
 *  - All payload strings are pre-built in the ScopedCapture constructor and
 *    held in singleton members; the fatal handler only calls
 *    FFileHelper::SaveStringToFile + FGenericCrashContext::SetEngineData.
 *  - Single capture slot — ExecuteAction is dispatched on the game thread
 *    (FHttpServerModule + FTicker), so concurrent slot writers are not
 *    expected. Nested ExecuteAction calls keep the outer capture (the call
 *    site that started the chain).
 */
class MONOLITHCORE_API FMonolithCrashBreadcrumb
{
public:
	static FMonolithCrashBreadcrumb& Get();

	/** Bind FCoreDelegates handlers. Call from MonolithCore StartupModule. */
	void Init();

	/** Unbind handlers and clear slot. Call from MonolithCore ShutdownModule. */
	void Shutdown();

	/** Plugin-relative crash directory: <Plugin>/Saved/Monolith/Crashes */
	static FString GetCrashesDir();

	/** Plugin-relative latest pointer: <CrashesDir>/latest.txt */
	static FString GetLatestPointerPath();

	/**
	 * RAII capture installed by FMonolithToolRegistry::ExecuteAction around
	 * each handler dispatch. Pre-builds the full JSON payload + target paths
	 * so the fatal handler can write without any heap allocation.
	 */
	class MONOLITHCORE_API FScopedCapture
	{
	public:
		FScopedCapture(const FString& Namespace,
			const FString& Action,
			const TSharedPtr<FJsonObject>& Params);
		~FScopedCapture();

		void SetOutcome(bool bSuccess, int32 ErrorCode, const TSharedPtr<FJsonObject>& ResultObject, const FString& ErrorMessage);

	private:
		// Non-copyable / non-movable to keep slot lifetime predictable.
		FScopedCapture(const FScopedCapture&) = delete;
		FScopedCapture& operator=(const FScopedCapture&) = delete;

		// True if this scope owns the active slot (false for nested calls).
		bool bOwnsSlot = false;
		FMonolithActionExecutionGuard::FExecutionScope ExecutionScope;
		TUniquePtr<FScopedTransaction> ScopedTransaction;
	};

private:
	FMonolithCrashBreadcrumb() = default;

	void OnHandleSystemError();
	void OnHandleSystemEnsure();
	void WriteFromSlot(const TCHAR* Kind);

	// ------- captured slot (game-thread only; outer-most ExecuteAction wins) -------
	FString CurrentPayloadPath;   // <CrashesDir>/<ISO8601>.json
	FString CurrentPayloadJson;   // full JSON body (built ahead of time)
	FString CurrentLatestPath;    // <CrashesDir>/latest.txt
	FString CurrentLatestName;    // "<ISO8601>.json" (single line for latest.txt)
	FString CurrentToolAction;    // "ns.action" (engine-data field)
	bool bSlotActive = false;

	// ------- module-lifetime state -------
	FString SessionId;            // FGuid for the current process
	FDelegateHandle FatalHandle;
	FDelegateHandle EnsureHandle;
	bool bInitialized = false;

	friend class FScopedCapture;
};
