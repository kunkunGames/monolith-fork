#include "MonolithPieInputActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithPieObjectActions.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/SpectatorPawn.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Char.h"
#include "Net/OnlineEngineInterface.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Templates/UnrealTemplate.h"
#include "UnrealEdGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "InputAction.h"
#include "InputActionValue.h"
#include "EnhancedInputSubsystems.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPieInput, Log, All);

// =============================================================================
//  Shared PIE-input plumbing (game-thread-only).
// =============================================================================

namespace
{
	constexpr int32 MaxPieUrlOptions = 32;
	constexpr int32 MaxPieUrlOptionLength = 256;

	bool IsAsciiAlpha(const TCHAR Ch)
	{
		return (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z'));
	}

	bool IsAsciiDigit(const TCHAR Ch)
	{
		return Ch >= TEXT('0') && Ch <= TEXT('9');
	}

	bool IsValidUrlOptionKey(const FString& Key)
	{
		if (Key.IsEmpty() || (!IsAsciiAlpha(Key[0]) && Key[0] != TEXT('_')))
		{
			return false;
		}

		for (int32 Index = 1; Index < Key.Len(); ++Index)
		{
			const TCHAR Ch = Key[Index];
			if (!IsAsciiAlpha(Ch) && !IsAsciiDigit(Ch)
				&& Ch != TEXT('_') && Ch != TEXT('-') && Ch != TEXT('.'))
			{
				return false;
			}
		}
		return true;
	}

	bool ValidatePieUrlOptions(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutOptions,
		FString& OutError)
	{
		OutOptions.Reset();

		if (!Params.IsValid())
		{
			OutError = TEXT("pie_start_with_url_options requires a parameter object");
			return false;
		}

		const TSharedPtr<FJsonValue> OptionsField = Params->TryGetField(TEXT("url_options"));
		if (!OptionsField.IsValid() || OptionsField->Type != EJson::Array)
		{
			OutError = TEXT("pie_start_with_url_options requires 'url_options' as an array of strings");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>& Values = OptionsField->AsArray();
		if (Values.Num() == 0 || Values.Num() > MaxPieUrlOptions)
		{
			OutError = FString::Printf(
				TEXT("pie_start_with_url_options requires between 1 and %d URL options"),
				MaxPieUrlOptions);
			return false;
		}

		TSet<FString> SeenKeys;
		OutOptions.Reserve(Values.Num());
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = Values[Index];
			FString Option;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Option))
			{
				OutError = FString::Printf(TEXT("url_options[%d] must be a string"), Index);
				return false;
			}

			if (Option.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("url_options[%d] must not be empty or whitespace-only"), Index);
				return false;
			}
			if (Option.Len() > MaxPieUrlOptionLength)
			{
				OutError = FString::Printf(
					TEXT("url_options[%d] exceeds the %d-character limit"),
					Index,
					MaxPieUrlOptionLength);
				return false;
			}
			if (Option.Contains(TEXT("?")) || Option.Contains(TEXT("#")))
			{
				OutError = FString::Printf(
					TEXT("url_options[%d] must not contain '?' or '#'; pass only the option body"),
					Index);
				return false;
			}
			for (const TCHAR Ch : Option)
			{
				if (FChar::IsControl(Ch))
				{
					OutError = FString::Printf(TEXT("url_options[%d] must not contain control characters"), Index);
					return false;
				}
			}

			FString Key;
			FString IgnoredValue;
			if (!Option.Split(TEXT("="), &Key, &IgnoredValue, ESearchCase::CaseSensitive, ESearchDir::FromStart))
			{
				Key = Option;
			}
			if (!IsValidUrlOptionKey(Key))
			{
				OutError = FString::Printf(
					TEXT("url_options[%d] has invalid key '%s'; keys must match [A-Za-z_][A-Za-z0-9_.-]*"),
					Index,
					*Key);
				return false;
			}

			const FString NormalizedKey = Key.ToLower();
			if (SeenKeys.Contains(NormalizedKey))
			{
				OutError = FString::Printf(
					TEXT("url_options[%d] duplicates option key '%s' (keys are case-insensitive)"),
					Index,
					*Key);
				return false;
			}
			SeenKeys.Add(NormalizedKey);
			OutOptions.Add(MoveTemp(Option));
		}

		return true;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	// Resolve the active PIE world (first EWorldType::PIE context), or nullptr.
	// Mirrors MonolithPieObject::FindPieWorld so the input actions share one notion of
	// "the running PIE world".
	UWorld* GetPieWorld()
	{
		return MonolithPieObject::FindPieWorld();
	}

	// Resolve the player controller for player_index against the PIE world. Index 0 is the
	// usual single-player case. Returns nullptr if PIE isn't running or the index has no PC.
	APlayerController* ResolvePlayerController(UWorld* PieWorld, int32 PlayerIndex)
	{
		if (!PieWorld)
		{
			return nullptr;
		}
		return UGameplayStatics::GetPlayerController(PieWorld, PlayerIndex);
	}

	// Read pitch/yaw/roll out of a params blob into a rotator (missing components default 0).
	FRotator ReadRotator(const TSharedPtr<FJsonObject>& Params)
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		Params->TryGetNumberField(TEXT("pitch"), Pitch);
		Params->TryGetNumberField(TEXT("yaw"), Yaw);
		Params->TryGetNumberField(TEXT("roll"), Roll);
		return FRotator(static_cast<double>(Pitch), static_cast<double>(Yaw), static_cast<double>(Roll));
	}

	// Preserve the three established lenient player_index contracts while keeping every
	// double-to-int conversion defined. Missing/wrong-type values still select player 0;
	// non-finite and out-of-range numbers resolve to an index that cannot name a normal PC.
	int32 ReadPlayerIndexLenient(const TSharedPtr<FJsonObject>& Params)
	{
		double RawPlayerIndex = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("player_index"), RawPlayerIndex))
		{
			return 0;
		}
		if (!FMath::IsFinite(RawPlayerIndex))
		{
			return MAX_int32;
		}
		return static_cast<int32>(FMath::Clamp(
			RawPlayerIndex,
			static_cast<double>(MIN_int32),
			static_cast<double>(MAX_int32)));
	}

	// pie_inject_key is a new strict contract: validate before any PIE-world lookup so
	// malformed payloads fail deterministically even when PIE is not running.
	bool TryReadPlayerIndexStrict(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ActionName,
		int32& OutPlayerIndex,
		FString& OutError)
	{
		OutPlayerIndex = 0;
		if (!Params->HasField(TEXT("player_index")))
		{
			return true;
		}

		double RawPlayerIndex = 0.0;
		if (!Params->TryGetNumberField(TEXT("player_index"), RawPlayerIndex)
			|| !FMath::IsFinite(RawPlayerIndex)
			|| RawPlayerIndex < 0.0
			|| RawPlayerIndex > static_cast<double>(MAX_int32)
			|| RawPlayerIndex != FMath::TruncToDouble(RawPlayerIndex))
		{
			OutError = FString::Printf(
				TEXT("%s parameter 'player_index' must be a non-negative integer"), ActionName);
			return false;
		}

		OutPlayerIndex = static_cast<int32>(RawPlayerIndex);
		return true;
	}

	// ── Held-rotation / repeated-input re-apply registry ──────────────────
	//
	// A single shared FTSTicker re-applies any active "holds" each editor frame. Holds are
	// keyed by player_index. A hold counts down its remaining frames every tick; at zero it is
	// dropped. When no holds remain the ticker self-unregisters. World validity is checked each
	// tick so a torn-down PIE world is never dereferenced.

	struct FHeldRotation
	{
		int32 PlayerIndex = 0;
		FRotator Rotation = FRotator::ZeroRotator;
		int32 FramesRemaining = 0; // re-applied while > 0
	};

	struct FHeldInput
	{
		int32 PlayerIndex = 0;
		TWeakObjectPtr<const UInputAction> Action;
		FInputActionValue Value;
		int32 FramesRemaining = 0; // injected while > 0
	};

	class FPieInputHoldRegistry
	{
	public:
		static FPieInputHoldRegistry& Get()
		{
			static FPieInputHoldRegistry Singleton;
			return Singleton;
		}

		// Replace any existing rotation hold for this player_index (latest write wins) and
		// ensure the ticker is running. FramesRemaining of 0 means a one-shot (no hold queued).
		void SetRotationHold(int32 PlayerIndex, const FRotator& Rotation, int32 HoldFrames)
		{
			if (HoldFrames <= 0)
			{
				return; // one-shot already applied by the handler; nothing to queue
			}
			for (FHeldRotation& H : HeldRotations)
			{
				if (H.PlayerIndex == PlayerIndex)
				{
					H.Rotation = Rotation;
					H.FramesRemaining = HoldFrames;
					EnsureTicker();
					return;
				}
			}
			FHeldRotation New;
			New.PlayerIndex = PlayerIndex;
			New.Rotation = Rotation;
			New.FramesRemaining = HoldFrames;
			HeldRotations.Add(New);
			EnsureTicker();
		}

		// Queue a repeated input injection. RepeatFrames of <= 1 means the handler already did
		// the single injection; nothing is queued. RepeatFrames > 1 queues (RepeatFrames - 1)
		// additional per-frame injections.
		void SetInputHold(int32 PlayerIndex, const UInputAction* Action, const FInputActionValue& Value, int32 ExtraFrames)
		{
			if (!Action || ExtraFrames <= 0)
			{
				return;
			}
			FHeldInput New;
			New.PlayerIndex = PlayerIndex;
			New.Action = Action;
			New.Value = Value;
			New.FramesRemaining = ExtraFrames;
			HeldInputs.Add(New);
			EnsureTicker();
		}

		// Drop every hold AND remove the ticker (called on PIE end + module shutdown). The
		// RemoveTicker is load-bearing: FTSTicker is an engine-lifetime singleton and the ticker
		// is bound with CreateRaw(this,...). Merely emptying the hold arrays would leave that
		// ticker registered, so if the module unloads while it is live it would fire into freed
		// registry memory — a use-after-free. StopTicker tears it down deterministically.
		void ClearAll()
		{
			HeldRotations.Reset();
			HeldInputs.Reset();
			StopTicker();
		}

	private:
		FPieInputHoldRegistry() = default;

		void EnsureTicker()
		{
			if (TickerHandle.IsValid())
			{
				return;
			}
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FPieInputHoldRegistry::OnTick), 0.0f);
		}

		// Unregister the shared ticker if it is live. RemoveTicker is a STATIC FTSTicker API
		// (verified offline: Containers/Ticker.h line 66 — `static void RemoveTicker(FDelegateHandle)`),
		// unlike AddTicker which is an instance method on GetCoreTicker().
		void StopTicker()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}

		bool OnTick(float /*DeltaTime*/)
		{
			UWorld* PieWorld = GetPieWorld();
			if (!PieWorld || PieWorld->bIsTearingDown)
			{
				// PIE gone (or tearing down) — drop all holds and stop ticking. Returning false
				// makes the engine auto-remove this ticker, so we only clear our cached handle
				// here (no RemoveTicker — that would double-remove). With the PIE-end hook also
				// calling StopTicker(), this guard is a belt-and-braces second line of defence.
				HeldRotations.Reset();
				HeldInputs.Reset();
				TickerHandle.Reset();
				return false;
			}

			// Re-apply rotation holds (executed each frame — best-effort vs per-tick camera
			// systems that may re-write ControlRotation later in the same frame).
			for (int32 Index = HeldRotations.Num() - 1; Index >= 0; --Index)
			{
				FHeldRotation& H = HeldRotations[Index];
				if (APlayerController* PC = ResolvePlayerController(PieWorld, H.PlayerIndex))
				{
					PC->SetControlRotation(H.Rotation);
				}
				if (--H.FramesRemaining <= 0)
				{
					HeldRotations.RemoveAt(Index);
				}
			}

			// Re-inject repeated input actions.
			for (int32 Index = HeldInputs.Num() - 1; Index >= 0; --Index)
			{
				FHeldInput& H = HeldInputs[Index];
				const UInputAction* Action = H.Action.Get();
				bool bDropped = (Action == nullptr);
				if (Action)
				{
					if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
						ULocalPlayer::GetSubsystemFromController<UEnhancedInputLocalPlayerSubsystem>(
							ResolvePlayerController(PieWorld, H.PlayerIndex)))
					{
						Subsystem->InjectInputForAction(Action, H.Value, {}, {});
					}
					else
					{
						bDropped = true; // no subsystem — stop trying
					}
				}
				if (bDropped || --H.FramesRemaining <= 0)
				{
					HeldInputs.RemoveAt(Index);
				}
			}

			if (HeldRotations.Num() == 0 && HeldInputs.Num() == 0)
			{
				TickerHandle.Reset();
				return false; // self-unregister
			}
			return true;
		}

		TArray<FHeldRotation> HeldRotations;
		TArray<FHeldInput> HeldInputs;
		FTSTicker::FDelegateHandle TickerHandle;
	};

	// ── Spectator detach/reverse state ────────────────────────────────────
	//
	// Stores the originally-possessed pawn per player_index so pie_possess_spectator_free can
	// re-possess on disable. Game-thread-only; cleared on PIE end.

	struct FSpectatorState
	{
		int32 PlayerIndex = 0;
		TWeakObjectPtr<APawn> OriginalPawn;
	};

	class FPieSpectatorRegistry
	{
	public:
		static FPieSpectatorRegistry& Get()
		{
			static FPieSpectatorRegistry Singleton;
			return Singleton;
		}

		void StoreOriginalPawn(int32 PlayerIndex, APawn* Pawn)
		{
			for (FSpectatorState& S : States)
			{
				if (S.PlayerIndex == PlayerIndex)
				{
					S.OriginalPawn = Pawn;
					return;
				}
			}
			FSpectatorState New;
			New.PlayerIndex = PlayerIndex;
			New.OriginalPawn = Pawn;
			States.Add(New);
		}

		// Returns the stored original pawn for this player_index (may be stale/null) and clears
		// the entry. Returns nullptr if none was stored.
		APawn* TakeOriginalPawn(int32 PlayerIndex)
		{
			for (int32 Index = 0; Index < States.Num(); ++Index)
			{
				if (States[Index].PlayerIndex == PlayerIndex)
				{
					APawn* Pawn = States[Index].OriginalPawn.Get();
					States.RemoveAt(Index);
					return Pawn;
				}
			}
			return nullptr;
		}

		bool HasState(int32 PlayerIndex) const
		{
			for (const FSpectatorState& S : States)
			{
				if (S.PlayerIndex == PlayerIndex) { return true; }
			}
			return false;
		}

		void ClearAll() { States.Reset(); }

	private:
		FPieSpectatorRegistry() = default;
		TArray<FSpectatorState> States;
	};
}

// Drop all per-session input/spectator state on PIE end so the re-apply ticker never touches a
// torn-down PIE world.
namespace
{
	void ClearPieInputState()
	{
		FPieInputHoldRegistry::Get().ClearAll();
		FPieSpectatorRegistry::Get().ClearAll();
	}

	// A single PIE-end hook. PrePIEEnded fires earlier in teardown than EndPIE (before the world
	// is fully gone), which is the safer point to stop the re-apply ticker — and ClearAll's
	// StopTicker is idempotent, so one hook is sufficient. (EndPIE would be redundant.)
	FDelegateHandle GPrePieEndedHandle;
}

void FMonolithPieInputActions::RegisterPieEndHook()
{
	if (!GPrePieEndedHandle.IsValid())
	{
		GPrePieEndedHandle = FEditorDelegates::PrePIEEnded.AddLambda([](const bool /*bIsSimulating*/) { ClearPieInputState(); });
	}
}

void FMonolithPieInputActions::UnregisterPieEndHook()
{
	if (GPrePieEndedHandle.IsValid())
	{
		FEditorDelegates::PrePIEEnded.Remove(GPrePieEndedHandle);
		GPrePieEndedHandle.Reset();
	}
	ClearPieInputState();
}

// =============================================================================
//  UInputAction resolution (by asset path or short name)
// =============================================================================

namespace
{
	// Resolve a UInputAction from a path or short name. A path-like string ("/Game/...") is
	// loaded directly; otherwise the asset registry is searched for a UInputAction whose asset
	// name matches. Returns nullptr + a reason when unresolved.
	const UInputAction* ResolveInputAction(const FString& Spec, FString& OutError)
	{
		if (Spec.IsEmpty())
		{
			OutError = TEXT("input_action is empty");
			return nullptr;
		}

		// Path-like: load directly.
		if (Spec.StartsWith(TEXT("/")))
		{
			if (const UInputAction* Action = FMonolithAssetUtils::LoadAssetByPath<UInputAction>(Spec))
			{
				return Action;
			}
			OutError = FString::Printf(TEXT("No UInputAction asset at path '%s'"), *Spec);
			return nullptr;
		}

		// Short name: scan the asset registry for a UInputAction whose name matches.
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

		const UInputAction* FirstMatch = nullptr;
		int32 MatchCount = 0;
		for (const FAssetData& Data : Assets)
		{
			if (Data.AssetName.ToString() == Spec)
			{
				if (const UInputAction* Action = Cast<UInputAction>(Data.GetAsset()))
				{
					if (!FirstMatch) { FirstMatch = Action; }
					++MatchCount;
				}
			}
		}
		if (FirstMatch)
		{
			if (MatchCount > 1)
			{
				UE_LOG(LogMonolithPieInput, Warning,
					TEXT("input_action '%s' matched %d assets — using the first. Pass a full /Game/... path to disambiguate."),
					*Spec, MatchCount);
			}
			return FirstMatch;
		}

		OutError = FString::Printf(TEXT("No UInputAction asset named '%s' found (pass a /Game/... path to load directly)"), *Spec);
		return nullptr;
	}

	// Build an FInputActionValue from a JSON value:
	//   bool        -> Boolean
	//   number      -> Axis1D
	//   array[2]    -> Axis2D
	//   array[3]    -> Axis3D
	// Returns false + a reason on an unsupported shape.
	bool BuildInputActionValue(const TSharedPtr<FJsonValue>& Value, FInputActionValue& OutValue, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("value is missing");
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			OutValue = FInputActionValue(Value->AsBool());
			return true;
		case EJson::Number:
			OutValue = FInputActionValue(static_cast<float>(Value->AsNumber()));
			return true;
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() == 2)
			{
				OutValue = FInputActionValue(FVector2D(Arr[0]->AsNumber(), Arr[1]->AsNumber()));
				return true;
			}
			if (Arr.Num() == 3)
			{
				OutValue = FInputActionValue(FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber()));
				return true;
			}
			OutError = FString::Printf(TEXT("value array must have 2 (Axis2D) or 3 (Axis3D) elements, got %d"), Arr.Num());
			return false;
		}
		default:
			OutError = TEXT("value must be a bool, number, array[2], or array[3]");
			return false;
		}
	}
}

// =============================================================================
//  Registration
// =============================================================================

void FMonolithPieInputActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("pie_set_control_rotation"),
		TEXT("Set the control rotation on a Play-In-Editor player controller (APlayerController::SetControlRotation). "
		     "MUTATES LIVE PIE STATE. Params: pitch / yaw / roll (degrees; omitted components default to 0), optional player_index (default 0). "
		     "hold_frames (default 0) re-applies the rotation each frame for that many frames so it can outlast a per-tick camera/control system that "
		     "re-writes ControlRotation. The hold is BEST-EFFORT, not frame-perfect: a camera director that runs later in the frame can still win for that "
		     "frame, and exact determinism in PIE is not guaranteed. The re-apply hook clears itself on PIE end. No-ops with a clean error when PIE is not running."),
		FMonolithActionHandler::CreateStatic(&HandleSetControlRotation),
		FParamSchemaBuilder()
			.Optional(TEXT("pitch"), TEXT("number"), TEXT("Control-rotation pitch in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("yaw"), TEXT("number"), TEXT("Control-rotation yaw in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("roll"), TEXT("number"), TEXT("Control-rotation roll in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Optional(TEXT("hold_frames"), TEXT("number"), TEXT("Re-apply the rotation each frame for this many frames (default 0 = one-shot). Best-effort vs per-tick camera systems."), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_inject_input_action"),
		TEXT("Inject a value for an Enhanced Input action into a live PIE local player (UEnhancedInputLocalPlayerSubsystem::InjectInputForAction), "
		     "running that action's modifiers and triggers as if real input arrived. MUTATES LIVE PIE STATE. "
		     "input_action is a UInputAction asset path (/Game/...) or short asset name (registry-resolved; first match wins). "
		     "value maps to FInputActionValue by JSON shape: bool -> Boolean, number -> Axis1D, array[2] -> Axis2D, array[3] -> Axis3D. "
		     "Optional player_index (default 0); repeat_frames (default 1) re-injects the value each frame for that many frames. "
		     "No-ops with a clean error when PIE is not running or the action asset cannot be resolved."),
		FMonolithActionHandler::CreateStatic(&HandleInjectInputAction),
		FParamSchemaBuilder()
			.Required(TEXT("input_action"), TEXT("string"), TEXT("UInputAction asset path (/Game/...) or short asset name."))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to inject: bool (Boolean), number (Axis1D), array[2] (Axis2D), or array[3] (Axis3D)."))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Optional(TEXT("repeat_frames"), TEXT("number"), TEXT("Inject the value each frame for this many frames (default 1 = single injection)."), TEXT("1"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_inject_key"),
		TEXT("Inject a named physical FKey into a live PIE game viewport through UGameViewportClient::InputKey using a simulated FInputKeyEventArgs. "
		     "This preserves viewport-client input routing such as UCommonGameViewportClient -> CommonUI and is the raw-key counterpart to pie_inject_input_action. "
		     "MUTATES LIVE PIE STATE. key is an exact registered FKey name such as Escape. event is tap (pressed then released), pressed, or released. "
		     "Optional player_index selects the local player's platform input device. No-ops with a clean error when PIE, the player, the input-device mapping, "
		     "or the PIE game viewport is unavailable."),
		FMonolithActionHandler::CreateStatic(&HandleInjectKey),
		FParamSchemaBuilder()
			.Required(TEXT("key"), TEXT("string"), TEXT("Exact registered FKey name, for example Escape, Enter, or Gamepad_FaceButton_Bottom."))
			.Optional(TEXT("event"), TEXT("string"), TEXT("tap, pressed, or released (default tap)."), TEXT("tap"))
			.Enum(TEXT("event"), {TEXT("tap"), TEXT("pressed"), TEXT("released")})
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0; must be a non-negative integer)."), TEXT("0"))
			.Minimum(TEXT("player_index"), 0.0)
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_start_with_url_options"),
		TEXT("Start canonical in-viewport PIE for the CURRENT editor map with validated Unreal URL options. OFFLINE/SYNCHRONOUS PIE ONLY. "
		     "url_options is a required array of 1-32 option bodies, for example {\"url_options\":[\"TagChaseDirectStart=1\"]}; do not include '?' prefixes. "
		     "Each entry is capped at 256 characters, may not contain control characters, '?' or '#', and its key must match "
		     "[A-Za-z_][A-Za-z0-9_.-]* with no case-insensitive duplicate keys. Before mutation/start, the action rejects IsPlaySessionInProgress, "
		     "queued request, session-info, or active PIE-world state, "
		     "and the UE 5.8 asynchronous Online PIE login predicate (Online PIE supported and desired clients <= available PIE logins). It constructs the current map package URL, "
		     "sets UEditorEngine::UserEditedPlayWorldURL only for the synchronous offline canonical start request, restores it immediately, "
		     "and returns the requested URL plus the actual PIE world URL/options when the world is available."),
		FMonolithActionHandler::CreateStatic(&HandleStartWithUrlOptions),
		FParamSchemaBuilder()
			.Required(TEXT("url_options"), TEXT("array"), TEXT("1-32 Unreal URL option bodies, for example [\"TagChaseDirectStart=1\"]; no '?' prefix."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_possess_spectator_free"),
		TEXT("Detach a PIE player controller to a free-fly spectator pawn for camera-independent observation, or re-possess the original pawn. "
		     "MUTATES LIVE PIE STATE. enable=true stores the currently-possessed pawn, unpossesses it, and switches the controller to the Spectating state "
		     "(APlayerController::ChangeState(NAME_Spectating)) which spawns a spectator pawn from the game mode's SpectatorClass. enable=false re-possesses the "
		     "stored original pawn (ChangeState(NAME_Playing) + Possess). The original pawn is held as a weak pointer and cleared on PIE end. "
		     "No-ops with a clean error when PIE is not running. Note: spectator spawning depends on the game mode providing a SpectatorClass; if none is "
		     "configured the controller still enters the Spectating state but no free-fly pawn is created."),
		FMonolithActionHandler::CreateStatic(&HandlePossessSpectatorFree),
		FParamSchemaBuilder()
			.Required(TEXT("enable"), TEXT("bool"), TEXT("true = detach to a free-fly spectator; false = re-possess the original pawn."))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Build());

	UE_LOG(LogMonolithPieInput, Log, TEXT("MonolithEditor: registered 5 PIE input/control actions"));
}

// =============================================================================
//  pie_set_control_rotation
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandleSetControlRotation(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	const int32 PlayerIndex = ReadPlayerIndexLenient(Params);

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	const FRotator Rotation = ReadRotator(Params);

	int32 HoldFrames = 0;
	{
		double Frames = 0.0;
		if (Params->TryGetNumberField(TEXT("hold_frames"), Frames)) { HoldFrames = FMath::Max(0, static_cast<int32>(Frames)); }
	}

	// Apply once immediately, then queue the per-frame re-apply (if any).
	PC->SetControlRotation(Rotation);
	if (HoldFrames > 0)
	{
		FPieInputHoldRegistry::Get().SetRotationHold(PlayerIndex, Rotation, HoldFrames);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	Root->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	Root->SetNumberField(TEXT("roll"), Rotation.Roll);
	Root->SetNumberField(TEXT("hold_frames"), HoldFrames);
	Root->SetBoolField(TEXT("applied"), true);
	Root->SetBoolField(TEXT("best_effort_hold"), HoldFrames > 0);
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_inject_input_action
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandleInjectInputAction(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	FString ActionSpec;
	if (!Params->TryGetStringField(TEXT("input_action"), ActionSpec) || ActionSpec.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("pie_inject_input_action requires 'input_action' (asset path or name)"));
	}

	FString ResolveError;
	const UInputAction* Action = ResolveInputAction(ActionSpec, ResolveError);
	if (!Action)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	FInputActionValue ActionValue;
	FString ValueError;
	if (!BuildInputActionValue(ValueField, ActionValue, ValueError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("pie_inject_input_action 'value' invalid: %s"), *ValueError));
	}

	const int32 PlayerIndex = ReadPlayerIndexLenient(Params);

	int32 RepeatFrames = 1;
	{
		double Frames = 1.0;
		if (Params->TryGetNumberField(TEXT("repeat_frames"), Frames)) { RepeatFrames = FMath::Max(1, static_cast<int32>(Frames)); }
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystemFromController<UEnhancedInputLocalPlayerSubsystem>(PC);
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Player %d has no UEnhancedInputLocalPlayerSubsystem (is Enhanced Input the active input system?)"), PlayerIndex));
	}

	// Inject once now; queue any additional per-frame injections.
	Subsystem->InjectInputForAction(Action, ActionValue, {}, {});
	if (RepeatFrames > 1)
	{
		FPieInputHoldRegistry::Get().SetInputHold(PlayerIndex, Action, ActionValue, RepeatFrames - 1);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("input_action"), Action->GetPathName());
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetNumberField(TEXT("repeat_frames"), RepeatFrames);
	Root->SetBoolField(TEXT("injected"), true);
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_inject_key
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandleInjectKey(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(
			TEXT("pie_inject_key requires a parameter object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString KeySpec;
	if (!Params->TryGetStringField(TEXT("key"), KeySpec) || KeySpec.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("pie_inject_key requires non-empty string parameter 'key'"), FMonolithJsonUtils::ErrInvalidParams);
	}
	KeySpec = KeySpec.TrimStartAndEnd();
	const FKey Key(*KeySpec);
	if (!Key.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("pie_inject_key key '%s' is not a registered FKey name"), *KeySpec),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FString EventSpec = TEXT("tap");
	if (Params->HasField(TEXT("event")) && !Params->TryGetStringField(TEXT("event"), EventSpec))
	{
		return FMonolithActionResult::Error(
			TEXT("pie_inject_key parameter 'event' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	EventSpec = EventSpec.TrimStartAndEnd().ToLower();
	if (EventSpec != TEXT("tap") && EventSpec != TEXT("pressed") && EventSpec != TEXT("released"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("pie_inject_key event '%s' is invalid; expected tap, pressed, or released"), *EventSpec),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 PlayerIndex = 0;
	FString PlayerIndexError;
	if (!TryReadPlayerIndexStrict(Params, TEXT("pie_inject_key"), PlayerIndex, PlayerIndexError))
	{
		return FMonolithActionResult::Error(PlayerIndexError, FMonolithJsonUtils::ErrInvalidParams);
	}

	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	const FPlatformUserId PlatformUser = PC->GetPlatformUserId();
	if (!PlatformUser.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Player %d has no valid platform-user mapping for PIE key injection"), PlayerIndex));
	}

	const FInputDeviceId InputDevice =
		IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PlatformUser);
	if (!InputDevice.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Player %d platform user %d has no primary input-device mapping"),
			PlayerIndex, PlatformUser.GetInternalId()));
	}

	UGameViewportClient* GameViewport = PieWorld->GetGameViewport();
	if (!GameViewport || !GameViewport->Viewport)
	{
		return FMonolithActionResult::Error(TEXT("The running PIE world has no game viewport available for key injection"));
	}

	auto InjectEvent = [GameViewport, InputDevice, &Key](EInputEvent InputEvent)
	{
		const float AmountDepressed = InputEvent == IE_Released ? 0.0f : 1.0f;
		return GameViewport->InputKey(FInputKeyEventArgs::CreateSimulated(
			Key,
			InputEvent,
			AmountDepressed,
			/*InNumSamplesOverride=*/1,
			InputDevice,
			/*bIsTouchEvent=*/false,
			GameViewport->Viewport));
	};

	bool bPressedSent = false;
	bool bReleasedSent = false;
	bool bPressedHandled = false;
	bool bReleasedHandled = false;
	if (EventSpec == TEXT("tap") || EventSpec == TEXT("pressed"))
	{
		bPressedSent = true;
		bPressedHandled = InjectEvent(IE_Pressed);
	}
	if (EventSpec == TEXT("tap") || EventSpec == TEXT("released"))
	{
		bReleasedSent = true;
		bReleasedHandled = InjectEvent(IE_Released);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("key"), Key.ToString());
	Root->SetStringField(TEXT("event"), EventSpec);
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetNumberField(TEXT("platform_user_id"), PlatformUser.GetInternalId());
	Root->SetNumberField(TEXT("input_device_id"), InputDevice.GetId());
	Root->SetStringField(TEXT("route"), TEXT("UGameViewportClient::InputKey"));
	Root->SetBoolField(TEXT("pressed_sent"), bPressedSent);
	Root->SetBoolField(TEXT("pressed_handled"), bPressedHandled);
	Root->SetBoolField(TEXT("released_sent"), bReleasedSent);
	Root->SetBoolField(TEXT("released_handled"), bReleasedHandled);
	Root->SetBoolField(TEXT("handled"), bPressedHandled || bReleasedHandled);
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_start_with_url_options
// =============================================================================

bool FMonolithPieInputActions::WouldUseAsyncOnlinePieLogin(
	const int32 DesiredNumberOfClients,
	const bool bSupportsOnlinePie,
	const int32 NumAvailablePieLogins)
{
	// Mirrors UEditorEngine::StartQueuedPlaySessionRequest in UE 5.8 for the canonical
	// HandleStartPIE request: PlayInEditor world type and bAllowOnlineSubsystem=true.
	return bSupportsOnlinePie && DesiredNumberOfClients <= NumAvailablePieLogins;
}

bool FMonolithPieInputActions::HasExistingPieStartConflict(
	const bool bPlaySessionInProgress,
	const bool bPlaySessionRequestSet,
	const bool bPlayInEditorSessionInfoSet,
	const bool bHasActivePieWorld)
{
	return bPlaySessionInProgress
		|| bPlaySessionRequestSet
		|| bPlayInEditorSessionInfoSet
		|| bHasActivePieWorld;
}

FMonolithActionResult FMonolithPieInputActions::HandleStartWithUrlOptions(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> UrlOptions;
	FString ValidationError;
	if (!ValidatePieUrlOptions(Params, UrlOptions, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, FMonolithJsonUtils::ErrInvalidParams);
	}

	// Validation deliberately precedes every editor/PIE lookup so malformed requests can
	// never load a world or start a play session.
	if (!GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("GUnrealEd not available"));
	}

	const bool bPlaySessionInProgress = GUnrealEd->IsPlaySessionInProgress();
	const bool bHasPlaySessionRequest = GUnrealEd->GetPlaySessionRequest().IsSet();
	const bool bHasPlayInEditorSessionInfo = GUnrealEd->GetPlayInEditorSessionInfo().IsSet();
	const bool bHasActivePieWorld = GetPieWorld() != nullptr;
	if (HasExistingPieStartConflict(
		bPlaySessionInProgress,
		bHasPlaySessionRequest,
		bHasPlayInEditorSessionInfo,
		bHasActivePieWorld))
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("reason"), TEXT("pie_start_conflict"));
		ErrorData->SetBoolField(TEXT("play_session_in_progress"), bPlaySessionInProgress);
		ErrorData->SetBoolField(TEXT("play_session_request_set"), bHasPlaySessionRequest);
		ErrorData->SetBoolField(TEXT("play_in_editor_session_info_set"), bHasPlayInEditorSessionInfo);
		ErrorData->SetBoolField(TEXT("active_pie_world"), bHasActivePieWorld);
		ErrorData->SetBoolField(TEXT("user_edited_play_world_url_mutated"), false);
		ErrorData->SetBoolField(TEXT("pie_start_requested"), false);
		return FMonolithActionResult::Error(
			TEXT("pie_start_with_url_options cannot start while another PIE request, PIE session, or PIE world exists; request rejected before URL override/start"))
			.WithErrorData(ErrorData);
	}

	const ULevelEditorPlaySettings* DefaultPlaySettings = GetDefault<ULevelEditorPlaySettings>();
	if (!DefaultPlaySettings)
	{
		return FMonolithActionResult::Error(
			TEXT("Default ULevelEditorPlaySettings are unavailable; request rejected before URL override/start"));
	}

	UOnlineEngineInterface* OnlineEngineInterface = UOnlineEngineInterface::Get();
	if (!OnlineEngineInterface)
	{
		return FMonolithActionResult::Error(
			TEXT("UOnlineEngineInterface is unavailable; request rejected before URL override/start"));
	}

	int32 DesiredNumberOfClients = 0;
	DefaultPlaySettings->GetPlayNumberOfClients(DesiredNumberOfClients);
	const bool bSupportsOnlinePie = GUnrealEd->SupportsOnlinePIE();
	const int32 NumAvailablePieLogins = OnlineEngineInterface->GetNumPIELogins();
	if (WouldUseAsyncOnlinePieLogin(DesiredNumberOfClients, bSupportsOnlinePie, NumAvailablePieLogins))
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("reason"), TEXT("async_online_pie_unsupported"));
		ErrorData->SetNumberField(TEXT("desired_number_of_clients"), DesiredNumberOfClients);
		ErrorData->SetBoolField(TEXT("supports_online_pie"), bSupportsOnlinePie);
		ErrorData->SetNumberField(TEXT("available_pie_logins"), NumAvailablePieLogins);
		ErrorData->SetBoolField(TEXT("user_edited_play_world_url_mutated"), false);
		ErrorData->SetBoolField(TEXT("pie_start_requested"), false);
		return FMonolithActionResult::Error(
			TEXT("pie_start_with_url_options supports only offline/synchronous PIE; UE 5.8 would use asynchronous Online PIE authentication, so the request was rejected before URL override/start"))
			.WithErrorData(ErrorData);
	}

	UWorld* EditorWorld = GUnrealEd->GetEditorWorldContext().World();
	if (!EditorWorld || !EditorWorld->GetOutermost())
	{
		return FMonolithActionResult::Error(TEXT("No current editor world is available for PIE URL construction"));
	}

	const FString EditorMapPackage = EditorWorld->GetOutermost()->GetName();
	if (EditorMapPackage.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("The current editor world has no map package name"));
	}

	FURL RequestedUrl(nullptr, *EditorMapPackage, TRAVEL_Absolute);
	for (const FString& Option : UrlOptions)
	{
		RequestedUrl.AddOption(*Option);
	}
	const FString RequestedUrlString = RequestedUrl.ToString();
	const FString PreviousUserEditedUrl = GUnrealEd->UserEditedPlayWorldURL;

	FMonolithActionResult StartResult;
	{
		// The fail-closed preflight above excludes UE 5.8's async Online PIE login branch.
		// Its remaining offline branch invokes OnLoginPIEComplete_Deferred inline, which
		// consumes UserEditedPlayWorldURL inside this canonical call before the guard restores it.
		TGuardValue<FString> UrlGuard(GUnrealEd->UserEditedPlayWorldURL, RequestedUrlString);
		StartResult = FMonolithEditorActions::HandleStartPIE(MakeShared<FJsonObject>());
	}

	if (!StartResult.bSuccess)
	{
		return StartResult;
	}
	if (!StartResult.Result.IsValid())
	{
		StartResult.Result = MakeShared<FJsonObject>();
	}

	StartResult.Result->SetStringField(TEXT("editor_map_package"), EditorMapPackage);
	StartResult.Result->SetStringField(TEXT("requested_pie_url"), RequestedUrlString);
	StartResult.Result->SetArrayField(TEXT("requested_url_options"), StringArrayToJson(UrlOptions));
	StartResult.Result->SetStringField(TEXT("start_route"), TEXT("FMonolithEditorActions::HandleStartPIE"));
	StartResult.Result->SetStringField(TEXT("url_override_lifecycle"), TEXT("offline_synchronous_call_scope"));
	StartResult.Result->SetNumberField(TEXT("desired_number_of_clients"), DesiredNumberOfClients);
	StartResult.Result->SetBoolField(TEXT("supports_online_pie"), bSupportsOnlinePie);
	StartResult.Result->SetNumberField(TEXT("available_pie_logins"), NumAvailablePieLogins);
	StartResult.Result->SetBoolField(TEXT("online_pie_login_selected"), false);
	StartResult.Result->SetBoolField(
		TEXT("user_edited_play_world_url_restored"),
		GUnrealEd->UserEditedPlayWorldURL == PreviousUserEditedUrl);
	StartResult.Result->SetBoolField(TEXT("actual_pie_url_available"), false);

	bool bStarted = false;
	StartResult.Result->TryGetBoolField(TEXT("started"), bStarted);
	if (bStarted)
	{
		if (UWorld* PieWorld = GetPieWorld())
		{
			StartResult.Result->SetBoolField(TEXT("actual_pie_url_available"), true);
			StartResult.Result->SetStringField(TEXT("actual_pie_url"), PieWorld->URL.ToString());
			StartResult.Result->SetArrayField(TEXT("actual_pie_url_options"), StringArrayToJson(PieWorld->URL.Op));
		}
	}

	return StartResult;
}

// =============================================================================
//  pie_possess_spectator_free
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandlePossessSpectatorFree(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	bool bEnable = false;
	if (!Params->TryGetBoolField(TEXT("enable"), bEnable))
	{
		return FMonolithActionResult::Error(TEXT("pie_possess_spectator_free requires 'enable' (bool)"));
	}

	const int32 PlayerIndex = ReadPlayerIndexLenient(Params);

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetBoolField(TEXT("enable"), bEnable);

	if (bEnable)
	{
		// Store the currently-possessed pawn for reversal, then detach to a spectator.
		APawn* OriginalPawn = PC->GetPawn();
		FPieSpectatorRegistry::Get().StoreOriginalPawn(PlayerIndex, OriginalPawn);

		// ChangeState(NAME_Spectating) routes through BeginSpectatingState(), which
		// unpossesses the current pawn and spawns a spectator pawn (from GameState->SpectatorClass).
		PC->ChangeState(NAME_Spectating);

		const APawn* Spectator = PC->GetSpectatorPawn();
		Root->SetStringField(TEXT("original_pawn"), OriginalPawn ? OriginalPawn->GetName() : TEXT("None"));
		Root->SetStringField(TEXT("spectator_pawn"), Spectator ? Spectator->GetName() : TEXT("None"));
		Root->SetBoolField(TEXT("spectating"), PC->IsInState(NAME_Spectating));
		if (!Spectator)
		{
			Root->SetStringField(TEXT("note"),
				TEXT("Controller entered the Spectating state but no spectator pawn was spawned — the game mode likely has no SpectatorClass configured."));
		}
		return FMonolithActionResult::Success(Root);
	}

	// Disable: re-possess the stored original pawn.
	APawn* OriginalPawn = FPieSpectatorRegistry::Get().TakeOriginalPawn(PlayerIndex);
	if (!OriginalPawn)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No stored original pawn for player_index %d — was pie_possess_spectator_free called with enable=true first? (the pawn may also have been destroyed)"),
			PlayerIndex));
	}

	// Leave the Spectating state, then re-possess. ChangeState(NAME_Playing) tears down the
	// spectator pawn; Possess re-attaches the controller to the original pawn.
	PC->ChangeState(NAME_Playing);
	PC->Possess(OriginalPawn);

	Root->SetStringField(TEXT("repossessed_pawn"), OriginalPawn->GetName());
	Root->SetBoolField(TEXT("spectating"), PC->IsInState(NAME_Spectating));
	return FMonolithActionResult::Success(Root);
}
