#include "MonolithActionExecutionGuard.h"

#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "MonolithToolProfileManager.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

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
	Scope.DirtyPackagesBefore = SnapshotDirtyPackages();
	Scope.OutcomeStatus = TEXT("running");
	Scope.ResultKind = TEXT("unknown");
	Scope.bActive = true;
	return Scope;
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

	const TSet<FString> DirtyPackagesAfter = SnapshotDirtyPackages();
	TArray<FString> ChangedPackages;
	for (const FString& PackageName : DirtyPackagesAfter)
	{
		if (!Scope.DirtyPackagesBefore.Contains(PackageName))
		{
			ChangedPackages.Add(PackageName);
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
	Row.ResultChars = Scope.ResultChars;
	Row.bResultTruncated = Scope.bResultTruncated;
	Row.RollbackStatus = TEXT("not_available_without_policy");

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
	Row.ResultChars = Reason.Len();
	Row.bResultTruncated = false;
	Row.RollbackStatus = TEXT("not_available_without_policy");
	Row.Reason = Reason;
	AppendAuditRow(Row);
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetStatusJson() const
{
	FScopeLock Lock(&GuardLock);
	const bool bAdvancedRecords = IsAdvancedToolCallRecordsEnabled();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("execution_guard"));
	Result->SetStringField(TEXT("mode"), TEXT("central_audit_first_milestone"));
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetBoolField(TEXT("advanced_tool_call_records"), bAdvancedRecords);
	Result->SetNumberField(TEXT("audit_capacity"), AuditCapacity);
	Result->SetNumberField(TEXT("audit_count"), AuditRows.Num());
	Result->SetBoolField(TEXT("dirty_package_tracking"), true);
	Result->SetBoolField(TEXT("raw_payload_logging"), false);
	Result->SetBoolField(TEXT("automatic_transaction_wrapping"), false);
	Result->SetBoolField(TEXT("automatic_rollback"), false);
	Result->SetBoolField(TEXT("post_edit_validation"), false);

	TArray<TSharedPtr<FJsonValue>> Implemented;
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_execution_guard_status")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.list_recent_action_audit")));
	Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_last_rollback")));
	if (bAdvancedRecords)
	{
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.list_tool_call_records")));
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.get_tool_call_record")));
		Implemented.Add(MakeShared<FJsonValueString>(TEXT("monolith.analyze_tool_call_records")));
	}
	Result->SetArrayField(TEXT("implemented_actions"), Implemented);

	TArray<TSharedPtr<FJsonValue>> Future;
	Future.Add(MakeShared<FJsonValueString>(TEXT("central execution policy metadata")));
	Future.Add(MakeShared<FJsonValueString>(TEXT("policy-driven transaction wrapping")));
	Future.Add(MakeShared<FJsonValueString>(TEXT("post-edit validators")));
	Future.Add(MakeShared<FJsonValueString>(TEXT("rollback reports for policy failures")));
	Result->SetArrayField(TEXT("future_work"), Future);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Audit capture is wired through the existing central crash-breadcrumb execution scope, so it applies to action dispatch without changing each domain action.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This milestone intentionally does not claim rollback support; rollback requires future registry policy metadata and transaction integration.")));
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
	LastRollback.Reset();
}
#endif
