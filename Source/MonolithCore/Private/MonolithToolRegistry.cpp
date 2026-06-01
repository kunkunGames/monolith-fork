#include "MonolithToolRegistry.h"
#include "MonolithFuzzyMatch.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithCrashBreadcrumb.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolProfileManager.h"
#include "HAL/PlatformMisc.h"

// =============================================================================
//  File-local helpers. FindSimilarActions edit distance now lives in
//  FMonolithFuzzyMatch::EditDistanceBounded (MonolithFuzzyMatch.h).
// =============================================================================
namespace
{
	class FScopedEnvironmentVar
	{
	public:
		FScopedEnvironmentVar(const TCHAR* InName, const FString& InValue)
			: Name(InName)
			, PreviousValue(FPlatformMisc::GetEnvironmentVariable(InName))
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *InValue);
		}

		~FScopedEnvironmentVar()
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *PreviousValue);
		}

	private:
		FString Name;
		FString PreviousValue;
	};

	bool StartsWithAnyActionVerb(const FString& Action, const TArray<FString>& Verbs)
	{
		for (const FString& Verb : Verbs)
		{
			if (Action == Verb
				|| Action.StartsWith(Verb + TEXT("_"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsReadLikeActionName(const FString& Action)
	{
		static const TArray<FString> ReadVerbs =
		{
			TEXT("get"),
			TEXT("list"),
			TEXT("find"),
			TEXT("search"),
			TEXT("read"),
			TEXT("validate"),
			TEXT("preview"),
			TEXT("can"),
			TEXT("describe"),
			TEXT("detect"),
			TEXT("analyze"),
			TEXT("compare"),
			TEXT("check"),
			TEXT("health"),
			TEXT("status"),
			TEXT("diff"),
			TEXT("review"),
			TEXT("inspect"),
			TEXT("estimate"),
			TEXT("explain"),
			TEXT("query"),
			TEXT("resolve"),
			TEXT("is"),
			TEXT("has")
		};
		return StartsWithAnyActionVerb(Action, ReadVerbs);
	}

	bool JsonValueMatchesSchemaType(const TSharedPtr<FJsonValue>& Value, const FString& Type)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Type == TEXT("string"))
		{
			return Value->Type == EJson::String;
		}
		if (Type == TEXT("number"))
		{
			return Value->Type == EJson::Number;
		}
		if (Type == TEXT("integer"))
		{
			double Number = 0.0;
			return Value->Type == EJson::Number
				&& Value->TryGetNumber(Number)
				&& FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
		}
		if (Type == TEXT("boolean") || Type == TEXT("bool"))
		{
			return Value->Type == EJson::Boolean;
		}
		if (Type == TEXT("object"))
		{
			return Value->Type == EJson::Object;
		}
		if (Type == TEXT("array"))
		{
			return Value->Type == EJson::Array;
		}

		return true;
	}

	bool JsonValueMatchesSchemaTypes(const TSharedPtr<FJsonValue>& Value, const FString& TypeSpec)
	{
		TArray<FString> Types;
		TypeSpec.ParseIntoArray(Types, TEXT("|"), true);
		for (FString Type : Types)
		{
			Type.TrimStartAndEndInline();
			Type.ToLowerInline();
			if (JsonValueMatchesSchemaType(Value, Type))
			{
				return true;
			}
		}
		return Types.Num() == 0;
	}

	FMonolithActionExecutionPolicy MakeInferredMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_optional");
		Policy.bDefaulted = true;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}
}

// =============================================================================
//  FMonolithParamSchema — K2 alias rewriting + K3 unknown-key detection
// =============================================================================

bool FMonolithParamSchema::ApplyAliases(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutCollision)
{
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	for (const auto& Pair : Schema->Values)
	{
		const FString& Canonical = Pair.Key;

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if (!(*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) || !AliasArr)
		{
			continue;
		}

		const bool bCanonicalPresent = Params->HasField(Canonical);

		for (const TSharedPtr<FJsonValue>& AliasVal : *AliasArr)
		{
			FString Alias;
			if (!AliasVal.IsValid() || !AliasVal->TryGetString(Alias))
			{
				continue;
			}

			if (!Params->HasField(Alias))
			{
				continue;
			}

			if (bCanonicalPresent)
			{
				OutCollision = FString::Printf(
					TEXT("Param collision: both canonical '%s' and alias '%s' supplied. Use only one. — supply either the canonical param OR its alias, never both."),
					*Canonical, *Alias);
				return false;
			}

			// Rewrite alias -> canonical (preserve value).
			TSharedPtr<FJsonValue> Val = Params->TryGetField(Alias);
			if (Val.IsValid())
			{
				Params->SetField(Canonical, Val);
			}
			Params->RemoveField(Alias);
			break; // Only one alias rewrite per canonical.
		}
	}

	return true;
}

TArray<FString> FMonolithParamSchema::FindUnknownKeys(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Unknown;
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return Unknown;
	}

	// Build the set of allowed keys: canonical names + their declared aliases.
	TSet<FString> Allowed;
	Allowed.Reserve(Schema->Values.Num());
	for (const auto& Pair : Schema->Values)
	{
		Allowed.Add(Pair.Key);

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr)
		{
			for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
			{
				FString A;
				if (AV.IsValid() && AV->TryGetString(A))
				{
					Allowed.Add(A);
				}
			}
		}
	}

	// Legacy wbp_path/asset_path back-compat: allow asset_path everywhere.
	Allowed.Add(TEXT("asset_path"));

	for (const auto& Pair : Params->Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			Unknown.Add(Pair.Key);
		}
	}

	return Unknown;
}

bool FMonolithParamSchema::ValidateTypedParams(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutErrors)
{
	OutErrors.Reset();
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	bool bValidateTypes = false;
	if (!Schema->TryGetBoolField(TEXT("_validate_types"), bValidateTypes) || !bValidateTypes)
	{
		return true;
	}

	for (const auto& Pair : Schema->Values)
	{
		if (Pair.Key.StartsWith(TEXT("_")))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		TSharedPtr<FJsonValue> ParamValue = Params->TryGetField(Pair.Key);
		if (!ParamValue.IsValid())
		{
			continue;
		}

		FString TypeSpec;
		if ((*ParamDef)->TryGetStringField(TEXT("type"), TypeSpec)
			&& !JsonValueMatchesSchemaTypes(ParamValue, TypeSpec))
		{
			OutErrors.Add(FString::Printf(TEXT("Invalid param '%s': expected %s."), *Pair.Key, *TypeSpec));
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues)
		{
			FString ActualValue;
			if (ParamValue->TryGetString(ActualValue))
			{
				TArray<FString> AllowedValues;
				for (const TSharedPtr<FJsonValue>& EnumValue : *EnumValues)
				{
					FString Allowed;
					if (EnumValue.IsValid() && EnumValue->TryGetString(Allowed))
					{
						AllowedValues.Add(Allowed);
					}
				}

				if (AllowedValues.Num() > 0 && !AllowedValues.Contains(ActualValue))
				{
					OutErrors.Add(FString::Printf(
						TEXT("Invalid param '%s': value '%s' must be one of [%s]."),
						*Pair.Key,
						*ActualValue,
						*FString::Join(AllowedValues, TEXT(", "))));
				}
			}
		}

		double NumberValue = 0.0;
		const bool bHasNumber = ParamValue->TryGetNumber(NumberValue);
		double MinValue = 0.0;
		if (bHasNumber && (*ParamDef)->TryGetNumberField(TEXT("minimum"), MinValue) && NumberValue < MinValue)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Invalid param '%s': value must be >= %s."),
				*Pair.Key,
				*FString::SanitizeFloat(MinValue)));
		}

		double MaxValue = 0.0;
		if (bHasNumber && (*ParamDef)->TryGetNumberField(TEXT("maximum"), MaxValue) && NumberValue > MaxValue)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Invalid param '%s': value must be <= %s."),
				*Pair.Key,
				*FString::SanitizeFloat(MaxValue)));
		}
	}

	return OutErrors.Num() == 0;
}

bool FMonolithParamSchema::IsStrictParamsEnabled()
{
	const FString Val = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	return Val == TEXT("1");
}

// =============================================================================
//  FMonolithToolRegistry
// =============================================================================

FMonolithActionExecutionPolicy FMonolithActionExecutionPolicy::DefaultReadOnly()
{
	FMonolithActionExecutionPolicy Policy;
	Policy.PolicyId = TEXT("read_only");
	Policy.bDefaulted = true;
	Policy.bDirtyPackageTracking = false;
	Policy.bTransactionWrapping = false;
	Policy.bPostEditValidation = false;
	Policy.bEnforced = false;
	return Policy;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionPolicy::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("policy_id"), PolicyId.IsEmpty() ? TEXT("read_only") : PolicyId);
	Obj->SetBoolField(TEXT("defaulted"), bDefaulted);
	Obj->SetBoolField(TEXT("dirty_package_tracking"), bDirtyPackageTracking);
	Obj->SetBoolField(TEXT("transaction_wrapping"), bTransactionWrapping);
	Obj->SetBoolField(TEXT("post_edit_validation"), bPostEditValidation);
	Obj->SetBoolField(TEXT("enforced"), bEnforced);
	return Obj;
}

FMonolithActionExecutionPolicy FMonolithToolRegistry::InferExecutionPolicy(
	const FString& Namespace,
	const FString& Action,
	const FMonolithActionExecutionPolicy& RequestedPolicy)
{
	const bool bLooksLikeImplicitDefault =
		RequestedPolicy.bDefaulted
		&& (RequestedPolicy.PolicyId.IsEmpty() || RequestedPolicy.PolicyId == TEXT("read_only"))
		&& !RequestedPolicy.bDirtyPackageTracking
		&& !RequestedPolicy.bTransactionWrapping
		&& !RequestedPolicy.bPostEditValidation
		&& !RequestedPolicy.bEnforced;

	if (!bLooksLikeImplicitDefault)
	{
		return RequestedPolicy;
	}

	if (Namespace == TEXT("policytest"))
	{
		return RequestedPolicy;
	}

	if (IsReadLikeActionName(Action))
	{
		return RequestedPolicy;
	}

	return MakeInferredMutationPolicy();
}

FMonolithToolRegistry& FMonolithToolRegistry::Get()
{
	static FMonolithToolRegistry Instance;
	return Instance;
}

void FMonolithToolRegistry::RegisterAction(
	const FString& Namespace,
	const FString& Action,
	const FString& Description,
	const FMonolithActionHandler& Handler,
	const TSharedPtr<FJsonObject>& ParamSchema,
	const FString& Category,
	const FMonolithActionExecutionPolicy& ExecutionPolicy,
	const FMonolithActionSearchMetadata& SearchMetadata)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);

	if (Actions.Contains(Key))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Overwriting existing action: %s"), *Key);
	}

	FRegisteredAction RegAction;
	RegAction.Info.Namespace = Namespace;
	RegAction.Info.Action = Action;
	RegAction.Info.Description = Description;
	RegAction.Info.Category = Category;
	RegAction.Info.ExecutionPolicy = InferExecutionPolicy(Namespace, Action, ExecutionPolicy);
	RegAction.Info.SearchMetadata = SearchMetadata;
	RegAction.Info.ParamSchema = ParamSchema;
	RegAction.Handler = Handler;
	if (RegistrationOwnerStack.Num() > 0)
	{
		RegAction.Owner = RegistrationOwnerStack.Last();
	}

	Actions.Add(Key, MoveTemp(RegAction));
	NamespaceActions.FindOrAdd(Namespace).AddUnique(Key);

	UE_LOG(LogMonolith, Verbose, TEXT("Registered action: %s — %s"), *Key, *Description);
}

void FMonolithToolRegistry::RegisterOwnedActions(const FString& Owner, TFunctionRef<void(FMonolithToolRegistry&)> Register)
{
	if (Owner.IsEmpty())
	{
		Register(*this);
		return;
	}

	{
		FScopeLock Lock(&RegistryLock);
		RegistrationOwnerStack.Add(Owner);
	}

	Register(*this);

	{
		FScopeLock Lock(&RegistryLock);
		if (RegistrationOwnerStack.Num() > 0 && RegistrationOwnerStack.Last() == Owner)
		{
			RegistrationOwnerStack.RemoveAt(RegistrationOwnerStack.Num() - 1, 1, EAllowShrinking::No);
		}
		else
		{
			RegistrationOwnerStack.RemoveSingle(Owner);
		}
	}
}

bool FMonolithToolRegistry::UnregisterActionByKey_NoLock(const FString& Key)
{
	FRegisteredAction RemovedAction;
	if (!Actions.RemoveAndCopyValue(Key, RemovedAction))
	{
		return false;
	}

	if (TArray<FString>* Keys = NamespaceActions.Find(RemovedAction.Info.Namespace))
	{
		Keys->Remove(Key);
		if (Keys->Num() == 0)
		{
			NamespaceActions.Remove(RemovedAction.Info.Namespace);
		}
	}

	return true;
}

bool FMonolithToolRegistry::UnregisterAction(const FString& Namespace, const FString& Action)
{
	FScopeLock Lock(&RegistryLock);
	const FString Key = MakeKey(Namespace, Action);
	const bool bRemoved = UnregisterActionByKey_NoLock(Key);
	if (bRemoved)
	{
		UE_LOG(LogMonolith, Verbose, TEXT("Unregistered action: %s"), *Key);
	}
	return bRemoved;
}

int32 FMonolithToolRegistry::UnregisterOwner(const FString& Owner)
{
	if (Owner.IsEmpty())
	{
		return 0;
	}

	FScopeLock Lock(&RegistryLock);

	TArray<FString> KeysToRemove;
	for (const auto& Pair : Actions)
	{
		if (Pair.Value.Owner == Owner)
		{
			KeysToRemove.Add(Pair.Key);
		}
	}

	for (const FString& Key : KeysToRemove)
	{
		UnregisterActionByKey_NoLock(Key);
	}

	if (KeysToRemove.Num() > 0)
	{
		UE_LOG(LogMonolith, Log, TEXT("Unregistered owner: %s (%d actions)"), *Owner, KeysToRemove.Num());
	}
	return KeysToRemove.Num();
}

void FMonolithToolRegistry::UnregisterNamespace(const FString& Namespace)
{
	FScopeLock Lock(&RegistryLock);

	if (TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			Actions.Remove(Key);
		}
		UE_LOG(LogMonolith, Log, TEXT("Unregistered namespace: %s (%d actions)"), *Namespace, Keys->Num());
		NamespaceActions.Remove(Namespace);
	}
}

FMonolithActionResult FMonolithToolRegistry::ExecuteAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	const FString LogStartTime = FMonolithToolInvocationLogger::NowIso8601WithOffset();
	const double LogStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	const FString ExistingTraceId = FMonolithToolInvocationLogger::GetCurrentTraceId();
	const FString ActionTraceId = ExistingTraceId.IsEmpty()
		? FMonolithToolInvocationLogger::GenerateTraceId(Namespace + TEXT(":") + Action + TEXT(":") + LogStartTime)
		: ExistingTraceId;
	const FString ExistingParentSpanId = FMonolithToolInvocationLogger::GetCurrentParentSpanId();
	const FString ActionSpanId = FMonolithToolInvocationLogger::GenerateSpanId(ActionTraceId + TEXT(":action:") + Namespace + TEXT(":") + Action + TEXT(":") + LogStartTime);
	FMonolithToolInvocationLogger::FScopedTrace ActionTraceScope(
		ActionTraceId,
		ExistingParentSpanId,
		ActionSpanId,
		FMonolithToolInvocationLogger::GetCurrentSessionKey(),
		FMonolithToolInvocationLogger::GetCurrentRoutingContext());
	FMonolithToolInvocationLogger::ClearCurrentChildProcess();
	TSharedPtr<FJsonObject> PhaseTiming = MakeShared<FJsonObject>();
	auto SetPhaseMs = [PhaseTiming](const TCHAR* Field, double PhaseStartSeconds)
	{
		if (PhaseTiming.IsValid())
		{
			PhaseTiming->SetNumberField(Field, (FMonolithToolInvocationLogger::NowSeconds() - PhaseStartSeconds) * 1000.0);
		}
	};
	const double LookupStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	auto RecordAndReturn = [&](const FMonolithActionResult& Result, const FString& ValidationPhase, const TSharedPtr<FJsonObject>& LogParams) -> FMonolithActionResult
	{
		FMonolithToolInvocationLogger::RecordAction(
			Namespace,
			Action,
			LogParams.IsValid() ? LogParams : MakeShared<FJsonObject>(),
			Result,
			ValidationPhase,
			LogStartTime,
			LogStartSeconds,
			PhaseTiming);
		return Result;
	};

	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);
	FRegisteredAction* RegAction = Actions.Find(Key);

	if (!RegAction)
	{
		// CC-05: surface "did you mean" suggestions for the agent so it can
		// recover in one round-trip instead of guessing iteratively.
		// Drop the lock before scoring (FindSimilarActions takes the lock again).
		Lock.Unlock();

		TArray<FString> Similar = FindSimilarActions(Namespace, Action, /*MaxResults=*/5);
		SetPhaseMs(TEXT("lookup_ms"), LookupStartSeconds);

		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown action: %s.%s — call monolith_discover(\"%s\") to enumerate valid actions in this namespace."), *Namespace, *Action, *Namespace),
			FMonolithJsonUtils::ErrMethodNotFound
		);
		R.RelatedActions = MoveTemp(Similar);
		if (R.RelatedActions.Num() == 0)
		{
			// No close matches — guide the agent to discovery.
			R.Hints.Add(FString::Printf(
				TEXT("Use monolith_discover(\"%s\") to list available actions."), *Namespace));
		}
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("malformed_dispatch"),
			R.ErrorCode,
			R.ErrorMessage);
		return RecordAndReturn(R, TEXT("lookup"), Params);
	}

	SetPhaseMs(TEXT("lookup_ms"), LookupStartSeconds);
	const double ProfileStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (!FMonolithToolProfileManager::Get().IsActionAllowed(Namespace, Action))
	{
		SetPhaseMs(TEXT("profile_ms"), ProfileStartSeconds);
		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Action '%s.%s' is disabled by the active Monolith tool profile '%s'."),
				*Namespace,
				*Action,
				*FMonolithToolProfileManager::Get().GetActiveProfileId()),
			FMonolithJsonUtils::ErrInvalidRequest);
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("profile_blocked"),
			R.ErrorCode,
			R.ErrorMessage);
		Lock.Unlock();
		return RecordAndReturn(R, TEXT("profile"), Params);
	}
	SetPhaseMs(TEXT("profile_ms"), ProfileStartSeconds);

	if (!RegAction->Handler.IsBound())
	{
		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Action handler not bound: %s — this is a Monolith bug; the action is registered but its handler delegate is null. Report at github.com/tumourlove/monolith."), *Key),
			FMonolithJsonUtils::ErrInternalError
		);
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("error"),
			R.ErrorCode,
			R.ErrorMessage);
		Lock.Unlock();
		return RecordAndReturn(R, TEXT("lookup"), Params);
	}

	const FMonolithActionInfo& ActionInfo = RegAction->Info;
	TSharedPtr<FJsonObject> EffectiveParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();

	// K2 — alias rewriting BEFORE the required-param check.
	const double AliasStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (ActionInfo.ParamSchema.IsValid())
	{
		FString Collision;
		if (!FMonolithParamSchema::ApplyAliases(ActionInfo.ParamSchema, EffectiveParams, Collision))
		{
			SetPhaseMs(TEXT("alias_ms"), AliasStartSeconds);
			FMonolithActionResult R = FMonolithActionResult::Error(Collision, FMonolithJsonUtils::ErrInvalidParams);
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}
	if (ActionInfo.ParamSchema.IsValid())
	{
		SetPhaseMs(TEXT("alias_ms"), AliasStartSeconds);
	}

	// Validate required params from schema before dispatching.
	// Skip asset_path — GetAssetPath() accepts both asset_path and system_path aliases
	// and produces a clear error message itself.
	const double SchemaStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (ActionInfo.ParamSchema.IsValid())
	{
		TArray<FString> Missing;
		for (const auto& Pair : ActionInfo.ParamSchema->Values)
		{
			if (Pair.Key == TEXT("asset_path")) continue;

			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
			{
				bool bRequired = false;
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (bRequired && !EffectiveParams->HasField(Pair.Key))
				{
					// Legacy wbp_path / asset_path aliasing: accept asset_path as substitute for wbp_path
					// (only fires for schemas not migrated to K2 aliases).
					if (Pair.Key == TEXT("wbp_path") && EffectiveParams->HasField(TEXT("asset_path")))
						continue;
					Missing.Add(Pair.Key);
				}
			}
		}
		if (Missing.Num() > 0)
		{
			SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
			TArray<FString> Provided;
			Provided.Reserve(EffectiveParams->Values.Num());
			for (const auto& P : EffectiveParams->Values) Provided.Add(P.Key);

			// CC-05: enrich the missing-param error with alias info so the agent
			// can fix typos without round-trip schema fetches.
			TArray<FString> AliasHints;
			for (const FString& MissKey : Missing)
			{
				const TSharedPtr<FJsonObject>* MissDef = nullptr;
				if (!ActionInfo.ParamSchema->TryGetObjectField(MissKey, MissDef) || !MissDef) continue;

				const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
				if ((*MissDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr && AliasArr->Num() > 0)
				{
					TArray<FString> Aliases;
					Aliases.Reserve(AliasArr->Num());
					for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
					{
						FString A;
						if (AV.IsValid() && AV->TryGetString(A)) Aliases.Add(A);
					}
					if (Aliases.Num() > 0)
					{
						AliasHints.Add(FString::Printf(TEXT("'%s' (aliases: %s)"),
							*MissKey, *FString::Join(Aliases, TEXT(", "))));
					}
				}
			}

			// Preserve the existing error code (default -32603) so callers that
			// match on it stay compatible. Only the Hints array is additive here.
			FMonolithActionResult R = FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required param(s): [%s]. Provided keys: [%s] — inspect the action's parameter schema via monolith_discover(\"<namespace>\") and supply all required fields."),
					*FString::Join(Missing, TEXT(", ")),
					*FString::Join(Provided, TEXT(", "))));
			if (AliasHints.Num() > 0)
			{
				R.Hints.Add(FString::Printf(TEXT("Accepted aliases: %s"),
					*FString::Join(AliasHints, TEXT("; "))));
			}
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}

	// K3 — unknown-key detection (after required-check, before dispatch).
	TArray<FString> Unknown;
	if (ActionInfo.ParamSchema.IsValid())
	{
		Unknown = FMonolithParamSchema::FindUnknownKeys(ActionInfo.ParamSchema, EffectiveParams);

		if (Unknown.Num() > 0)
		{
			for (const FString& K : Unknown)
			{
				UE_LOG(LogMonolith, Warning,
					TEXT("Unknown param '%s' for action '%s:%s' (typo? not in schema)"),
					*K, *Namespace, *Action);
			}

			if (FMonolithParamSchema::IsStrictParamsEnabled())
			{
				SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
				FMonolithActionResult R = FMonolithActionResult::Error(
					FString::Printf(TEXT("STRICT_PARAMS=1: rejected action '%s:%s' due to unknown params: [%s] — unset STRICT_PARAMS or remove the unknown params from the call."),
						*Namespace, *Action, *FString::Join(Unknown, TEXT(", "))),
					FMonolithJsonUtils::ErrInvalidParams);
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					TEXT(""),
					Namespace,
					Action,
					TEXT("malformed_dispatch"),
					R.ErrorCode,
					R.ErrorMessage);
				Lock.Unlock();
				return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
			}
		}
	}

	// Typed/range/enum validation is opt-in per schema via _validate_types.
	if (ActionInfo.ParamSchema.IsValid())
	{
		TArray<FString> ValidationErrors;
		if (!FMonolithParamSchema::ValidateTypedParams(ActionInfo.ParamSchema, EffectiveParams, ValidationErrors))
		{
			SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
			FMonolithActionResult R = FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid param(s) for action '%s:%s': %s"),
					*Namespace,
					*Action,
					*FString::Join(ValidationErrors, TEXT("; "))),
				FMonolithJsonUtils::ErrInvalidParams);
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}
	if (ActionInfo.ParamSchema.IsValid())
	{
		SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
	}

	// Release lock before executing handler (handlers may take time)
	FMonolithActionHandler HandlerCopy = RegAction->Handler;
	Lock.Unlock();

	// Crash breadcrumb capture — records (namespace, action, params) into a
	// pre-built file path/payload that the fatal handler writes synchronously
	// if the editor crashes during the handler. RAII clears the slot on exit.
	FMonolithCrashBreadcrumb::FScopedCapture CrashCapture(Namespace, Action, EffectiveParams);

	const double HandlerStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	FScopedEnvironmentVar TraceEnv(TEXT("MONOLITH_TRACE_ID"), ActionTraceId);
	FScopedEnvironmentVar ParentSpanEnv(TEXT("MONOLITH_PARENT_SPAN_ID"), ActionSpanId);
	FMonolithActionResult ActionResult = HandlerCopy.Execute(EffectiveParams);
	SetPhaseMs(TEXT("handler_ms"), HandlerStartSeconds);

	// On success, append `warnings` array for unknown params (K3 soft-warn mode).
	if (ActionResult.bSuccess && Unknown.Num() > 0 && ActionResult.Result.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Existing;
		const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
		if (ActionResult.Result->TryGetArrayField(TEXT("warnings"), Found) && Found)
		{
			Existing = *Found;
		}
		for (const FString& K : Unknown)
		{
			Existing.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Unknown param '%s' for action '%s:%s'"), *K, *Namespace, *Action)));
		}
		ActionResult.Result->SetArrayField(TEXT("warnings"), Existing);
	}

	const double PostEditStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	CrashCapture.ApplyPostEditValidation(ActionResult, EffectiveParams);
	SetPhaseMs(TEXT("post_edit_ms"), PostEditStartSeconds);
	CrashCapture.SetOutcome(ActionResult.bSuccess, ActionResult.ErrorCode, ActionResult.Result, ActionResult.ErrorMessage);
	return RecordAndReturn(ActionResult, TEXT("dispatch"), EffectiveParams);
}

TArray<FString> FMonolithToolRegistry::GetNamespaces() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;
	Result.Reserve(NamespaceActions.Num());
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : NamespaceActions)
	{
		const FString& Namespace = Pair.Key;
		for (const FString& Key : Pair.Value)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(Namespace);
					break;
				}
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetActions(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		Result.Reserve(Keys->Num());
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(Profiles.ApplyDescriptionOverride(RegAction->Info));
				}
			}
		}
	}
	return Result;
}

TArray<FString> FMonolithToolRegistry::GetActionNames(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		Result.Reserve(Keys->Num());
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(RegAction->Info.Action);
				}
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetAllActions() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;
	Result.Reserve(Actions.Num());
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : Actions)
	{
		const FMonolithActionInfo& Info = Pair.Value.Info;
		if (Profiles.IsActionAllowed(Info.Namespace, Info.Action))
		{
			Result.Add(Profiles.ApplyDescriptionOverride(Info));
		}
	}
	return Result;
}

bool FMonolithToolRegistry::HasAction(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Contains(MakeKey(Namespace, Action));
}

bool FMonolithToolRegistry::HasNamespace(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	return NamespaceActions.Contains(Namespace);
}

FMonolithActionExecutionPolicy FMonolithToolRegistry::GetActionExecutionPolicy(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	if (const FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action)))
	{
		return RegAction->Info.ExecutionPolicy;
	}
	return FMonolithActionExecutionPolicy::DefaultReadOnly();
}

bool FMonolithToolRegistry::SetActionExecutionPolicy(
	const FString& Namespace,
	const FString& Action,
	const FMonolithActionExecutionPolicy& ExecutionPolicy,
	FString& OutError)
{
	FScopeLock Lock(&RegistryLock);
	if (FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action)))
	{
		RegAction->Info.ExecutionPolicy = ExecutionPolicy;
		OutError.Empty();
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown action: %s.%s"), *Namespace, *Action);
	return false;
}

int32 FMonolithToolRegistry::GetActionCount() const
{
	FScopeLock Lock(&RegistryLock);
	int32 Count = 0;
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : Actions)
	{
		const FMonolithActionInfo& Info = Pair.Value.Info;
		if (Profiles.IsActionAllowed(Info.Namespace, Info.Action))
		{
			++Count;
		}
	}
	return Count;
}

int32 FMonolithToolRegistry::GetNamespaceCount() const
{
	FScopeLock Lock(&RegistryLock);
	int32 Count = 0;
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : NamespaceActions)
	{
		const FString& Namespace = Pair.Key;
		for (const FString& Key : Pair.Value)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					++Count;
					break;
				}
			}
		}
	}
	return Count;
}

int32 FMonolithToolRegistry::GetNamespaceActionCount(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		int32 Count = 0;
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					++Count;
				}
			}
		}
		return Count;
	}
	return 0;
}

TArray<FString> FMonolithToolRegistry::FindSimilarActions(const FString& Namespace, const FString& ActionName, int32 MaxResults) const
{
	TArray<FString> Result;
	if (ActionName.IsEmpty() || MaxResults <= 0)
	{
		return Result;
	}

	// Snapshot candidate names under the lock, then score outside the lock.
	TArray<FString> Candidates;
	{
		FScopeLock Lock(&RegistryLock);
		const TArray<FString>* Keys = NamespaceActions.Find(Namespace);
		if (!Keys)
		{
			return Result;
		}
		Candidates.Reserve(Keys->Num());
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* Reg = Actions.Find(Key))
			{
				if (FMonolithToolProfileManager::Get().IsActionAllowed(Namespace, Reg->Info.Action))
				{
					Candidates.Add(Reg->Info.Action);
				}
			}
		}
	}

	// Score: prefix match (case-insensitive) wins; otherwise Levenshtein distance.
	// Distance threshold scales with name length so longer names tolerate more typos.
	struct FScoredCandidate { FString Name; int32 Score; };
	TArray<FScoredCandidate> Scored;
	Scored.Reserve(Candidates.Num());

	const int32 Threshold = FMath::Max(2, ActionName.Len() / 2);
	const FString LowerName = ActionName.ToLower();

	for (const FString& Cand : Candidates)
	{
		const FString LowerCand = Cand.ToLower();

		// Prefix or substring match — very strong signal, push to top.
		if (LowerCand.StartsWith(LowerName) || LowerName.StartsWith(LowerCand))
		{
			Scored.Add({Cand, 0});
			continue;
		}
		if (LowerCand.Contains(LowerName) || LowerName.Contains(LowerCand))
		{
			Scored.Add({Cand, 1});
			continue;
		}

		const int32 Dist = FMonolithFuzzyMatch::EditDistanceBounded(ActionName, Cand, Threshold, /*bCaseInsensitive=*/true);
		if (Dist <= Threshold)
		{
			Scored.Add({Cand, 2 + Dist});
		}
	}

	Scored.Sort([](const FScoredCandidate& L, const FScoredCandidate& R) { return L.Score < R.Score; });

	const int32 Count = FMath::Min(MaxResults, Scored.Num());
	Result.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		Result.Add(Scored[i].Name);
	}
	return Result;
}
