#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Bounded in-memory observer for MCP Streamable HTTP session headers.
 *
 * Stores redacted/hash identifiers only. Raw session ids, params, results,
 * auth headers, cookies, and bearer tokens are deliberately never retained.
 */
class MONOLITHCORE_API FMonolithMcpSessionTracker
{
public:
	static FMonolithMcpSessionTracker& Get();

	void ObserveRequest(
		const FString& SessionId,
		const FString& ProtocolVersion,
		const FString& Method,
		const FString& ToolName);

	TSharedPtr<FJsonObject> ListSessionsJson(int32 Limit) const;
	TSharedPtr<FJsonObject> RemoveSessionJson(const FString& SessionId);

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	struct FSessionRow
	{
		FString SessionKey;
		FString SessionIdRedacted;
		FString ProtocolVersion;
		FString LastMethod;
		FString LastToolName;
		FDateTime FirstSeenUtc;
		FDateTime LastSeenUtc;
		int32 RequestCount = 0;
	};

	static constexpr int32 SessionCapacity = 128;

	static FString BoundedString(FString Value, int32 MaxLen);
	static FString MakeSessionKey(const FString& SessionId);
	static FString RedactSessionId(const FString& SessionId);
	static TSharedPtr<FJsonObject> RowToJson(const FSessionRow& Row);

	void EvictOldestIfNeeded(const FString& IncomingKey);

	mutable FCriticalSection TrackerLock;
	TMap<FString, FSessionRow> RowsByKey;
};
