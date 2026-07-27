#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

// Gap 4: deterministic Play-In-Editor input / control driving (editor namespace).
//
//   pie_set_control_rotation   — write APlayerController::SetControlRotation, optionally
//                                re-applied each frame for hold_frames to outlast per-tick
//                                camera/control systems (best-effort — see the action description).
//   pie_inject_input_action    — inject a UInputAction value through the EnhancedInput local-player
//                                subsystem (InjectInputForAction), optionally repeated for N frames.
//   pie_inject_key             — inject a named physical FKey through the PIE game viewport's
//                                InputKey path so viewport-level routers (including CommonUI)
//                                process the same event contract as real gameplay input.
//   pie_start_with_url_options — start canonical offline/synchronous in-viewport PIE for the
//                                current editor map while transiently supplying validated Unreal
//                                URL options; async Online PIE is rejected before mutation/start.
//   pie_possess_spectator_free — detach the PlayerController to a free-fly spectator pawn and back.
//
// All game-thread-only. The held-rotation / repeated-input re-apply runs from a single shared
// FTSTicker that self-unregisters when no holds remain and clears on PIE end, so a torn-down PIE
// world is never dereferenced.
class FMonolithPieInputActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Bind / unbind the editor PIE-end delegate that drops held-rotation, repeated-input, and
	// stored-spectator state so the re-apply ticker never touches a torn-down PIE world. Called
	// from the module's StartupModule / ShutdownModule. Idempotent.
	static void RegisterPieEndHook();
	static void UnregisterPieEndHook();

	// Pure test seams for the URL-option start's fail-closed preflight. The Online PIE
	// predicate mirrors UE 5.8's canonical PlayInEditor route with bAllowOnlineSubsystem=true.
	static bool WouldUseAsyncOnlinePieLogin(
		int32 DesiredNumberOfClients,
		bool bSupportsOnlinePie,
		int32 NumAvailablePieLogins);
	static bool HasExistingPieStartConflict(
		bool bPlaySessionInProgress,
		bool bPlaySessionRequestSet,
		bool bPlayInEditorSessionInfoSet,
		bool bHasActivePieWorld);

	static FMonolithActionResult HandleSetControlRotation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleInjectInputAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleInjectKey(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStartWithUrlOptions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandlePossessSpectatorFree(const TSharedPtr<FJsonObject>& Params);
};
