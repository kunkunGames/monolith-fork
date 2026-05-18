#include "MonolithActionExecutionGuard.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "MonolithToolProfileManager.h"
#include "MonolithToolRegistry.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FString BlueprintStatusToString(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown: return TEXT("Unknown");
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		case BS_BeingCreated: return TEXT("BeingCreated");
		default: return TEXT("Unknown");
		}
	}

	bool TryGetNonEmptyStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FString& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		FString Value;
		if (Object->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
		{
			OutValue = Value;
			return true;
		}
		return false;
	}

	FString FindPostEditValidationTargetPath(
		const TSharedPtr<FJsonObject>& ResultObject,
		const TSharedPtr<FJsonObject>& Params)
	{
		static const TCHAR* CandidateFields[] =
		{
			TEXT("asset_path"),
			TEXT("blueprint_path"),
			TEXT("widget_blueprint"),
			TEXT("wbp_path"),
			TEXT("save_path"),
			TEXT("new_path")
		};

		FString TargetPath;
		for (const TCHAR* FieldName : CandidateFields)
		{
			if (TryGetNonEmptyStringField(ResultObject, FieldName, TargetPath)
				|| TryGetNonEmptyStringField(Params, FieldName, TargetPath))
			{
				return TargetPath;
			}
		}
		return FString();
	}
}

FMonolithPostEditValidationResult FMonolithPostEditValidationResult::Passed(
	const FString& InValidatorName,
	const FString& InTargetAssetPath)
{
	FMonolithPostEditValidationResult Result;
	Result.bSuccess = true;
	Result.Status = TEXT("passed_by_validator");
	Result.ValidatorName = InValidatorName;
	Result.TargetAssetPath = InTargetAssetPath;
	return Result;
}

FMonolithPostEditValidationResult FMonolithPostEditValidationResult::Failed(
	const FString& InStatus,
	const FString& InValidatorName,
	const FString& InErrorMessage,
	const FString& InTargetAssetPath)
{
	FMonolithPostEditValidationResult Result;
	Result.bSuccess = false;
	Result.Status = InStatus.IsEmpty() ? TEXT("failed_by_validator") : InStatus;
	Result.ValidatorName = InValidatorName;
	Result.ErrorMessage = InErrorMessage;
	Result.TargetAssetPath = InTargetAssetPath;
	return Result;
}

FMonolithPostEditValidationResult FMonolithPostEditValidationResult::Skipped(
	const FString& InStatus,
	const FString& Reason)
{
	FMonolithPostEditValidationResult Result;
	Result.bSuccess = true;
	Result.Status = InStatus;
	Result.ErrorMessage = Reason;
	return Result;
}

TSharedPtr<FJsonObject> FMonolithPostEditValidationResult::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), bSuccess);
	Obj->SetStringField(TEXT("status"), Status);
	if (!ValidatorName.IsEmpty())
	{
		Obj->SetStringField(TEXT("validator"), ValidatorName);
	}
	if (!TargetAssetPath.IsEmpty())
	{
		Obj->SetStringField(TEXT("target_asset_path"), TargetAssetPath);
	}
	if (!ErrorMessage.IsEmpty())
	{
		Obj->SetStringField(TEXT("message"), ErrorMessage);
	}
	if (Details.IsValid())
	{
		Obj->SetObjectField(TEXT("details"), Details);
	}
	return Obj;
}

FMonolithActionExecutionGuard& FMonolithActionExecutionGuard::Get()
{
	static FMonolithActionExecutionGuard Instance;
	return Instance;
}

FMonolithActionExecutionGuard::FExecutionScope FMonolithActionExecutionGuard::BeginAction(
	const FString& Namespace,
	const FString& Action)
{
	FExecutionScope Scope;
	Scope.Id = FGuid::NewGuid();
	Scope.Namespace = Namespace;
	Scope.Action = Action;
	Scope.StartedUtc = FDateTime::UtcNow();
	Scope.StartedSeconds = FPlatformTime::Seconds();
	Scope.OutcomeStatus = TEXT("running");
	Scope.ResultKind = TEXT("unknown");
	Scope.ExecutionPolicy = FMonolithToolRegistry::Get().GetActionExecutionPolicy(Namespace, Action);
	Scope.bDirtyPackageTrackingActive = Scope.ExecutionPolicy.bDirtyPackageTracking;
	Scope.DirtyPackageTrackingStatus = Scope.bDirtyPackageTrackingActive
		? TEXT("tracked_by_policy")
		: TEXT("skipped_by_policy");
	Scope.TransactionStatus = Scope.ExecutionPolicy.bTransactionWrapping
		? TEXT("requested_by_policy")
		: TEXT("not_requested");
	Scope.PostEditValidationStatus = Scope.ExecutionPolicy.bPostEditValidation
		? TEXT("requested_by_policy")
		: TEXT("not_requested");
	Scope.RollbackStatus = Scope.ExecutionPolicy.bTransactionWrapping
		? TEXT("not_available_without_rollback_policy")
		: TEXT("not_available_without_policy");
	if (Scope.bDirtyPackageTrackingActive)
	{
		Scope.DirtyPackagesBefore = SnapshotDirtyPackages();
	}
	Scope.bActive = true;
	return Scope;
}

bool FMonolithActionExecutionGuard::RegisterPostEditValidator(
	const FString& Namespace,
	const FString& Action,
	const FMonolithPostEditValidator& Validator,
	FString& OutError)
{
	if (Namespace.IsEmpty() || Action.IsEmpty())
	{
		OutError = TEXT("Post-edit validator registration requires a namespace and action.");
		return false;
	}
	if (!Validator.IsBound())
	{
		OutError = FString::Printf(TEXT("Post-edit validator for %s.%s is not bound."), *Namespace, *Action);
		return false;
	}

	FScopeLock Lock(&GuardLock);
	PostEditValidators.Add(MakeActionKey(Namespace, Action), Validator);
	OutError.Empty();
	return true;
}

FMonolithPostEditValidationResult FMonolithActionExecutionGuard::RunPostEditValidation(
	FExecutionScope& Scope,
	const TSharedPtr<FJsonObject>& Params,
	const TSharedPtr<FJsonObject>& ResultObject)
{
	if (!Scope.bActive)
	{
		return FMonolithPostEditValidationResult::Skipped(
			TEXT("skipped_inactive_scope"),
			TEXT("Execution scope is not active."));
	}

	if (!Scope.ExecutionPolicy.bPostEditValidation)
	{
		Scope.PostEditValidationStatus = TEXT("not_requested");
		return FMonolithPostEditValidationResult::Skipped(
			TEXT("not_requested"),
			TEXT("Action policy did not request post-edit validation."));
	}

	FMonolithPostEditValidator Validator;
	{
		FScopeLock Lock(&GuardLock);
		if (const FMonolithPostEditValidator* Registered = PostEditValidators.Find(MakeActionKey(Scope.Namespace, Scope.Action)))
		{
			Validator = *Registered;
		}
	}

	FMonolithPostEditValidationContext Context;
	Context.Namespace = Scope.Namespace;
	Context.Action = Scope.Action;
	Context.Params = Params;
	Context.ResultObject = ResultObject;

	FMonolithPostEditValidationResult Validation = Validator.IsBound()
		? Validator.Execute(Context)
		: RunDefaultPostEditValidation(Context);

	if (Validation.Status.IsEmpty())
	{
		Validation.Status = Validation.bSuccess ? TEXT("passed_by_validator") : TEXT("failed_by_validator");
	}
	Scope.PostEditValidationStatus = Validation.Status;
	Scope.PostEditValidationMessage = Validation.ErrorMessage;
	return Validation;
}

void FMonolithActionExecutionGuard::SetActionOutcome(
	FExecutionScope& Scope,
	bool bSuccess,
	int32 ErrorCode,
	const TSharedPtr<FJsonObject>& ResultObject,
	const FString& ErrorMessage)
{
	if (!Scope.bActive)
	{
		return;
	}

	Scope.OutcomeStatus = bSuccess ? TEXT("success") : TEXT("error");
	Scope.JsonRpcErrorCode = bSuccess ? 0 : ErrorCode;
	Scope.ResultKind = bSuccess ? TEXT("json_object") : TEXT("error_text");

	if (!IsAdvancedToolCallRecordsEnabled())
	{
		Scope.ResultChars = 0;
		Scope.bResultTruncated = false;
		return;
	}

	if (bSuccess && ResultObject.IsValid())
	{
		Scope.ResultChars = FMonolithJsonUtils::Serialize(ResultObject).Len();
	}
	else
	{
		Scope.ResultChars = ErrorMessage.Len();
	}
	Scope.bResultTruncated = false;
}

void FMonolithActionExecutionGuard::EndAction(FExecutionScope& Scope)
{
	if (!Scope.bActive)
	{
		return;
	}

	TArray<FString> ChangedPackages;
	if (Scope.bDirtyPackageTrackingActive)
	{
		const TSet<FString> DirtyPackagesAfter = SnapshotDirtyPackages();
		for (const FString& PackageName : DirtyPackagesAfter)
		{
			if (!Scope.DirtyPackagesBefore.Contains(PackageName))
			{
				ChangedPackages.Add(PackageName);
			}
		}
	}
	ChangedPackages.Sort();

	FAuditRow Row;
	Row.Id = Scope.Id;
	Row.ActionName = Scope.Namespace + TEXT(".") + Scope.Action;
	Row.Namespace = Scope.Namespace;
	Row.Action = Scope.Action;
	Row.SourceToolName = Scope.Namespace == TEXT("monolith")
		? FString::Printf(TEXT("monolith_%s"), *Scope.Action)
		: FString::Printf(TEXT("%s_query"), *Scope.Namespace);
	Row.ActiveProfileId = FMonolithToolProfileManager::Get().GetActiveProfileId();
	Row.SessionIdRedacted = TEXT("stateless");
	Row.StartedUtc = Scope.StartedUtc;
	Row.DurationMs = FMath::Max(0.0, (FPlatformTime::Seconds() - Scope.StartedSeconds) * 1000.0);
	Row.ChangedPackageCount = ChangedPackages.Num();
	Row.ChangedPackages = MoveTemp(ChangedPackages);
	Row.Status = TEXT("handler_returned");
	Row.ToolCallStatus = Scope.OutcomeStatus.IsEmpty() || Scope.OutcomeStatus == TEXT("running")
		? TEXT("success")
		: Scope.OutcomeStatus;
	Row.JsonRpcErrorCode = Scope.JsonRpcErrorCode;
	Row.ResultKind = Scope.ResultKind;
	Row.ExecutionPolicy = Scope.ExecutionPolicy;
	Row.DirtyPackageTrackingStatus = Scope.DirtyPackageTrackingStatus.IsEmpty()
		? TEXT("skipped_by_policy")
		: Scope.DirtyPackageTrackingStatus;
	Row.TransactionStatus = Scope.TransactionStatus.IsEmpty()
		? TEXT("not_requested")
		: Scope.TransactionStatus;
	Row.PostEditValidationStatus = Scope.PostEditValidationStatus.IsEmpty()
		? TEXT("not_requested")
		: Scope.PostEditValidationStatus;
	Row.PostEditValidationMessage = Scope.PostEditValidationMessage;
	Row.ResultChars = Scope.ResultChars;
	Row.bResultTruncated = Scope.bResultTruncated;
	Row.RollbackStatus = Scope.RollbackStatus.IsEmpty()
		? TEXT("not_available_without_policy")
		: Scope.RollbackStatus;

	AppendAuditRow(Row);
	Scope.bActive = false;
}

void FMonolithActionExecutionGuard::RecordRejectedToolCall(
	const FString& SourceToolName,
	const FString& Namespace,
	const FString& Action,
	const FString& Status,
	int32 ErrorCode,
	const FString& Reason)
{
	if (!IsAdvancedToolCallRecordsEnabled())
	{
		return;
	}

	FAuditRow Row;
	Row.Id = FGuid::NewGuid();
	Row.Namespace = Namespace;
	Row.Action = Action;
	Row.ActionName = Namespace.IsEmpty() || Action.IsEmpty()
		? SourceToolName
		: Namespace + TEXT(".") + Action;
	Row.SourceToolName = SourceToolName;
	Row.ActiveProfileId = FMonolithToolProfileManager::Get().GetActiveProfileId();
	Row.SessionIdRedacted = TEXT("stateless");
	Row.StartedUtc = FDateTime::UtcNow();
	Row.DurationMs = 0.0;
	Row.ChangedPackageCount = 0;
	Row.Status = TEXT("rejected_before_handler");
	Row.ToolCallStatus = Status;
	Row.JsonRpcErrorCode = ErrorCode;
	Row.ResultKind = TEXT("error_text");
	Row.ExecutionPolicy = FMonolithActionExecutionPolicy::DefaultReadOnly();
	Row.DirtyPackageTrackingStatus = TEXT("skipped_by_policy");
	Row.TransactionStatus = TEXT("not_requested");
	Row.PostEditValidationStatus = TEXT("not_requested");
	Row.ResultChars = Reason.Len();
	Row.bResultTruncated = false;
	Row.RollbackStatus = TEXT("not_available_without_policy");
	Row.Reason = Reason;
	AppendAuditRow(Row);
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetStatusJson() const
{
	const bool bAdvancedRecords = IsAdvancedToolCallRecordsEnabled();
	const bool bAdvancedRegistered = bAdvancedRecords
		&& FMonolithToolRegistry::Get().HasAction(TEXT("monolith"), TEXT("list_tool_call_records"));
	int32 AuditCount = 0;
	{
		FScopeLock Lock(&GuardLock);
		AuditCount = AuditRows.Num();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("execution_guard"));
	Result->SetStringField(TEXT("mode"), TEXT("central_policy_gated_audit"));
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetBoolField(TEXT("advanced_tool_call_records"), bAdvancedRecords);
	Result->SetNumberField(TEXT("audit_capacity"), AuditCapacity);
	Result->SetNumberField(TEXT("audit_count"), AuditCount);
	Result->SetBoolField(TEXT("dirty_package_tracking"), true);
	Result->SetStringField(TEXT("dirty_package_tracking_mode"), TEXT("policy_gated"));
	Result->SetBoolField(TEXT("policy_gated_dirty_package_tracking"), true);
	Result->SetBoolField(TEXT("raw_payload_logging"), false);
	Result->SetBoolField(TEXT("automatic_transaction_wrapping"), true);
	Result->SetStringField(TEXT("transaction_wrapping_mode"), TEXT("policy_gated"));
	Result->SetBoolField(TEXT("policy_gated_transaction_wrapping"), true);
	Result->SetBoolField(TEXT("automatic_rollback"), false);
	Result->SetBoolField(TEXT("post_edit_validation"), true);
	Result->SetStringField(TEXT("post_edit_validation_mode"), TEXT("policy_gated"));
	Result->SetBoolField(TEXT("execution_policy_metadata"), true);
	{
		FScopeLock Lock(&GuardLock);
		Result->SetNumberField(TEXT("registered_post_edit_validators"), PostEditValidators.Num());
	}
	Result->SetBoolField(TEXT("builtin_blueprint_post_edit_validator"), true);

	TArray<TSharedPtr<FJsonValue>> Implemented;
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_execution_guard_status")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.list_recent_action_audit")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_last_rollback")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("registry execution_policy metadata")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("policy-gated dirty package tracking")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("policy-gated transaction wrapping")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("post-edit validator hooks")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.set_action_execution_policy")));
	// Advanced ToolCall actions are only registered during RegisterMonolithExecutionGuardActions
	// when the setting was true at startup. Gate the advertised list on actual registry state
	// so toggling the setting at runtime (without restart) doesn't make capability discovery
	// lie about endpoints that still 'unknown action' on dispatch.
	if (bAdvancedRegistered)
	{
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.list_tool_call_records")));
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_tool_call_record")));
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.analyze_tool_call_records")));
	}
	Result->SetBoolField(TEXT("advanced_records_registered"), bAdvancedRegistered);
	if (bAdvancedRecords && !bAdvancedRegistered)
	{
		Result->SetBoolField(TEXT("restart_required"), true);
	}
	Result->SetArrayField(TEXT("implemented_actions"), Implemented);

	TArray<TSharedPtr<FJsonValue>> Future;
	Future.Add(MakeShared<FJsonValueString>(TEXT("rollback reports for policy failures")));
	Result->SetArrayField(TEXT("future_work"), Future);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Audit capture is wired through the existing central crash-breadcrumb execution scope, so it applies to action dispatch without changing each domain action.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Default read_only policies skip package scans, transactions, and post-edit validation; explicit mutating policies can opt into each central guard feature.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Post-edit validation failure returns a structured action error, but automatic asset rollback remains unavailable.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetRecentAuditJson(int32 Limit) const
{
	const int32 ClampedLimit = FMath::Clamp(Limit, 1, AuditCapacity);
	FScopeLock Lock(&GuardLock);

	TArray<TSharedPtr<FJsonValue>> Rows;
	const int32 StartIndex = FMath::Max(0, AuditRows.Num() - ClampedLimit);
	for (int32 Index = AuditRows.Num() - 1; Index >= StartIndex; --Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(AuditRowToJson(AuditRows[Index])));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("execution_guard"));
	Result->SetNumberField(TEXT("audit_count"), AuditRows.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), ClampedLimit);
	Result->SetArrayField(TEXT("rows"), Rows);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetLastRollbackJson() const
{
	FScopeLock Lock(&GuardLock);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("execution_guard"));
	Result->SetBoolField(TEXT("available"), LastRollback.IsValid());
	if (LastRollback.IsValid())
	{
		Result->SetObjectField(TEXT("rollback"), LastRollback);
	}
	else
	{
		Result->SetStringField(TEXT("rollback_status"), TEXT("not_available_without_policy"));
		Result->SetStringField(TEXT("reason"), TEXT("No registry execution policy has requested rollback in this milestone."));
	}
	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetToolCallRecordsJson(
	int32 Limit,
	const FString& StatusFilter,
	const FString& ActionFilter) const
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("tool_call_records"));
	Result->SetBoolField(TEXT("enabled"), IsAdvancedToolCallRecordsEnabled());
	Result->SetBoolField(TEXT("raw_payload_logging"), false);
	Result->SetStringField(TEXT("session_id_redaction"), TEXT("stateless"));
	Result->SetNumberField(TEXT("retention_capacity"), AuditCapacity);

	if (!IsAdvancedToolCallRecordsEnabled())
	{
		Result->SetStringField(TEXT("status"), TEXT("disabled"));
		Result->SetStringField(TEXT("reason"), TEXT("bEnableAdvancedToolCallRecords is false. Enable it and restart the editor to register advanced ToolCall record actions."));
		Result->SetArrayField(TEXT("records"), TArray<TSharedPtr<FJsonValue>>{});
		Result->SetNumberField(TEXT("returned_count"), 0);
		return Result;
	}

	const int32 ClampedLimit = FMath::Clamp(Limit, 1, AuditCapacity);
	TArray<TSharedPtr<FJsonValue>> Rows;
	FScopeLock Lock(&GuardLock);
	for (int32 Index = AuditRows.Num() - 1; Index >= 0 && Rows.Num() < ClampedLimit; --Index)
	{
		const FAuditRow& Row = AuditRows[Index];
		if (RowMatchesFilters(Row, StatusFilter, ActionFilter))
		{
			Rows.Add(MakeShared<FJsonValueObject>(ToolCallRecordToJson(Row)));
		}
	}

	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetNumberField(TEXT("audit_count"), AuditRows.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), ClampedLimit);
	Result->SetStringField(TEXT("status_filter"), StatusFilter);
	Result->SetStringField(TEXT("action_filter"), ActionFilter);
	Result->SetArrayField(TEXT("records"), Rows);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetToolCallRecordJson(const FString& RecordId) const
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("tool_call_records"));
	Result->SetBoolField(TEXT("enabled"), IsAdvancedToolCallRecordsEnabled());
	Result->SetStringField(TEXT("id"), RecordId);

	if (!IsAdvancedToolCallRecordsEnabled())
	{
		Result->SetBoolField(TEXT("found"), false);
		Result->SetStringField(TEXT("status"), TEXT("disabled"));
		return Result;
	}

	FScopeLock Lock(&GuardLock);
	for (const FAuditRow& Row : AuditRows)
	{
		if (Row.Id.ToString(EGuidFormats::DigitsWithHyphens) == RecordId)
		{
			Result->SetBoolField(TEXT("found"), true);
			Result->SetObjectField(TEXT("record"), ToolCallRecordToJson(Row));
			return Result;
		}
	}

	Result->SetBoolField(TEXT("found"), false);
	Result->SetStringField(TEXT("status"), TEXT("not_found"));
	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::AnalyzeToolCallRecordsJson(int32 Limit) const
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("tool_call_records"));
	Result->SetBoolField(TEXT("enabled"), IsAdvancedToolCallRecordsEnabled());

	if (!IsAdvancedToolCallRecordsEnabled())
	{
		Result->SetStringField(TEXT("status"), TEXT("disabled"));
		Result->SetStringField(TEXT("reason"), TEXT("bEnableAdvancedToolCallRecords is false."));
		return Result;
	}

	const int32 ClampedLimit = FMath::Clamp(Limit, 1, AuditCapacity);
	int32 Examined = 0;
	int32 SlowCalls = 0;
	int32 RepeatedFailures = 0;
	int32 ProfileBlocks = 0;
	int32 MutationHeavyCalls = 0;
	TMap<FString, int32> FailureCountsByAction;
	TMap<FString, int32> ErrorCounts;

	FScopeLock Lock(&GuardLock);
	for (int32 Index = AuditRows.Num() - 1; Index >= 0 && Examined < ClampedLimit; --Index)
	{
		const FAuditRow& Row = AuditRows[Index];
		++Examined;
		if (Row.DurationMs >= 1000.0)
		{
			++SlowCalls;
		}
		if (Row.ToolCallStatus == TEXT("profile_blocked"))
		{
			++ProfileBlocks;
		}
		if (Row.ChangedPackageCount >= 5)
		{
			++MutationHeavyCalls;
		}
		if (Row.ToolCallStatus != TEXT("success"))
		{
			FailureCountsByAction.FindOrAdd(Row.ActionName)++;
			const FString ErrorKey = FString::Printf(TEXT("%s:%d"), *Row.ToolCallStatus, Row.JsonRpcErrorCode);
			ErrorCounts.FindOrAdd(ErrorKey)++;
		}
	}

	for (const auto& Pair : FailureCountsByAction)
	{
		if (Pair.Value >= 2)
		{
			++RepeatedFailures;
		}
	}

	TArray<TSharedPtr<FJsonValue>> TopErrors;
	for (const auto& Pair : ErrorCounts)
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("key"), Pair.Key);
		Error->SetNumberField(TEXT("count"), Pair.Value);
		TopErrors.Add(MakeShared<FJsonValueObject>(Error));
	}

	TSharedPtr<FJsonObject> Window = MakeShared<FJsonObject>();
	Window->SetNumberField(TEXT("limit"), ClampedLimit);
	Window->SetNumberField(TEXT("examined"), Examined);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("slow_calls"), SlowCalls);
	Summary->SetNumberField(TEXT("repeated_failures"), RepeatedFailures);
	Summary->SetNumberField(TEXT("profile_blocks"), ProfileBlocks);
	Summary->SetNumberField(TEXT("mutation_heavy_calls"), MutationHeavyCalls);

	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetObjectField(TEXT("window"), Window);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetArrayField(TEXT("top_errors"), TopErrors);
	return Result;
}

bool FMonolithActionExecutionGuard::IsAdvancedToolCallRecordsEnabled()
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	return Settings && Settings->bEnableAdvancedToolCallRecords;
}

FString FMonolithActionExecutionGuard::MakeActionKey(const FString& Namespace, const FString& Action)
{
	return Namespace + TEXT(".") + Action;
}

FMonolithPostEditValidationResult FMonolithActionExecutionGuard::RunDefaultPostEditValidation(
	const FMonolithPostEditValidationContext& Context)
{
	const bool bBlueprintLikeNamespace = Context.Namespace == TEXT("blueprint") || Context.Namespace == TEXT("ui");
	if (!bBlueprintLikeNamespace)
	{
		return FMonolithPostEditValidationResult::Failed(
			TEXT("missing_post_edit_validator"),
			TEXT("default"),
			FString::Printf(TEXT("No post-edit validator is registered for %s.%s."), *Context.Namespace, *Context.Action));
	}

	const FString TargetPathRaw = FindPostEditValidationTargetPath(Context.ResultObject, Context.Params);
	if (TargetPathRaw.IsEmpty())
	{
		return FMonolithPostEditValidationResult::Failed(
			TEXT("missing_validation_target"),
			TEXT("builtin_blueprint_compile"),
			FString::Printf(TEXT("Post-edit validation for %s.%s could not find an asset_path, blueprint_path, widget_blueprint, wbp_path, save_path, or new_path target."), *Context.Namespace, *Context.Action));
	}

	const FString TargetPath = FMonolithAssetUtils::ResolveAssetPath(TargetPathRaw);
	UBlueprint* Blueprint = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(TargetPath);
	if (!Blueprint)
	{
		return FMonolithPostEditValidationResult::Failed(
			TEXT("validation_target_not_blueprint"),
			TEXT("builtin_blueprint_compile"),
			FString::Printf(TEXT("Post-edit validation target is not a Blueprint asset: %s"), *TargetPath),
			TargetPath);
	}

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint,
		EBlueprintCompileOptions::SkipGarbageCollection,
		&Results);

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		if (Message->GetSeverity() == EMessageSeverity::Error)
		{
			++ErrorCount;
		}
		else if (Message->GetSeverity() == EMessageSeverity::Warning)
		{
			++WarningCount;
		}
	}

	const FString StatusString = BlueprintStatusToString(Blueprint->Status);
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("blueprint_status"), StatusString);
	Details->SetNumberField(TEXT("error_count"), ErrorCount);
	Details->SetNumberField(TEXT("warning_count"), WarningCount);

	if (!Blueprint->IsUpToDate() || ErrorCount > 0)
	{
		FMonolithPostEditValidationResult Failed = FMonolithPostEditValidationResult::Failed(
			TEXT("failed_by_validator"),
			TEXT("builtin_blueprint_compile"),
			FString::Printf(TEXT("Post-edit Blueprint validation failed for %s: status=%s, errors=%d, warnings=%d."),
				*TargetPath,
				*StatusString,
				ErrorCount,
				WarningCount),
			TargetPath);
		Failed.Details = Details;
		return Failed;
	}

	FMonolithPostEditValidationResult Passed = FMonolithPostEditValidationResult::Passed(
		TEXT("builtin_blueprint_compile"),
		TargetPath);
	Passed.Details = Details;
	return Passed;
}

TSet<FString> FMonolithActionExecutionGuard::SnapshotDirtyPackages()
{
	TSet<FString> DirtyPackages;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty())
		{
			continue;
		}
		const FString PackageName = Package->GetName();
		if (!PackageName.StartsWith(TEXT("/Game")))
		{
			continue;
		}
		DirtyPackages.Add(PackageName);
	}
	return DirtyPackages;
}

TArray<TSharedPtr<FJsonValue>> FMonolithActionExecutionGuard::StringsToJson(const TArray<FString>& Values, int32 Limit)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	const int32 Count = FMath::Min(Values.Num(), Limit);
	Result.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Result.Add(MakeShared<FJsonValueString>(Values[Index]));
	}
	return Result;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::AuditRowToJson(const FAuditRow& Row)
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("id"), Row.Id.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("action"), Row.ActionName);
	Obj->SetStringField(TEXT("started_utc"), Row.StartedUtc.ToIso8601());
	Obj->SetNumberField(TEXT("duration_ms"), Row.DurationMs);
	Obj->SetStringField(TEXT("status"), Row.Status);
	Obj->SetNumberField(TEXT("changed_package_count"), Row.ChangedPackageCount);
	Obj->SetArrayField(TEXT("changed_packages"), StringsToJson(Row.ChangedPackages, 25));
	Obj->SetBoolField(TEXT("changed_packages_truncated"), Row.ChangedPackages.Num() > 25);
	Obj->SetObjectField(TEXT("execution_policy"), Row.ExecutionPolicy.ToJson());
	Obj->SetStringField(TEXT("dirty_package_tracking_status"), Row.DirtyPackageTrackingStatus.IsEmpty() ? TEXT("skipped_by_policy") : Row.DirtyPackageTrackingStatus);
	Obj->SetStringField(TEXT("transaction_status"), Row.TransactionStatus.IsEmpty() ? TEXT("not_requested") : Row.TransactionStatus);
	Obj->SetStringField(TEXT("post_edit_validation_status"), Row.PostEditValidationStatus.IsEmpty() ? TEXT("not_requested") : Row.PostEditValidationStatus);
	if (!Row.PostEditValidationMessage.IsEmpty())
	{
		Obj->SetStringField(TEXT("post_edit_validation_message"), Row.PostEditValidationMessage);
	}
	Obj->SetStringField(TEXT("rollback_status"), Row.RollbackStatus);
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::ToolCallRecordToJson(const FAuditRow& Row)
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("id"), Row.Id.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("json_rpc_id"), TEXT("unknown"));
	Obj->SetStringField(TEXT("tool_call_id"), TEXT("unknown"));
	Obj->SetStringField(TEXT("session_id_redacted"), Row.SessionIdRedacted.IsEmpty() ? TEXT("stateless") : Row.SessionIdRedacted);
	Obj->SetStringField(TEXT("namespace"), Row.Namespace);
	Obj->SetStringField(TEXT("action"), Row.Action);
	Obj->SetStringField(TEXT("action_name"), Row.ActionName);
	Obj->SetStringField(TEXT("source_tool_name"), Row.SourceToolName);
	Obj->SetStringField(TEXT("active_profile_id"), Row.ActiveProfileId);
	Obj->SetStringField(TEXT("status"), Row.ToolCallStatus.IsEmpty() ? Row.Status : Row.ToolCallStatus);
	Obj->SetNumberField(TEXT("json_rpc_error_code"), Row.JsonRpcErrorCode);
	Obj->SetStringField(TEXT("started_utc"), Row.StartedUtc.ToIso8601());
	Obj->SetNumberField(TEXT("duration_ms"), Row.DurationMs);
	Obj->SetStringField(TEXT("result_kind"), Row.ResultKind);
	Obj->SetNumberField(TEXT("result_chars"), Row.ResultChars);
	Obj->SetBoolField(TEXT("result_truncated"), Row.bResultTruncated);
	Obj->SetNumberField(TEXT("changed_package_count"), Row.ChangedPackageCount);
	Obj->SetArrayField(TEXT("changed_packages"), StringsToJson(Row.ChangedPackages, 25));
	Obj->SetBoolField(TEXT("changed_packages_truncated"), Row.ChangedPackages.Num() > 25);
	Obj->SetObjectField(TEXT("execution_policy"), Row.ExecutionPolicy.ToJson());
	Obj->SetStringField(TEXT("dirty_package_tracking_status"), Row.DirtyPackageTrackingStatus.IsEmpty() ? TEXT("skipped_by_policy") : Row.DirtyPackageTrackingStatus);
	Obj->SetStringField(TEXT("transaction_status"), Row.TransactionStatus.IsEmpty() ? TEXT("not_requested") : Row.TransactionStatus);
	Obj->SetStringField(TEXT("post_edit_validation_status"), Row.PostEditValidationStatus.IsEmpty() ? TEXT("not_requested") : Row.PostEditValidationStatus);
	if (!Row.PostEditValidationMessage.IsEmpty())
	{
		Obj->SetStringField(TEXT("post_edit_validation_message"), Row.PostEditValidationMessage);
	}
	Obj->SetBoolField(TEXT("raw_payload_logging"), false);
	Obj->SetNumberField(TEXT("redaction_version"), 1);
	Obj->SetStringField(TEXT("rollback_status"), Row.RollbackStatus);
	if (!Row.Reason.IsEmpty())
	{
		Obj->SetStringField(TEXT("reason"), Row.Reason);
	}
	return Obj;
}

bool FMonolithActionExecutionGuard::RowMatchesFilters(const FAuditRow& Row, const FString& StatusFilter, const FString& ActionFilter)
{
	const FString RowStatus = Row.ToolCallStatus.IsEmpty() ? Row.Status : Row.ToolCallStatus;
	if (!StatusFilter.IsEmpty() && RowStatus != StatusFilter)
	{
		return false;
	}
	if (!ActionFilter.IsEmpty() && Row.ActionName != ActionFilter && Row.Action != ActionFilter)
	{
		return false;
	}
	return true;
}

void FMonolithActionExecutionGuard::AppendAuditRow(const FAuditRow& Row)
{
	FScopeLock Lock(&GuardLock);
	AuditRows.Add(Row);
	if (AuditRows.Num() > AuditCapacity)
	{
		AuditRows.RemoveAt(0, AuditRows.Num() - AuditCapacity);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithActionExecutionGuard::ResetForTests()
{
	FScopeLock Lock(&GuardLock);
	AuditRows.Empty();
	PostEditValidators.Empty();
	LastRollback.Reset();
}
#endif
