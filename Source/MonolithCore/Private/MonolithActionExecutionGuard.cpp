#include "MonolithActionExecutionGuard.h"

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
	Scope.bActive = true;
	return Scope;
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
	Row.StartedUtc = Scope.StartedUtc;
	Row.DurationMs = FMath::Max(0.0, (FPlatformTime::Seconds() - Scope.StartedSeconds) * 1000.0);
	Row.ChangedPackageCount = ChangedPackages.Num();
	Row.ChangedPackages = MoveTemp(ChangedPackages);
	Row.Status = TEXT("handler_returned");
	Row.RollbackStatus = TEXT("not_available_without_policy");

	AppendAuditRow(Row);
	Scope.bActive = false;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionGuard::GetStatusJson() const
{
	FScopeLock Lock(&GuardLock);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Result->SetStringField(TEXT("domain"), TEXT("execution_guard"));
	Result->SetStringField(TEXT("mode"), TEXT("central_audit_first_milestone"));
	Result->SetBoolField(TEXT("enabled"), true);
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

void FMonolithActionExecutionGuard::AppendAuditRow(const FAuditRow& Row)
{
	FScopeLock Lock(&GuardLock);
	AuditRows.Add(Row);
	if (AuditRows.Num() > AuditCapacity)
	{
		AuditRows.RemoveAt(0, AuditRows.Num() - AuditCapacity);
	}
}
