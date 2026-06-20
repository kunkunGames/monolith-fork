#include "MonolithProgressRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FMonolithProgressRegistry& FMonolithProgressRegistry::Get()
{
	static FMonolithProgressRegistry Instance;
	return Instance;
}

void FMonolithProgressRegistry::Register(const FString& ProgressToken)
{
	if (ProgressToken.IsEmpty())
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	FProgressState& State = ActiveProgress.FindOrAdd(ProgressToken);
	++State.RefCount;
}

void FMonolithProgressRegistry::Unregister(const FString& ProgressToken)
{
	if (ProgressToken.IsEmpty())
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	if (FProgressState* State = ActiveProgress.Find(ProgressToken))
	{
		if (--State->RefCount <= 0)
		{
			ActiveProgress.Remove(ProgressToken);
		}
	}
}

void FMonolithProgressRegistry::Report(const FString& ProgressToken, double Progress, double Total, const FString& Message)
{
	if (ProgressToken.IsEmpty())
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	FProgressState* State = ActiveProgress.Find(ProgressToken);
	if (!State)
	{
		// Only report progress for in-flight, registered tokens — avoids unbounded
		// growth from stale or never-started requests.
		return;
	}
	State->Progress = Progress;
	State->Total = Total;
	State->Message = Message;
	++State->UpdateCount;
}

bool FMonolithProgressRegistry::IsActive(const FString& ProgressToken) const
{
	if (ProgressToken.IsEmpty())
	{
		return false;
	}
	FScopeLock ScopeLock(&Lock);
	return ActiveProgress.Contains(ProgressToken);
}

int32 FMonolithProgressRegistry::GetActiveCount() const
{
	FScopeLock ScopeLock(&Lock);
	return ActiveProgress.Num();
}

TSharedPtr<FJsonObject> FMonolithProgressRegistry::GetActiveJson() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Items;
	{
		FScopeLock ScopeLock(&Lock);
		Items.Reserve(ActiveProgress.Num());
		for (const TPair<FString, FProgressState>& Pair : ActiveProgress)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("progress_token"), Pair.Key);
			Obj->SetNumberField(TEXT("progress"), Pair.Value.Progress);
			if (Pair.Value.Total >= 0.0)
			{
				Obj->SetNumberField(TEXT("total"), Pair.Value.Total);
			}
			if (!Pair.Value.Message.IsEmpty())
			{
				Obj->SetStringField(TEXT("message"), Pair.Value.Message);
			}
			Obj->SetNumberField(TEXT("update_count"), Pair.Value.UpdateCount);
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	Root->SetArrayField(TEXT("active_progress"), Items);
	Root->SetNumberField(TEXT("count"), Items.Num());
	return Root;
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithProgressRegistry::ResetForTests()
{
	FScopeLock ScopeLock(&Lock);
	ActiveProgress.Empty();
}
#endif

FScopedMonolithProgressRegistration::FScopedMonolithProgressRegistration(const FString& InProgressToken)
	: ProgressToken(InProgressToken)
{
	if (!ProgressToken.IsEmpty())
	{
		FMonolithProgressRegistry::Get().Register(ProgressToken);
		bActive = true;
	}
}

FScopedMonolithProgressRegistration::~FScopedMonolithProgressRegistration()
{
	if (bActive)
	{
		FMonolithProgressRegistry::Get().Unregister(ProgressToken);
	}
}
