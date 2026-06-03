#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

class MONOLITHCORE_API FMonolithToolInvocationLogger
{
public:
	class MONOLITHCORE_API FScopedTrace
	{
	public:
		explicit FScopedTrace(
			const FString& TraceId,
			const FString& ParentSpanId = FString(),
			const FString& SpanId = FString(),
			const FString& SessionKey = FString(),
			const TSharedPtr<FJsonObject>& RoutingContext = nullptr);
		~FScopedTrace();

	private:
		FString PreviousTraceId;
		FString PreviousParentSpanId;
		FString PreviousSpanId;
		FString PreviousSessionKey;
		TSharedPtr<FJsonObject> PreviousRoutingContext;
	};

	static FString NowIso8601WithOffset();
	static double NowSeconds();
	static FString GenerateTraceId(const FString& Seed);
	static FString GenerateSpanId(const FString& Seed);
	static FString GetCurrentTraceId();
	static FString GetCurrentParentSpanId();
	static FString GetCurrentSpanId();
	static FString GetCurrentSessionKey();
	static TSharedPtr<FJsonObject> GetCurrentRoutingContext();
	static void ClearCurrentChildProcess();
	static void RecordChildProcess(
		const FString& Executable,
		const FString& ArgvSummary,
		double ExecProcessMs,
		int32 ExitCode,
		int64 StdoutBytes,
		int64 StderrBytes,
		const FString& TraceId,
		const FString& SpanId = FString());

	static void RecordAction(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FMonolithActionResult& Result,
		const FString& ValidationPhase,
		const FString& StartTime,
		double StartSeconds,
		const TSharedPtr<FJsonObject>& PhaseTiming = nullptr);

private:
	static bool IsEnabled();
};
