#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

class FMonolithToolInvocationLogger
{
public:
	class FScopedTrace
	{
	public:
		explicit FScopedTrace(const FString& TraceId);
		~FScopedTrace();

	private:
		FString PreviousTraceId;
	};

	static FString NowIso8601WithOffset();
	static double NowSeconds();
	static FString GenerateTraceId(const FString& Seed);
	static FString GetCurrentTraceId();

	static void RecordAction(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FMonolithActionResult& Result,
		const FString& ValidationPhase,
		const FString& StartTime,
		double StartSeconds);

private:
	static bool IsEnabled();
};
