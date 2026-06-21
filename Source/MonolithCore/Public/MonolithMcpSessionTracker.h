#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MCP session lifecycle status, advanced additively by the P1c session-mode
 * spec-correctness slice. `Observed` is the legacy default for a row created by
 * a plain request with no initialize handshake; `Initializing` is set when an
 * `initialize` request is seen; `Initialized` is set when the matching
 * `notifications/initialized` is seen. The enum only changes observation
 * metadata — request execution stays stateless.
 */
enum class EMonolithMcpSessionStatus : uint8
{
	Observed,
	Initializing,
	Initialized,
};

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

	// P1c session-mode spec-correctness additions. These promote the observer
	// toward the MCP lifecycle contract without changing stateless execution.

	/**
	 * Records an `initialize` request: seeds/updates the row, marks it
	 * `Initializing`, and stores redacted client-capability booleans. Only the
	 * boolean presence of each capability group is retained — never the raw
	 * capability object, client name, or version string.
	 */
	void MarkInitialize(
		const FString& SessionId,
		const FString& ProtocolVersion,
		bool bClientSupportsRoots,
		bool bClientSupportsSampling,
		bool bClientSupportsElicitation);

	/** Records a `notifications/initialized` for the session: marks `Initialized`. */
	void MarkInitialized(const FString& SessionId);

	/**
	 * Returns true when an observed row exists for the supplied raw session id.
	 * Used by the server-side session gate to distinguish an unknown session
	 * (404) from a known one. The raw id is hashed before lookup and never stored.
	 */
	bool IsKnownSession(const FString& SessionId) const;

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

		// P1c additions (additive; default to the legacy observed shape).
		EMonolithMcpSessionStatus Status = EMonolithMcpSessionStatus::Observed;
		bool bClientSupportsRoots = false;
		bool bClientSupportsSampling = false;
		bool bClientSupportsElicitation = false;
	};

	static const TCHAR* StatusToken(EMonolithMcpSessionStatus Status);

	static constexpr int32 SessionCapacity = 128;

	static FString BoundedString(FString Value, int32 MaxLen);
	static FString MakeSessionKey(const FString& SessionId);
	static FString RedactSessionId(const FString& SessionId);
	static TSharedPtr<FJsonObject> RowToJson(const FSessionRow& Row);

	void EvictOldestIfNeeded(const FString& IncomingKey);

	mutable FCriticalSection TrackerLock;
	TMap<FString, FSessionRow> RowsByKey;
};
