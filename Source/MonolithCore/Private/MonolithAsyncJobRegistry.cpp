#include "MonolithAsyncJobRegistry.h"

#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

FMonolithAsyncJobRegistry& FMonolithAsyncJobRegistry::Get()
{
	static FMonolithAsyncJobRegistry Instance;
	return Instance;
}

FString FMonolithAsyncJobRegistry::SubmitJob(const FString& Namespace, const FString& Action)
{
	const FString JobId = MakeJobId();
	const FDateTime Now = FDateTime::UtcNow();

	FScopeLock Lock(&RegistryLock);
	EvictOldestIfNeeded(JobId);

	FJobRow NewRow;
	NewRow.JobId = JobId;
	NewRow.Namespace = Namespace;
	NewRow.Action = Action;
	NewRow.Status = EMonolithAsyncJobStatus::Pending;
	NewRow.CreatedUtc = Now;
	NewRow.UpdatedUtc = Now;
	RowsById.Add(JobId, MoveTemp(NewRow));

	return JobId;
}

void FMonolithAsyncJobRegistry::UpdateProgress(const FString& JobId, double Percent, const FString& Stage, const FString& Message)
{
	FScopeLock Lock(&RegistryLock);
	FJobRow* Row = RowsById.Find(JobId);
	if (!Row)
	{
		return;
	}

	// Progress only advances a job out of Pending/Running; terminal states are left untouched.
	if (Row->Status == EMonolithAsyncJobStatus::Pending || Row->Status == EMonolithAsyncJobStatus::Running)
	{
		Row->Status = EMonolithAsyncJobStatus::Running;
	}

	Row->ProgressPercent = FMath::Clamp(Percent, 0.0, 100.0);
	Row->ProgressStage = Stage;
	Row->ProgressMessage = Message;
	Row->UpdatedUtc = FDateTime::UtcNow();
}

void FMonolithAsyncJobRegistry::CompleteJob(const FString& JobId, const TSharedPtr<FJsonObject>& Result)
{
	FScopeLock Lock(&RegistryLock);
	FJobRow* Row = RowsById.Find(JobId);
	if (!Row)
	{
		return;
	}

	Row->Status = EMonolithAsyncJobStatus::Completed;
	Row->ProgressPercent = 100.0;
	Row->Result = Result;
	Row->UpdatedUtc = FDateTime::UtcNow();
}

void FMonolithAsyncJobRegistry::FailJob(const FString& JobId, const FString& Error)
{
	FScopeLock Lock(&RegistryLock);
	FJobRow* Row = RowsById.Find(JobId);
	if (!Row)
	{
		return;
	}

	Row->Status = EMonolithAsyncJobStatus::Failed;
	Row->Error = Error;
	Row->UpdatedUtc = FDateTime::UtcNow();
}

void FMonolithAsyncJobRegistry::RequestCancel(const FString& JobId)
{
	FScopeLock Lock(&RegistryLock);
	FJobRow* Row = RowsById.Find(JobId);
	if (!Row)
	{
		return;
	}

	Row->bCancelRequested = true;

	// Cooperative-cancellation: only a job that has not already reached a terminal
	// state is flipped to Cancelled. A running action is expected to observe the
	// flag via IsCancelRequested and stop; the registry never interrupts work.
	if (Row->Status == EMonolithAsyncJobStatus::Pending || Row->Status == EMonolithAsyncJobStatus::Running)
	{
		Row->Status = EMonolithAsyncJobStatus::Cancelled;
	}

	Row->UpdatedUtc = FDateTime::UtcNow();
}

bool FMonolithAsyncJobRegistry::IsCancelRequested(const FString& JobId) const
{
	FScopeLock Lock(&RegistryLock);
	const FJobRow* Row = RowsById.Find(JobId);
	return Row ? Row->bCancelRequested : false;
}

TSharedPtr<FJsonObject> FMonolithAsyncJobRegistry::GetJobJson(const FString& JobId) const
{
	FScopeLock Lock(&RegistryLock);
	const FJobRow* Row = RowsById.Find(JobId);
	if (!Row)
	{
		TSharedPtr<FJsonObject> NotFound = MakeShared<FJsonObject>();
		NotFound->SetStringField(TEXT("status"), TEXT("not_found"));
		return NotFound;
	}

	return RowToJson(*Row);
}

TSharedPtr<FJsonObject> FMonolithAsyncJobRegistry::ListJobsJson(int32 Limit) const
{
	const int32 ClampedLimit = FMath::Clamp(Limit, 1, 1000);

	TArray<FJobRow> Rows;
	{
		FScopeLock Lock(&RegistryLock);
		RowsById.GenerateValueArray(Rows);
	}

	Rows.Sort([](const FJobRow& A, const FJobRow& B)
	{
		return A.UpdatedUtc > B.UpdatedUtc;
	});

	TArray<TSharedPtr<FJsonValue>> JobValues;
	const int32 Returned = FMath::Min(ClampedLimit, Rows.Num());
	JobValues.Reserve(Returned);
	for (int32 Index = 0; Index < Returned; ++Index)
	{
		JobValues.Add(MakeShared<FJsonValueObject>(RowToJson(Rows[Index])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("active"));
	Result->SetNumberField(TEXT("job_capacity"), JobCapacity);
	Result->SetNumberField(TEXT("job_count"), Rows.Num());
	Result->SetNumberField(TEXT("returned_count"), Returned);
	Result->SetArrayField(TEXT("jobs"), JobValues);
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithAsyncJobRegistry::ResetForTests()
{
	FScopeLock Lock(&RegistryLock);
	RowsById.Reset();
}
#endif

FString FMonolithAsyncJobRegistry::MakeJobId()
{
	// Lower-case digits-with-hyphens form (FGuid::EGuidFormats::DigitsWithHyphensLower).
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString FMonolithAsyncJobRegistry::StatusToken(EMonolithAsyncJobStatus Status)
{
	switch (Status)
	{
	case EMonolithAsyncJobStatus::Pending:   return TEXT("pending");
	case EMonolithAsyncJobStatus::Running:   return TEXT("running");
	case EMonolithAsyncJobStatus::Completed: return TEXT("completed");
	case EMonolithAsyncJobStatus::Failed:    return TEXT("failed");
	case EMonolithAsyncJobStatus::Cancelled: return TEXT("cancelled");
	default:                                 return TEXT("pending");
	}
}

TSharedPtr<FJsonObject> FMonolithAsyncJobRegistry::RowToJson(const FJobRow& Row)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("job_id"), Row.JobId);
	Obj->SetStringField(TEXT("namespace"), Row.Namespace);
	Obj->SetStringField(TEXT("action"), Row.Action);
	Obj->SetStringField(TEXT("status"), StatusToken(Row.Status));

	TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
	Progress->SetNumberField(TEXT("percent"), Row.ProgressPercent);
	Progress->SetStringField(TEXT("stage"), Row.ProgressStage);
	Progress->SetStringField(TEXT("message"), Row.ProgressMessage);
	Obj->SetObjectField(TEXT("progress"), Progress);

	if (Row.Result.IsValid())
	{
		Obj->SetObjectField(TEXT("result"), Row.Result);
	}

	if (!Row.Error.IsEmpty())
	{
		Obj->SetStringField(TEXT("error"), Row.Error);
	}

	Obj->SetStringField(TEXT("created_utc"), Row.CreatedUtc.ToIso8601());
	Obj->SetStringField(TEXT("updated_utc"), Row.UpdatedUtc.ToIso8601());
	return Obj;
}

void FMonolithAsyncJobRegistry::EvictOldestIfNeeded(const FString& IncomingJobId)
{
	if (RowsById.Contains(IncomingJobId) || RowsById.Num() < JobCapacity)
	{
		return;
	}

	FString OldestId;
	FDateTime OldestUpdated = FDateTime::MaxValue();
	for (const TPair<FString, FJobRow>& Pair : RowsById)
	{
		if (Pair.Value.UpdatedUtc < OldestUpdated)
		{
			OldestUpdated = Pair.Value.UpdatedUtc;
			OldestId = Pair.Key;
		}
	}

	if (!OldestId.IsEmpty())
	{
		RowsById.Remove(OldestId);
	}
}
