#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/ThreadSafeBool.h"

// Central per-request execution context for the MCP server (UnrealMCP gap spec M4
// foundation, PRD/UnrealMCP/Spec/04). Carries redacted request/session metadata, a
// progress token, and a thread-safe cancellation flag. This slice only adds the type
// and its scoped thread-local accessor; legacy handlers keep their existing signature
// and behaviour, and dispatch/cancellation wiring lands in follow-up slices.
class MONOLITHCORE_API FMonolithExecutionContext
{
public:
	struct FParams
	{
		FString JsonRpcId;
		FString ToolCallId;
		FString SessionIdRedacted;
		FString ProtocolVersion;
		FString ClientName;
		FString SourceToolName;
		FString Namespace;
		FString Action;
		FString ProgressToken;
		bool bCancellable = false;
	};

	explicit FMonolithExecutionContext(const FParams& InParams);

	const FString& GetJsonRpcId() const { return Params.JsonRpcId; }
	const FString& GetToolCallId() const { return Params.ToolCallId; }
	const FString& GetSessionIdRedacted() const { return Params.SessionIdRedacted; }
	const FString& GetProtocolVersion() const { return Params.ProtocolVersion; }
	const FString& GetClientName() const { return Params.ClientName; }
	const FString& GetSourceToolName() const { return Params.SourceToolName; }
	const FString& GetNamespace() const { return Params.Namespace; }
	const FString& GetAction() const { return Params.Action; }
	const FString& GetProgressToken() const { return Params.ProgressToken; }
	const FString& GetCancellationReason() const { return CancellationReason; }
	const FDateTime& GetStartedUtc() const { return StartedUtc; }
	bool IsCancellable() const { return Params.bCancellable; }
	bool IsCancellationRequested() const { return bCancellationRequested; }

	void RequestCancellation(const FString& Reason);
	TSharedPtr<FJsonObject> ToJson() const;

	static const FMonolithExecutionContext* GetCurrent();
	static bool HasCurrent();
	static FString JsonRpcIdToString(const TSharedPtr<FJsonValue>& Id);
	static FString ExtractProgressToken(const TSharedPtr<FJsonObject>& JsonRpcParams);
	static FString RedactSessionId(const FString& SessionId);
	static FString GenerateLocalToolCallId();

private:
	friend class FScopedMonolithExecutionContext;

	FParams Params;
	FThreadSafeBool bCancellationRequested;
	FString CancellationReason;
	FDateTime StartedUtc;
};

// RAII scope that publishes a context as the current thread-local execution context
// and restores the previous one on destruction (nesting-safe).
class MONOLITHCORE_API FScopedMonolithExecutionContext
{
public:
	explicit FScopedMonolithExecutionContext(FMonolithExecutionContext& InContext);
	~FScopedMonolithExecutionContext();

	FScopedMonolithExecutionContext(const FScopedMonolithExecutionContext&) = delete;
	FScopedMonolithExecutionContext& operator=(const FScopedMonolithExecutionContext&) = delete;

private:
	FMonolithExecutionContext* PreviousContext = nullptr;
};
