#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Bounded in-memory registry for long-running asynchronous Monolith jobs.
 *
 * A job is minted by SubmitJob and progresses through a small lifecycle state
 * machine (Pending -> Running -> Completed | Failed | Cancelled). Callers poll
 * GetJobJson / ListJobsJson for status; cancellation is cooperative only — the
 * registry sets a flag that the running action is expected to observe.
 *
 * The registry stores only coarse status/progress metadata plus an optional
 * result/error JSON object. It never spawns or owns worker threads; the action
 * that submitted the job is responsible for driving it to a terminal state.
 *
 * Header lives in Public/ so MonolithAI (and other modules) can include it.
 */
enum class EMonolithAsyncJobStatus : uint8
{
	Pending,
	Running,
	Completed,
	Failed,
	Cancelled
};

class MONOLITHCORE_API FMonolithAsyncJobRegistry
{
public:
	static FMonolithAsyncJobRegistry& Get();

	/** Mints a new job id, seeds a Pending row, and returns the job id. */
	FString SubmitJob(const FString& Namespace, const FString& Action);

	/** Marks the job Running and updates its progress fields. */
	void UpdateProgress(const FString& JobId, double Percent, const FString& Stage, const FString& Message);

	/** Marks the job Completed and attaches the optional result object. */
	void CompleteJob(const FString& JobId, const TSharedPtr<FJsonObject>& Result);

	/** Marks the job Failed and records the error string. */
	void FailJob(const FString& JobId, const FString& Error);

	/** Sets the cooperative cancel flag; does not interrupt running work. */
	void RequestCancel(const FString& JobId);

	/** Returns true if cancellation has been requested for the job. */
	bool IsCancelRequested(const FString& JobId) const;

	/** Returns the job as JSON, or {"status":"not_found"} for an unknown id. */
	TSharedPtr<FJsonObject> GetJobJson(const FString& JobId) const;

	/** Returns up to Limit jobs (UpdatedUtc desc), Limit clamped to 1..1000. */
	TSharedPtr<FJsonObject> ListJobsJson(int32 Limit) const;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	struct FJobRow
	{
		FString JobId;
		FString Namespace;
		FString Action;
		EMonolithAsyncJobStatus Status = EMonolithAsyncJobStatus::Pending;
		double ProgressPercent = 0.0;
		FString ProgressStage;
		FString ProgressMessage;
		TSharedPtr<FJsonObject> Result;
		FString Error;
		bool bCancelRequested = false;
		FDateTime CreatedUtc;
		FDateTime UpdatedUtc;
	};

	static constexpr int32 JobCapacity = 128;

	static FString MakeJobId();
	static FString StatusToken(EMonolithAsyncJobStatus Status);
	static TSharedPtr<FJsonObject> RowToJson(const FJobRow& Row);

	void EvictOldestIfNeeded(const FString& IncomingJobId);

	mutable FCriticalSection RegistryLock;
	TMap<FString, FJobRow> RowsById;
};
