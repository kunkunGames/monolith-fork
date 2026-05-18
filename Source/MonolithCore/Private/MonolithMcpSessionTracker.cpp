#include "MonolithMcpSessionTracker.h"

#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"

FMonolithMcpSessionTracker& FMonolithMcpSessionTracker::Get()
{
	static FMonolithMcpSessionTracker Instance;
	return Instance;
}

void FMonolithMcpSessionTracker::ObserveRequest(
	const FString& SessionId,
	const FString& ProtocolVersion,
	const FString& Method,
	const FString& ToolName)
{
	const FString SessionKey = MakeSessionKey(SessionId);
	const FDateTime Now = FDateTime::UtcNow();

	FScopeLock Lock(&TrackerLock);
	EvictOldestIfNeeded(SessionKey);

	FSessionRow* Row = RowsByKey.Find(SessionKey);
	if (!Row)
	{
		FSessionRow NewRow;
		NewRow.SessionKey = SessionKey;
		NewRow.SessionIdRedacted = RedactSessionId(SessionId);
		NewRow.FirstSeenUtc = Now;
		NewRow.LastSeenUtc = Now;
		Row = &RowsByKey.Add(SessionKey, MoveTemp(NewRow));
	}

	Row->LastSeenUtc = Now;
	Row->RequestCount += 1;
	Row->ProtocolVersion = BoundedString(
		ProtocolVersion.IsEmpty() ? Row->ProtocolVersion : ProtocolVersion,
		64);
	Row->LastMethod = BoundedString(Method, 128);
	Row->LastToolName = BoundedString(ToolName, 128);
}

TSharedPtr<FJsonObject> FMonolithMcpSessionTracker::ListSessionsJson(int32 Limit) const
{
	const int32 ClampedLimit = FMath::Clamp(Limit, 1, 1000);

	TArray<FSessionRow> Rows;
	{
		FScopeLock Lock(&TrackerLock);
		RowsByKey.GenerateValueArray(Rows);
	}

	Rows.Sort([](const FSessionRow& A, const FSessionRow& B)
	{
		return A.LastSeenUtc > B.LastSeenUtc;
	});

	TArray<TSharedPtr<FJsonValue>> SessionValues;
	const int32 Returned = FMath::Min(ClampedLimit, Rows.Num());
	SessionValues.Reserve(Returned);
	for (int32 Index = 0; Index < Returned; ++Index)
	{
		SessionValues.Add(MakeShared<FJsonValueObject>(RowToJson(Rows[Index])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("active"));
	Result->SetStringField(TEXT("mode"), TEXT("in_memory_observer"));
	Result->SetBoolField(TEXT("raw_session_ids_stored"), false);
	Result->SetBoolField(TEXT("persistent"), false);
	Result->SetBoolField(TEXT("progress_notifications"), false);
	Result->SetBoolField(TEXT("request_cancellation"), false);
	Result->SetNumberField(TEXT("session_capacity"), SessionCapacity);
	Result->SetNumberField(TEXT("session_count"), Rows.Num());
	Result->SetNumberField(TEXT("returned_count"), Returned);
	Result->SetArrayField(TEXT("sessions"), SessionValues);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithMcpSessionTracker::RemoveSessionJson(const FString& SessionId)
{
	const FString SessionKey = MakeSessionKey(SessionId);

	bool bRemoved = false;
	{
		FScopeLock Lock(&TrackerLock);
		bRemoved = RowsByKey.Remove(SessionKey) > 0;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), bRemoved ? TEXT("observed_row_removed") : TEXT("not_found"));
	Result->SetBoolField(TEXT("terminated"), bRemoved);
	Result->SetBoolField(TEXT("cancelled_in_flight_requests"), false);
	Result->SetStringField(TEXT("session_key"), SessionKey);
	Result->SetStringField(TEXT("reason"), bRemoved
		? TEXT("First slice removes only the local observed session row; it does not interrupt running Unreal actions.")
		: TEXT("No observed MCP session row matched the supplied session id."));
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithMcpSessionTracker::ResetForTests()
{
	FScopeLock Lock(&TrackerLock);
	RowsByKey.Reset();
}
#endif

FString FMonolithMcpSessionTracker::BoundedString(FString Value, int32 MaxLen)
{
	Value.TrimStartAndEndInline();
	if (Value.Len() > MaxLen)
	{
		Value.LeftInline(MaxLen);
	}
	return Value;
}

FString FMonolithMcpSessionTracker::MakeSessionKey(const FString& SessionId)
{
	const FString Trimmed = BoundedString(SessionId, 512);
	if (Trimmed.IsEmpty())
	{
		return TEXT("stateless");
	}
	return TEXT("md5:") + FMD5::HashAnsiString(*Trimmed);
}

FString FMonolithMcpSessionTracker::RedactSessionId(const FString& SessionId)
{
	const FString Trimmed = BoundedString(SessionId, 512);
	if (Trimmed.IsEmpty())
	{
		return TEXT("stateless");
	}

	if (Trimmed.Len() <= 8)
	{
		return FString::Printf(TEXT("len:%d:%s"), Trimmed.Len(), *FMD5::HashAnsiString(*Trimmed).Left(8));
	}

	return Trimmed.Left(4) + TEXT("...") + Trimmed.Right(4);
}

TSharedPtr<FJsonObject> FMonolithMcpSessionTracker::RowToJson(const FSessionRow& Row)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("session_key"), Row.SessionKey);
	Obj->SetStringField(TEXT("session_id_redacted"), Row.SessionIdRedacted);
	Obj->SetStringField(TEXT("protocol_version"), Row.ProtocolVersion);
	Obj->SetNumberField(TEXT("request_count"), Row.RequestCount);
	Obj->SetStringField(TEXT("first_seen_utc"), Row.FirstSeenUtc.ToIso8601());
	Obj->SetStringField(TEXT("last_seen_utc"), Row.LastSeenUtc.ToIso8601());
	Obj->SetStringField(TEXT("last_method"), Row.LastMethod);
	Obj->SetStringField(TEXT("last_tool_name"), Row.LastToolName);
	return Obj;
}

void FMonolithMcpSessionTracker::EvictOldestIfNeeded(const FString& IncomingKey)
{
	if (RowsByKey.Contains(IncomingKey) || RowsByKey.Num() < SessionCapacity)
	{
		return;
	}

	FString OldestKey;
	FDateTime OldestSeen = FDateTime::MaxValue();
	for (const TPair<FString, FSessionRow>& Pair : RowsByKey)
	{
		if (Pair.Value.LastSeenUtc < OldestSeen)
		{
			OldestSeen = Pair.Value.LastSeenUtc;
			OldestKey = Pair.Key;
		}
	}

	if (!OldestKey.IsEmpty())
	{
		RowsByKey.Remove(OldestKey);
	}
}
