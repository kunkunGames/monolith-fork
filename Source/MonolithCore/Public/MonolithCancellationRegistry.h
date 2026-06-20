#pragma once

#include "CoreMinimal.h"

// Thread-safe registry of in-flight MCP requests that can be cancelled across
// threads (UnrealMCP gap spec M4). The MCP transport delivers
// `notifications/cancelled` as a SEPARATE request, potentially on a different
// thread from the running `tools/call`; the running action cannot be reached
// through the thread-local FMonolithExecutionContext::GetCurrent(), so
// cancellation is routed through this request-id-keyed registry instead.
//
// A long-running, opt-in action cooperatively polls
// FMonolithCancellationRegistry::Get().IsCancellationRequested(GetCurrent()->GetJsonRpcId())
// and aborts. Synchronous actions that never poll are unaffected; this is the
// cancellation transport, not a preemption mechanism.
//
// Ordering: a notifications/cancelled that arrives before the target tools/call
// has registered, or after it has finished, is a no-op (RequestCancellation
// returns false) — there is no pre-arming of not-yet-seen request ids.
class MONOLITHCORE_API FMonolithCancellationRegistry
{
public:
	static FMonolithCancellationRegistry& Get();

	// Mark a request id as in-flight (idempotent). Called at dispatch start.
	void Register(const FString& RequestId);

	// Remove a request id (idempotent). Called when dispatch finishes.
	void Unregister(const FString& RequestId);

	// Mark an in-flight request as cancelled. Returns true if the request was
	// in-flight (found), false otherwise (already finished / never started).
	bool RequestCancellation(const FString& RequestId, const FString& Reason);

	// True if the request id is in-flight AND has been cancelled.
	bool IsCancellationRequested(const FString& RequestId) const;

	// Number of in-flight requests (for status / diagnostics).
	int32 GetActiveCount() const;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	FMonolithCancellationRegistry() = default;

	struct FEntry
	{
		bool bCancelled = false;
		FString Reason;
	};

	mutable FCriticalSection Lock;
	TMap<FString, FEntry> ActiveRequests;
};

// RAII registration of a request id for the duration of dispatch. Ignores empty
// and the non-addressable sentinel ids ("notification" / "unknown").
class MONOLITHCORE_API FScopedMonolithCancellationRegistration
{
public:
	explicit FScopedMonolithCancellationRegistration(const FString& InRequestId);
	~FScopedMonolithCancellationRegistration();

	FScopedMonolithCancellationRegistration(const FScopedMonolithCancellationRegistration&) = delete;
	FScopedMonolithCancellationRegistration& operator=(const FScopedMonolithCancellationRegistration&) = delete;

	bool IsCancellationRequested() const;

private:
	FString RequestId;
	bool bActive = false;
};
