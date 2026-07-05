#include "MonolithPlanExecutor.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolProfileManager.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 MaxPlanSteps = 25;
	constexpr int32 DefaultMaxResultBytesPerStep = 16384;
	constexpr int32 MinMaxResultBytesPerStep = 1024;
	constexpr int32 MaxMaxResultBytesPerStep = 262144;

	TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value);

	TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (!Object.IsValid())
		{
			return Clone;
		}
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Object))
		{
			Clone->SetField(FMonolithJsonUtils::FieldKeyToString(Pair.Key), CloneJsonValue(Pair.Value));
		}
		return Clone;
	}

	TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		switch (Value->Type)
		{
		case EJson::Object:
			return MakeShared<FJsonValueObject>(CloneJsonObject(Value->AsObject()));
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Cloned;
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				Cloned.Add(CloneJsonValue(Element));
			}
			return MakeShared<FJsonValueArray>(Cloned);
		}
		default:
			return Value;
		}
	}

	bool IsValidStepIdentifier(const FString& Id)
	{
		if (Id.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Char : Id)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}

	/** Whole-string step reference: "$steps.<id>.result" or "$steps.<id>.result.<field>[.<field>]*". */
	bool ParseStepReference(const FString& Value, FString& OutStepId, TArray<FString>& OutPath)
	{
		if (!Value.StartsWith(TEXT("$steps.")))
		{
			return false;
		}
		TArray<FString> Tokens;
		Value.ParseIntoArray(Tokens, TEXT("."), false);
		if (Tokens.Num() < 3 || Tokens[0] != TEXT("$steps") || Tokens[2] != TEXT("result"))
		{
			return false;
		}
		if (!IsValidStepIdentifier(Tokens[1]))
		{
			return false;
		}
		for (int32 Index = 3; Index < Tokens.Num(); ++Index)
		{
			if (Tokens[Index].IsEmpty())
			{
				return false;
			}
		}
		OutStepId = Tokens[1];
		OutPath.Reset();
		for (int32 Index = 3; Index < Tokens.Num(); ++Index)
		{
			OutPath.Add(Tokens[Index]);
		}
		return true;
	}

	void CollectStepReferences(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutStepIds)
	{
		if (!Value.IsValid())
		{
			return;
		}
		switch (Value->Type)
		{
		case EJson::String:
		{
			FString StepId;
			TArray<FString> Path;
			if (ParseStepReference(Value->AsString(), StepId, Path))
			{
				OutStepIds.AddUnique(StepId);
			}
			break;
		}
		case EJson::Object:
			for (const auto& Pair : FMonolithJsonUtils::GetFields(Value->AsObject()))
			{
				CollectStepReferences(Pair.Value, OutStepIds);
			}
			break;
		case EJson::Array:
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				CollectStepReferences(Element, OutStepIds);
			}
			break;
		default:
			break;
		}
	}

	TSharedPtr<FJsonValue> ResolveReferencePath(
		const TSharedPtr<FJsonObject>& StepResult,
		const TArray<FString>& Path,
		FString& OutError)
	{
		TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(StepResult.IsValid() ? StepResult : MakeShared<FJsonObject>());
		for (const FString& Segment : Path)
		{
			if (Current->Type == EJson::Array)
			{
				// v2: all-digit segments index into arrays ("$steps.s1.result.items.0.name").
				if (!Segment.IsNumeric())
				{
					OutError = FString::Printf(TEXT("path segment '%s' addresses an array; use a numeric index"), *Segment);
					return nullptr;
				}
				const TArray<TSharedPtr<FJsonValue>>& CurrentArray = Current->AsArray();
				const int32 ArrayIndex = FCString::Atoi(*Segment);
				if (!CurrentArray.IsValidIndex(ArrayIndex))
				{
					OutError = FString::Printf(TEXT("array index %d out of range (array has %d element(s))"),
						ArrayIndex, CurrentArray.Num());
					return nullptr;
				}
				Current = CurrentArray[ArrayIndex];
				continue;
			}
			const TSharedPtr<FJsonObject> CurrentObject = Current->AsObject();
			if (!CurrentObject.IsValid())
			{
				OutError = FString::Printf(TEXT("path segment '%s' addresses a non-object value"), *Segment);
				return nullptr;
			}
			const TSharedPtr<FJsonValue> Next = CurrentObject->TryGetField(Segment);
			if (!Next.IsValid())
			{
				TArray<FString> AvailableKeys;
				FMonolithJsonUtils::GetFieldNames(CurrentObject, AvailableKeys);
				OutError = FString::Printf(TEXT("field '%s' not found (available: %s)"),
					*Segment, *FString::Join(AvailableKeys, TEXT(", ")));
				return nullptr;
			}
			Current = Next;
		}
		return Current;
	}

	TSharedPtr<FJsonValue> ResolveReferencesInValue(
		const TSharedPtr<FJsonValue>& Value,
		const TMap<FString, TSharedPtr<FJsonObject>>& CompletedResults,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		switch (Value->Type)
		{
		case EJson::String:
		{
			FString StepId;
			TArray<FString> Path;
			const FString Raw = Value->AsString();
			if (!ParseStepReference(Raw, StepId, Path))
			{
				return Value;
			}
			const TSharedPtr<FJsonObject>* StepResult = CompletedResults.Find(StepId);
			if (!StepResult)
			{
				OutError = FString::Printf(TEXT("reference '%s': step '%s' has no completed result"), *Raw, *StepId);
				return nullptr;
			}
			FString PathError;
			TSharedPtr<FJsonValue> Resolved = ResolveReferencePath(*StepResult, Path, PathError);
			if (!Resolved.IsValid())
			{
				OutError = FString::Printf(TEXT("reference '%s': %s"), *Raw, *PathError);
				return nullptr;
			}
			return Resolved;
		}
		case EJson::Object:
		{
			TSharedPtr<FJsonObject> Resolved = MakeShared<FJsonObject>();
			for (const auto& Pair : FMonolithJsonUtils::GetFields(Value->AsObject()))
			{
				TSharedPtr<FJsonValue> ResolvedField = ResolveReferencesInValue(Pair.Value, CompletedResults, OutError);
				if (!ResolvedField.IsValid())
				{
					return nullptr;
				}
				Resolved->SetField(FMonolithJsonUtils::FieldKeyToString(Pair.Key), ResolvedField);
			}
			return MakeShared<FJsonValueObject>(Resolved);
		}
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Resolved;
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				TSharedPtr<FJsonValue> ResolvedElement = ResolveReferencesInValue(Element, CompletedResults, OutError);
				if (!ResolvedElement.IsValid())
				{
					return nullptr;
				}
				Resolved.Add(ResolvedElement);
			}
			return MakeShared<FJsonValueArray>(Resolved);
		}
		default:
			return Value;
		}
	}

	int64 SerializedUtf8Bytes(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return 0;
		}
		FString Serialized;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return FTCHARToUTF8(*Serialized).Length();
	}

	struct FPlanStep
	{
		FString Id;
		FString Namespace;
		FString Action;
		TSharedPtr<FJsonObject> Params;
		FMonolithActionInfo Info;
		bool bMutating = false;
		bool bDestructive = false;
		TArray<FString> ReferencedStepIds;
	};

	FMonolithActionResult StepError(const int32 StepIndex, const FString& StepId, const FString& Message)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Plan step %d ('%s'): %s"), StepIndex + 1, *StepId, *Message),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> MakePlanRow(const FPlanStep& Step)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("id"), Step.Id);
		Row->SetStringField(TEXT("action_id"), Step.Namespace + TEXT(".") + Step.Action);
		Row->SetStringField(TEXT("policy_id"), Step.Info.ExecutionPolicy.PolicyId);
		Row->SetBoolField(TEXT("mutating"), Step.bMutating);
		Row->SetBoolField(TEXT("destructive"), Step.bDestructive);
		if (Step.ReferencedStepIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Refs;
			for (const FString& Ref : Step.ReferencedStepIds)
			{
				Refs.Add(MakeShared<FJsonValueString>(Ref));
			}
			Row->SetArrayField(TEXT("references"), Refs);
		}
		return Row;
	}
}

void FMonolithPlanExecutor::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("monolith"), TEXT("execute_plan"),
		TEXT("Execute a validated multi-step plan of registered actions in one call. Steps run sequentially through the normal dispatch pipeline (profile, aliases, schema validation, guards, logging). Whole-string params of the form '$steps.<id>.result.<field.path>' are replaced with a prior step's result value. dry_run validates and returns the plan without executing. Plans containing mutating steps require confirm=true; destructive steps additionally require allow_destructive=true. v1 has no cross-step rollback: on mid-plan failure, prior mutations remain applied."),
		FMonolithActionHandler::CreateStatic(&FMonolithPlanExecutor::HandleExecutePlan),
		FParamSchemaBuilder()
			.Required(TEXT("steps"), TEXT("array"), TEXT("Plan steps, each {id?, namespace, action, params?}. Max 25. Default ids are s1..sN."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate every step and return the plan without executing."), TEXT("false"))
			.Optional(TEXT("stop_on_error"), TEXT("boolean"), TEXT("Stop at the first failing step and mark the rest skipped. false continues; steps referencing a failed step still fail."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when any step is mutating (execution policy other than read_only)."), TEXT("false"))
			.Optional(TEXT("allow_destructive"), TEXT("boolean"), TEXT("Required true when any step is annotated destructive."), TEXT("false"))
			.Optional(TEXT("max_result_bytes_per_step"), TEXT("integer"), TEXT("Per-step result size cap in the plan response; larger results are summarized (full results still reach the invocation log). Default 16384, range 1024..262144."), TEXT("16384"))
			.Optional(TEXT("transaction"), TEXT("string"), TEXT("auto wraps mutating plans in one outermost editor transaction and cancels it when stop_on_error halts the plan (undoable edits roll back; saves/disk/source-control effects do not). off disables the wrapper."), TEXT("auto"))
			.Enum(TEXT("transaction"), { TEXT("auto"), TEXT("off") })
			.Build());
}

FMonolithActionResult FMonolithPlanExecutor::HandleExecutePlan(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'steps'."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("steps"), StepsArray) || !StepsArray || StepsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'steps' must be a non-empty array of {namespace, action, params?} objects."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (StepsArray->Num() > MaxPlanSteps)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'steps' has %d entries; max %d per plan."), StepsArray->Num(), MaxPlanSteps),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bStopOnError = true;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	bool bAllowDestructive = false;
	Params->TryGetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	int32 MaxResultBytesPerStep = DefaultMaxResultBytesPerStep;
	{
		double MaxBytesValue = 0.0;
		if (Params->TryGetNumberField(TEXT("max_result_bytes_per_step"), MaxBytesValue) && MaxBytesValue > 0.0)
		{
			MaxResultBytesPerStep = FMath::Clamp(FMath::FloorToInt(MaxBytesValue), MinMaxResultBytesPerStep, MaxMaxResultBytesPerStep);
		}
	}
	FString TransactionMode = TEXT("auto");
	{
		FString TransactionText;
		if (Params->TryGetStringField(TEXT("transaction"), TransactionText) && !TransactionText.IsEmpty())
		{
			TransactionText.TrimStartAndEndInline();
			TransactionText.ToLowerInline();
			if (TransactionText != TEXT("auto") && TransactionText != TEXT("off"))
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("Parameter 'transaction' must be 'auto' or 'off'; got '%s'"), *TransactionText),
					FMonolithJsonUtils::ErrInvalidParams);
			}
			TransactionMode = TransactionText;
		}
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// ---- Plan-time validation (shared by dry_run and execution) ----
	TArray<FPlanStep> Steps;
	Steps.Reserve(StepsArray->Num());
	TSet<FString> KnownStepIds;
	TArray<FString> MutatingStepIds;
	TArray<FString> DestructiveStepIds;

	for (int32 Index = 0; Index < StepsArray->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> StepObject = (*StepsArray)[Index]->AsObject();
		if (!StepObject.IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Plan step %d is not an object."), Index + 1),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FPlanStep Step;
		StepObject->TryGetStringField(TEXT("id"), Step.Id);
		if (Step.Id.IsEmpty())
		{
			Step.Id = FString::Printf(TEXT("s%d"), Index + 1);
		}
		if (!IsValidStepIdentifier(Step.Id))
		{
			return StepError(Index, Step.Id, TEXT("step ids must contain only letters, digits, or underscores"));
		}
		if (KnownStepIds.Contains(Step.Id))
		{
			return StepError(Index, Step.Id, TEXT("duplicate step id"));
		}

		StepObject->TryGetStringField(TEXT("namespace"), Step.Namespace);
		StepObject->TryGetStringField(TEXT("action"), Step.Action);
		if (Step.Namespace.TrimStartAndEnd().IsEmpty() || Step.Action.TrimStartAndEnd().IsEmpty())
		{
			return StepError(Index, Step.Id, TEXT("'namespace' and 'action' are required"));
		}
		Step.Namespace.TrimStartAndEndInline();
		Step.Action.TrimStartAndEndInline();
		if (Step.Namespace == TEXT("monolith") && Step.Action == TEXT("execute_plan"))
		{
			return StepError(Index, Step.Id, TEXT("nested execute_plan steps are not allowed"));
		}
		if (!Registry.HasAction(Step.Namespace, Step.Action))
		{
			return StepError(Index, Step.Id,
				FString::Printf(TEXT("unknown action '%s.%s' — use monolith.find or monolith_discover to locate the correct action"), *Step.Namespace, *Step.Action));
		}
		if (!FMonolithToolProfileManager::Get().IsActionAllowed(Step.Namespace, Step.Action))
		{
			return StepError(Index, Step.Id,
				FString::Printf(TEXT("action '%s.%s' is disabled by the active Monolith tool profile"), *Step.Namespace, *Step.Action));
		}

		const TSharedPtr<FJsonObject>* StepParams = nullptr;
		Step.Params = StepObject->TryGetObjectField(TEXT("params"), StepParams) && StepParams
			? *StepParams
			: MakeShared<FJsonObject>();

		for (const FMonolithActionInfo& Candidate : Registry.GetActions(Step.Namespace))
		{
			if (Candidate.Action == Step.Action)
			{
				Step.Info = Candidate;
				break;
			}
		}
		Step.bMutating = Step.Info.ExecutionPolicy.PolicyId != TEXT("read_only");
		Step.bDestructive = Step.Info.bDestructiveHint;
		if (Step.bMutating)
		{
			MutatingStepIds.Add(Step.Id);
		}
		if (Step.bDestructive)
		{
			DestructiveStepIds.Add(Step.Id);
		}

		// References must point at earlier steps only.
		CollectStepReferences(MakeShared<FJsonValueObject>(Step.Params), Step.ReferencedStepIds);
		for (const FString& Ref : Step.ReferencedStepIds)
		{
			if (!KnownStepIds.Contains(Ref))
			{
				return StepError(Index, Step.Id,
					FString::Printf(TEXT("reference to step '%s' which is not an earlier step in this plan"), *Ref));
			}
		}

		// Schema validation on a clone: aliases + required presence (references count as
		// provided), then typed/range/enum checks with reference-valued top-level keys
		// removed because their concrete type is only known at execution time.
		if (Step.Info.ParamSchema.IsValid())
		{
			TSharedPtr<FJsonObject> PresenceParams = CloneJsonObject(Step.Params);
			FString Collision;
			if (!FMonolithParamSchema::ApplyAliases(Step.Info.ParamSchema, PresenceParams, Collision))
			{
				return StepError(Index, Step.Id, Collision);
			}
			TArray<FString> Missing;
			for (const auto& Pair : FMonolithJsonUtils::GetFields(Step.Info.ParamSchema))
			{
				const FString ParamKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
				const TSharedPtr<FJsonObject>* ParamDef = nullptr;
				bool bRequired = false;
				if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
				{
					(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				}
				if (bRequired && !PresenceParams->HasField(ParamKey))
				{
					Missing.Add(ParamKey);
				}
			}
			if (Missing.Num() > 0)
			{
				return StepError(Index, Step.Id,
					FString::Printf(TEXT("missing required param(s): [%s]"), *FString::Join(Missing, TEXT(", "))));
			}

			TSharedPtr<FJsonObject> TypedParams = MakeShared<FJsonObject>();
			for (const auto& Pair : FMonolithJsonUtils::GetFields(PresenceParams))
			{
				FString StepId;
				TArray<FString> Path;
				if (Pair.Value.IsValid()
					&& Pair.Value->Type == EJson::String
					&& ParseStepReference(Pair.Value->AsString(), StepId, Path))
				{
					continue;
				}
				TypedParams->SetField(FMonolithJsonUtils::FieldKeyToString(Pair.Key), Pair.Value);
			}
			TArray<FString> ValidationErrors;
			if (!FMonolithParamSchema::ValidateTypedParams(Step.Info.ParamSchema, TypedParams, ValidationErrors))
			{
				return StepError(Index, Step.Id,
					FString::Printf(TEXT("invalid param(s): %s"), *FString::Join(ValidationErrors, TEXT("; "))));
			}
		}

		KnownStepIds.Add(Step.Id);
		Steps.Add(MoveTemp(Step));
	}

	// ---- Plan report scaffolding ----
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("step_count"), Steps.Num());
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_steps"), MaxPlanSteps);
	Limits->SetNumberField(TEXT("max_result_bytes_per_step"), MaxResultBytesPerStep);
	Result->SetObjectField(TEXT("limits"), Limits);

	if (bDryRun)
	{
		TArray<TSharedPtr<FJsonValue>> PlanRows;
		for (const FPlanStep& Step : Steps)
		{
			TSharedPtr<FJsonObject> Row = MakePlanRow(Step);
			Row->SetStringField(TEXT("status"), TEXT("planned"));
			PlanRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Result->SetStringField(TEXT("status"), TEXT("ok"));
		Result->SetArrayField(TEXT("steps"), PlanRows);
		Result->SetBoolField(TEXT("requires_confirm"), MutatingStepIds.Num() > 0);
		Result->SetBoolField(TEXT("requires_allow_destructive"), DestructiveStepIds.Num() > 0);
		return FMonolithActionResult::Success(Result);
	}

	// ---- Mutation/destructive gates (execution only) ----
	if (MutatingStepIds.Num() > 0 && !bConfirm)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("plan_requires_confirm"));
		TArray<TSharedPtr<FJsonValue>> Ids;
		for (const FString& Id : MutatingStepIds)
		{
			Ids.Add(MakeShared<FJsonValueString>(Id));
		}
		ErrorData->SetArrayField(TEXT("mutating_steps"), Ids);
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Plan contains %d mutating step(s) [%s]; re-run with confirm=true, or dry_run=true to inspect the plan."),
				MutatingStepIds.Num(), *FString::Join(MutatingStepIds, TEXT(", "))),
			FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("dry_run=true returns the validated plan with per-step policy classification."));
	}
	if (DestructiveStepIds.Num() > 0 && !bAllowDestructive)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("plan_requires_allow_destructive"));
		TArray<TSharedPtr<FJsonValue>> Ids;
		for (const FString& Id : DestructiveStepIds)
		{
			Ids.Add(MakeShared<FJsonValueString>(Id));
		}
		ErrorData->SetArrayField(TEXT("destructive_steps"), Ids);
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Plan contains %d destructive step(s) [%s]; re-run with allow_destructive=true (and confirm=true)."),
				DestructiveStepIds.Num(), *FString::Join(DestructiveStepIds, TEXT(", "))),
			FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData);
	}

	// ---- Sequential execution ----
	// Children dispatched below must record the plan action as their parent span, so
	// push a scope whose parent is this action's own span (ExecuteAction left the
	// pre-plan parent in place).
	FMonolithToolInvocationLogger::FScopedTrace ChildLinkScope(
		FMonolithToolInvocationLogger::GetCurrentTraceId(),
		FMonolithToolInvocationLogger::GetCurrentSpanId(),
		FString(),
		FString(),
		nullptr);

	// v2: one outermost editor transaction around mutating plans. Child actions that
	// open their own transactions nest inside it, so cancelling here rolls back every
	// undoable edit from the executed steps. Saves, disk writes, source-control, and
	// external-process effects are NOT undoable and stay applied either way.
	const bool bWantsTransaction = TransactionMode == TEXT("auto") && MutatingStepIds.Num() > 0;
	FString TransactionState = TransactionMode == TEXT("off") ? TEXT("off") : TEXT("none");
	int32 TransactionIndex = INDEX_NONE;
	if (bWantsTransaction)
	{
		if (GEditor)
		{
			TransactionIndex = GEditor->BeginTransaction(
				NSLOCTEXT("Monolith", "ExecutePlanTransaction", "Monolith execute_plan"));
			TransactionState = TransactionIndex != INDEX_NONE ? TEXT("active") : TEXT("unavailable");
		}
		else
		{
			TransactionState = TEXT("unavailable");
		}
	}

	TMap<FString, TSharedPtr<FJsonObject>> CompletedResults;
	TArray<TSharedPtr<FJsonValue>> StepRows;
	TArray<TSharedPtr<FJsonObject>> ExecutedMutatingRows;
	TArray<TSharedPtr<FJsonObject>> FailedMutatingRows;
	int32 Succeeded = 0;
	int32 Failed = 0;
	int32 Skipped = 0;
	bool bStopped = false;

	for (const FPlanStep& Step : Steps)
	{
		TSharedPtr<FJsonObject> Row = MakePlanRow(Step);

		if (bStopped)
		{
			Row->SetStringField(TEXT("status"), TEXT("skipped"));
			Row->SetStringField(TEXT("skip_reason"), TEXT("stop_on_error"));
			++Skipped;
			StepRows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		FString ReferenceError;
		TSharedPtr<FJsonValue> ResolvedParamsValue =
			ResolveReferencesInValue(MakeShared<FJsonValueObject>(Step.Params), CompletedResults, ReferenceError);
		if (!ResolvedParamsValue.IsValid())
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetStringField(TEXT("error"), ReferenceError);
			++Failed;
			StepRows.Add(MakeShared<FJsonValueObject>(Row));
			if (bStopOnError)
			{
				bStopped = true;
			}
			continue;
		}

		const double StepStartSeconds = FPlatformTime::Seconds();
		const FMonolithActionResult StepResult =
			Registry.ExecuteAction(Step.Namespace, Step.Action, ResolvedParamsValue->AsObject());
		Row->SetNumberField(TEXT("duration_ms"), (FPlatformTime::Seconds() - StepStartSeconds) * 1000.0);
		if (Step.bMutating)
		{
			ExecutedMutatingRows.Add(Row);
		}

		if (StepResult.bSuccess)
		{
			++Succeeded;
			Row->SetStringField(TEXT("status"), TEXT("ok"));
			CompletedResults.Add(Step.Id, StepResult.Result.IsValid() ? StepResult.Result : MakeShared<FJsonObject>());
			const int64 ResultBytes = SerializedUtf8Bytes(StepResult.Result);
			if (ResultBytes > MaxResultBytesPerStep)
			{
				TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
				Summary->SetBoolField(TEXT("result_truncated"), true);
				Summary->SetNumberField(TEXT("result_bytes"), static_cast<double>(ResultBytes));
				TArray<FString> TopKeys;
				if (StepResult.Result.IsValid())
				{
					FMonolithJsonUtils::GetFieldNames(StepResult.Result, TopKeys);
					TopKeys.Sort();
				}
				TArray<TSharedPtr<FJsonValue>> TopKeyValues;
				for (const FString& Key : TopKeys)
				{
					TopKeyValues.Add(MakeShared<FJsonValueString>(Key));
				}
				Summary->SetArrayField(TEXT("result_top_keys"), TopKeyValues);
				Row->SetObjectField(TEXT("result"), Summary);
			}
			else if (StepResult.Result.IsValid())
			{
				Row->SetObjectField(TEXT("result"), StepResult.Result);
			}
		}
		else
		{
			++Failed;
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetStringField(TEXT("error"), StepResult.ErrorMessage);
			Row->SetNumberField(TEXT("error_code"), StepResult.ErrorCode);
			if (StepResult.ErrorData.IsValid())
			{
				Row->SetObjectField(TEXT("error_data"), StepResult.ErrorData);
			}
			if (Step.bMutating)
			{
				FailedMutatingRows.Add(Row);
			}
			if (bStopOnError)
			{
				bStopped = true;
			}
		}
		StepRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	// Resolve the outermost transaction and stamp honest per-step rollback markers.
	if (TransactionIndex != INDEX_NONE)
	{
		if (bStopped && Failed > 0)
		{
			GEditor->CancelTransaction(TransactionIndex);
			TransactionState = TEXT("cancelled");
			for (const TSharedPtr<FJsonObject>& Row : ExecutedMutatingRows)
			{
				Row->SetStringField(TEXT("rolled_back"), TEXT("editor_transaction"));
			}
		}
		else
		{
			// stop_on_error=false with failures means the caller opted into partial
			// state, so the surviving successful mutations are committed.
			GEditor->EndTransaction();
			TransactionState = TEXT("committed");
		}
	}
	if (TransactionState != TEXT("cancelled"))
	{
		for (const TSharedPtr<FJsonObject>& Row : FailedMutatingRows)
		{
			Row->SetBoolField(TEXT("rollback_available"), false);
		}
	}

	Result->SetStringField(TEXT("status"),
		Failed == 0 ? TEXT("ok") : (Succeeded > 0 ? TEXT("partial") : TEXT("error")));
	Result->SetNumberField(TEXT("succeeded"), Succeeded);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetNumberField(TEXT("skipped"), Skipped);
	Result->SetArrayField(TEXT("steps"), StepRows);
	TSharedPtr<FJsonObject> TransactionObj = MakeShared<FJsonObject>();
	TransactionObj->SetStringField(TEXT("mode"), TransactionMode);
	TransactionObj->SetStringField(TEXT("state"), TransactionState);
	if (TransactionState == TEXT("cancelled") || TransactionState == TEXT("committed"))
	{
		TransactionObj->SetStringField(TEXT("caveat"),
			TEXT("Editor transactions cover undoable object edits only; saves, disk writes, source-control, and external-process effects are not rolled back."));
	}
	Result->SetObjectField(TEXT("transaction"), TransactionObj);
	if (Failed > 0 && MutatingStepIds.Num() > 0 && TransactionState != TEXT("cancelled"))
	{
		Result->SetStringField(TEXT("partial_state_note"),
			TEXT("Mutations from earlier successful steps remain applied (no active plan transaction rolled them back)."));
	}
	return FMonolithActionResult::Success(Result);
}
