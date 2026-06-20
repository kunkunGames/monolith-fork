#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Thread-safe registry of in-flight MCP progress, keyed by the client-supplied
// `_meta.progressToken` (UnrealMCP gap spec M4). A long-running, opt-in action
// reports progress via Report(token, ...); the latest state per active token is
// exposed read-only (the monolith://progress/active resource and status).
//
// NOTE: real-time server->client `notifications/progress` STREAMING is not
// delivered by the embedded UE HTTP server, which does not support long-lived
// SSE connections (HandleGetMcp returns a single event and closes). Progress is
// therefore POLL-delivered today via resources/read of monolith://progress/active.
// A push transport (held-open SSE / the stdio proxy) is a follow-up; see
// Docs/specs/SPEC_MonolithCore.md and Docs/TODO.md.
class MONOLITHCORE_API FMonolithProgressRegistry
{
public:
	static FMonolithProgressRegistry& Get();

	// Mark a progress token as in-flight (idempotent). Called at dispatch start.
	void Register(const FString& ProgressToken);

	// Remove a progress token (idempotent). Called when dispatch finishes.
	void Unregister(const FString& ProgressToken);

	// Upsert the latest progress for an in-flight token. Ignored when the token
	// is empty or not registered. Total < 0 means "unknown total".
	void Report(const FString& ProgressToken, double Progress, double Total, const FString& Message);

	bool IsActive(const FString& ProgressToken) const;
	int32 GetActiveCount() const;
	TSharedPtr<FJsonObject> GetActiveJson() const;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	FMonolithProgressRegistry() = default;

	struct FProgressState
	{
		double Progress = 0.0;
		double Total = -1.0;
		FString Message;
		int32 UpdateCount = 0;
		// In-flight registrations sharing this token. The progressToken is the MCP
		// correlation key, so concurrent requests that (against spec) reuse a token
		// share one state; refcounting keeps it alive until the last registrant
		// exits, so a finishing request never drops a still-running survivor.
		int32 RefCount = 0;
	};

	mutable FCriticalSection Lock;
	TMap<FString, FProgressState> ActiveProgress;
};

// RAII registration of a progress token for the duration of dispatch. Empty
// tokens (the common case — most requests carry no progressToken) are inert.
class MONOLITHCORE_API FScopedMonolithProgressRegistration
{
public:
	explicit FScopedMonolithProgressRegistration(const FString& InProgressToken);
	~FScopedMonolithProgressRegistration();

	FScopedMonolithProgressRegistration(const FScopedMonolithProgressRegistration&) = delete;
	FScopedMonolithProgressRegistration& operator=(const FScopedMonolithProgressRegistration&) = delete;

private:
	FString ProgressToken;
	bool bActive = false;
};
