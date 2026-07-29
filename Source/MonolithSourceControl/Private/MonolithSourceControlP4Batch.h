#pragma once

#include "CoreMinimal.h"

namespace MonolithSourceControlP4
{
	inline constexpr int32 MaxInputPathCount = 5000;
	inline constexpr int32 MaxUniquePathCount = 5000;
	inline constexpr int32 MaxPathsPerCommand = 128;
	inline constexpr int32 MaxCommandChars = 24000;
	inline constexpr int32 MaxCommandCount = 40;
	inline constexpr int32 MaxOpenedLimit = 5000;
	inline constexpr int32 MaxOpenedBackendRecordCount = MaxOpenedLimit + 1;
	inline constexpr int32 TimedOutReturnCode = -1001;
	inline constexpr double CommandTimeoutSeconds = 30.0;
	inline constexpr float ProcessPollIntervalSeconds = 0.01f;

	struct FDepotPathMapping
	{
		FString LocalPath;
		FString Error;

		bool IsResolved() const
		{
			return !LocalPath.IsEmpty();
		}
	};

	struct FDepotPathBatchResult
	{
		TMap<FString, FDepotPathMapping> Mappings;
		int32 RawPathCount = 0;
		int32 RequestedPathCount = 0;
		int32 UniquePathCount = 0;
		int32 CommandCount = 0;
		int32 ResolvedPathCount = 0;
		int32 FailedPathCount = 0;
		bool bRejected = false;
		FString Error;
	};

	struct FOpenedRecordWindow
	{
		int32 BackendRecordLimit = 0;
		int32 ObservedRecordCount = 0;
		int32 ReturnedRecordCount = 0;
		int32 SentinelRecordCount = 0;
		bool bHasMore = false;
		bool bCountIsLowerBound = false;
	};

	struct FProcessPollCallbacks
	{
		TFunction<bool()> IsRunning;
		TFunction<void()> TerminateAndWait;
		TFunction<FString()> ReadStdOut;
		TFunction<FString()> ReadStdErr;
		TFunction<bool(int32&)> GetReturnCode;
		TFunction<double()> NowSeconds;
		TFunction<void(float)> Sleep;
	};

	struct FProcessPollResult
	{
		FString StdOut;
		FString StdErr;
		int32 ReturnCode = INDEX_NONE;
		bool bTimedOut = false;
		bool bReturnCodeAvailable = false;
	};

	using FWhereBatchRunner = TFunction<bool(
		const TArray<FString>& /*DepotPaths*/,
		FString& /*OutStdOut*/,
		FString& /*OutStdErr*/,
		int32& /*OutReturnCode*/)>;

	/** Encodes one argument according to the Microsoft C runtime argv rules. */
	FString QuoteWindowsCommandLineArgument(const FString& Argument);

	/** Encodes one quote-free argument for Unreal's Unix CreateProc argv parser. */
	FString QuoteUnrealUnixCommandLineArgument(const FString& Argument);

	/** Encodes one argument for the host platform's FPlatformProcess command line. */
	FString QuoteCommandLineArgument(const FString& Argument);

	/** Returns false for CR/LF/NUL and every other control character. */
	bool ValidateCommandLineArgument(const FString& Argument, FString& OutError);

	/** Accepts an empty changelist filter, decimal changelist number, or `default`. */
	bool ValidateChangelist(const FString& Changelist, FString& OutError);

	/** Accepts only finite integral opened limits in the public [1, 5000] range. */
	bool TryValidateOpenedLimit(double LimitValue, int32& OutLimit, FString& OutError);

	/** Builds a bounded `p4 -ztag opened -m (Limit + 1)` argument string. */
	bool TryBuildOpenedCommandArgs(
		const FString& Changelist,
		int32 Limit,
		FString& OutArgs,
		int32& OutBackendRecordLimit,
		FString& OutError);

	/** Projects a bounded backend observation into returned/sentinel semantics. */
	FOpenedRecordWindow MakeOpenedRecordWindow(int32 ObservedRecordCount, int32 Limit);

	/**
	 * Polls an already-launched process, drains both output pipes, and terminates
	 * the process when the monotonic deadline expires.
	 */
	FProcessPollResult PollProcessWithTimeout(
		const FProcessPollCallbacks& Callbacks,
		double TimeoutSeconds,
		float PollIntervalSeconds = ProcessPollIntervalSeconds);

	/**
	 * Resolves depot/client paths through bounded `p4 -ztag where` batches.
	 *
	 * The injected runner receives one path chunk per command. This keeps process
	 * execution outside the pure batching/mapping logic and makes process-count,
	 * partial-failure, and command-boundary behavior deterministic in tests.
	 */
	FDepotPathBatchResult ResolveDepotPathsBatched(
		const TArray<FString>& DepotPaths,
		const FWhereBatchRunner& Runner,
		int32 InMaxPathsPerCommand = MaxPathsPerCommand,
		int32 InMaxCommandChars = MaxCommandChars,
		int32 InMaxCommandCount = MaxCommandCount);
}
