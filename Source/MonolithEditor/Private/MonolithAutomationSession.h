#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FMonolithAsyncAutomationTestDescriptor
{
	FString FullPath;
	FString TestName;
	FString DisplayName;
	uint32 Flags = 0;
};

namespace MonolithAutomationAsync
{
	using FRunFinishedCallback = TFunction<void(const TSharedPtr<FJsonObject>&)>;

	/**
	 * Start one AutomationController-owned run. The call only queues worker discovery;
	 * the engine's normal controller/worker frame loop performs every test step.
	 */
	bool StartRun(
		const TArray<FMonolithAsyncAutomationTestDescriptor>& Tests,
		const TSharedPtr<FJsonObject>& Run,
		double DiscoveryTimeoutSeconds,
		double ReadinessTimeoutSeconds,
		double RunTimeoutSeconds,
		FRunFinishedCallback OnFinished,
		FString& OutError);

	/** Request bounded cancellation for the Monolith-owned run. */
	bool StopRun(
		const FString& ExpectedRunId,
		const FString& CompletionReason,
		TSharedPtr<FJsonObject>& OutRun,
		FString& OutError);

	/** Refresh progress/results from AutomationController without advancing engine time. */
	void RefreshSnapshot(bool bIncludeResults);

	bool IsActive();
	FString GetActiveRunId();

	/** Deterministic delegate/ticker cleanup for MonolithEditor module shutdown. */
	void Shutdown();
}
