#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAsyncJobRegistry.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAsyncJobRegistryLifecycleTest,
	"Monolith.Core.AsyncJobRegistry.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAsyncJobRegistryLifecycleTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	const FString JobId = Registry.SubmitJob(TEXT("source"), TEXT("trigger_reindex"));
	TestFalse(TEXT("Submitted job id is non-empty"), JobId.IsEmpty());

	TSharedPtr<FJsonObject> Pending = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Seeded status is pending"), Pending->GetStringField(TEXT("status")), TEXT("pending"));
	TestEqual(TEXT("Namespace echoed"), Pending->GetStringField(TEXT("namespace")), TEXT("source"));
	TestEqual(TEXT("Action echoed"), Pending->GetStringField(TEXT("action")), TEXT("trigger_reindex"));
	TestTrue(TEXT("Default job is cancellable"), Pending->GetBoolField(TEXT("cancellable")));
	TestTrue(TEXT("Default job supports progress"), Pending->GetBoolField(TEXT("supports_progress")));
	TestFalse(TEXT("Cancel not requested by default"), Pending->GetBoolField(TEXT("cancel_requested")));
	TestEqual(TEXT("Default poll action"), Pending->GetStringField(TEXT("poll_action")), TEXT("monolith.get_job"));
	TestEqual(TEXT("Default cancel action"), Pending->GetStringField(TEXT("cancel_action")), TEXT("monolith.cancel_job"));

	Registry.UpdateProgress(JobId, 42.0, TEXT("indexing"), TEXT("walking assets"));
	TSharedPtr<FJsonObject> Running = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Progress flips to running"), Running->GetStringField(TEXT("status")), TEXT("running"));
	const TSharedPtr<FJsonObject>* ProgressObj = nullptr;
	TestTrue(TEXT("progress object exists"), Running->TryGetObjectField(TEXT("progress"), ProgressObj));
	if (ProgressObj)
	{
		TestEqual(TEXT("Percent recorded"), (*ProgressObj)->GetNumberField(TEXT("percent")), 42.0);
		TestEqual(TEXT("Stage recorded"), (*ProgressObj)->GetStringField(TEXT("stage")), TEXT("indexing"));
		TestEqual(TEXT("Message recorded"), (*ProgressObj)->GetStringField(TEXT("message")), TEXT("walking assets"));
	}

	TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
	ResultPayload->SetNumberField(TEXT("indexed"), 1234);
	Registry.CompleteJob(JobId, ResultPayload);

	TSharedPtr<FJsonObject> Completed = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Status is completed"), Completed->GetStringField(TEXT("status")), TEXT("completed"));
	const TSharedPtr<FJsonObject>* CompletedProgress = nullptr;
	if (Completed->TryGetObjectField(TEXT("progress"), CompletedProgress) && CompletedProgress)
	{
		TestEqual(TEXT("Percent forced to 100 on completion"), (*CompletedProgress)->GetNumberField(TEXT("percent")), 100.0);
	}
	const TSharedPtr<FJsonObject>* CompletedResult = nullptr;
	TestTrue(TEXT("result attached"), Completed->TryGetObjectField(TEXT("result"), CompletedResult));
	if (CompletedResult)
	{
		TestEqual(TEXT("Result payload preserved"), (*CompletedResult)->GetIntegerField(TEXT("indexed")), 1234);
	}

	Registry.RequestCancel(JobId);
	TSharedPtr<FJsonObject> CompletedAfterCancel = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Completed status stays terminal after cancel request"), CompletedAfterCancel->GetStringField(TEXT("status")), TEXT("completed"));
	TestTrue(TEXT("Cancel flag is still recorded on terminal completed job"), Registry.IsCancelRequested(JobId));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAsyncJobRegistryNonCancellableTest,
	"Monolith.Core.AsyncJobRegistry.NonCancellable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAsyncJobRegistryNonCancellableTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	const FString JobId = Registry.SubmitJob(
		TEXT("monolith"),
		TEXT("reindex"),
		/*bCancellable=*/false,
		/*bSupportsProgress=*/false,
		TEXT("monolith.status"),
		FString());
	Registry.UpdateProgress(JobId, 0.0, TEXT("external_process"), TEXT("Started external reindex process."));

	TSharedPtr<FJsonObject> Running = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Non-cancellable job is running before cancel request"), Running->GetStringField(TEXT("status")), TEXT("running"));
	TestFalse(TEXT("Non-cancellable flag is surfaced"), Running->GetBoolField(TEXT("cancellable")));
	TestFalse(TEXT("No progress support is surfaced"), Running->GetBoolField(TEXT("supports_progress")));
	TestEqual(TEXT("External poll action is surfaced"), Running->GetStringField(TEXT("poll_action")), TEXT("monolith.status"));
	TestFalse(TEXT("No cancel action is emitted"), Running->HasField(TEXT("cancel_action")));

	Registry.RequestCancel(JobId);
	TSharedPtr<FJsonObject> AfterCancel = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Non-cancellable job is not marked cancelled"), AfterCancel->GetStringField(TEXT("status")), TEXT("running"));
	TestFalse(TEXT("Non-cancellable job does not record cancel requested"), AfterCancel->GetBoolField(TEXT("cancel_requested")));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAsyncJobRegistryFailAndErrorTest,
	"Monolith.Core.AsyncJobRegistry.FailAndError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAsyncJobRegistryFailAndErrorTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	const FString JobId = Registry.SubmitJob(TEXT("ai"), TEXT("rebuild_zone_graph"));
	Registry.UpdateProgress(JobId, 10.0, TEXT("rebuilding"), TEXT("started"));
	Registry.FailJob(JobId, TEXT("ZoneGraph subsystem unavailable"));

	TSharedPtr<FJsonObject> Failed = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Status is failed"), Failed->GetStringField(TEXT("status")), TEXT("failed"));
	TestEqual(TEXT("Error recorded"), Failed->GetStringField(TEXT("error")), TEXT("ZoneGraph subsystem unavailable"));

	// A terminal failed job is not dragged back to running by a late progress update.
	Registry.UpdateProgress(JobId, 99.0, TEXT("late"), TEXT("ignored"));
	TSharedPtr<FJsonObject> StillFailed = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Failed status is terminal"), StillFailed->GetStringField(TEXT("status")), TEXT("failed"));
	const TSharedPtr<FJsonObject>* ProgressObj = nullptr;
	if (StillFailed->TryGetObjectField(TEXT("progress"), ProgressObj) && ProgressObj)
	{
		TestEqual(TEXT("Failed progress percent is not overwritten"), (*ProgressObj)->GetNumberField(TEXT("percent")), 10.0);
		TestEqual(TEXT("Failed progress stage is not overwritten"), (*ProgressObj)->GetStringField(TEXT("stage")), TEXT("rebuilding"));
		TestEqual(TEXT("Failed progress message is not overwritten"), (*ProgressObj)->GetStringField(TEXT("message")), TEXT("started"));
	}

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAsyncJobRegistryCancellationTest,
	"Monolith.Core.AsyncJobRegistry.Cancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAsyncJobRegistryCancellationTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	const FString JobId = Registry.SubmitJob(TEXT("monolith"), TEXT("reindex"));
	Registry.UpdateProgress(JobId, 25.0, TEXT("building"), TEXT("phase 1"));

	TestFalse(TEXT("Cancel not requested initially"), Registry.IsCancelRequested(JobId));
	Registry.RequestCancel(JobId);
	TestTrue(TEXT("Cancel flag set"), Registry.IsCancelRequested(JobId));

	TSharedPtr<FJsonObject> CancelRequested = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Status remains running until producer acknowledges cancellation"), CancelRequested->GetStringField(TEXT("status")), TEXT("running"));
	TestTrue(TEXT("Row reports cancel_requested"), CancelRequested->GetBoolField(TEXT("cancel_requested")));

	Registry.UpdateProgress(JobId, 50.0, TEXT("winding_down"), TEXT("producer still winding down"));
	TSharedPtr<FJsonObject> StillRunning = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Progress can update while cancellation is pending"), StillRunning->GetStringField(TEXT("status")), TEXT("running"));

	Registry.CancelJob(JobId, TEXT("producer acknowledged cancellation"));
	TSharedPtr<FJsonObject> Cancelled = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Status uses double-l cancelled token after producer ack"), Cancelled->GetStringField(TEXT("status")), TEXT("cancelled"));

	// Cancelled is terminal: late producer updates must not mask acknowledged
	// cancellation with a fake completion or failure.
	Registry.UpdateProgress(JobId, 90.0, TEXT("late"), TEXT("ignored"));
	TSharedPtr<FJsonObject> StillCancelledAfterProgress = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Cancelled status ignores late progress"), StillCancelledAfterProgress->GetStringField(TEXT("status")), TEXT("cancelled"));
	if (StillCancelledAfterProgress->HasTypedField<EJson::Object>(TEXT("progress")))
	{
		const TSharedPtr<FJsonObject>* ProgressObj = nullptr;
		if (StillCancelledAfterProgress->TryGetObjectField(TEXT("progress"), ProgressObj) && ProgressObj)
		{
			TestEqual(TEXT("Cancelled progress percent is not overwritten"), (*ProgressObj)->GetNumberField(TEXT("percent")), 50.0);
			TestEqual(TEXT("Cancelled progress stage is not overwritten"), (*ProgressObj)->GetStringField(TEXT("stage")), TEXT("cancelled"));
		}
	}

	TSharedPtr<FJsonObject> LateResult = MakeShared<FJsonObject>();
	LateResult->SetBoolField(TEXT("should_not_attach"), true);
	Registry.CompleteJob(JobId, LateResult);
	TSharedPtr<FJsonObject> StillCancelledAfterComplete = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Cancelled status ignores late complete"), StillCancelledAfterComplete->GetStringField(TEXT("status")), TEXT("cancelled"));
	TestFalse(TEXT("Late complete result is not attached"), StillCancelledAfterComplete->HasField(TEXT("result")));

	Registry.FailJob(JobId, TEXT("late failure"));
	TSharedPtr<FJsonObject> StillCancelledAfterFail = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Cancelled status ignores late fail"), StillCancelledAfterFail->GetStringField(TEXT("status")), TEXT("cancelled"));
	TestFalse(TEXT("Late fail error is not attached"), StillCancelledAfterFail->HasField(TEXT("error")));

	// Unknown id surfaces not_found and reports no cancellation.
	TSharedPtr<FJsonObject> Unknown = Registry.GetJobJson(TEXT("00000000-0000-0000-0000-000000000000"));
	TestEqual(TEXT("Unknown id is not_found"), Unknown->GetStringField(TEXT("status")), TEXT("not_found"));
	TestFalse(TEXT("Unknown id reports no cancel"), Registry.IsCancelRequested(TEXT("00000000-0000-0000-0000-000000000000")));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAsyncJobRegistryBoundedRowsTest,
	"Monolith.Core.AsyncJobRegistry.BoundedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAsyncJobRegistryBoundedRowsTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	TArray<FString> SubmittedIds;
	SubmittedIds.Reserve(140);
	for (int32 Index = 0; Index < 140; ++Index)
	{
		// Distinct UpdatedUtc per row so oldest-eviction is deterministic.
		const FString JobId = Registry.SubmitJob(TEXT("source"), TEXT("trigger_reindex"));
		Registry.UpdateProgress(JobId, static_cast<double>(Index % 100), TEXT("stage"), FString::Printf(TEXT("job %d"), Index));
		SubmittedIds.Add(JobId);
	}

	TSharedPtr<FJsonObject> Result = Registry.ListJobsJson(1000);
	TestEqual(TEXT("Job count is capped at JobCapacity"),
		Result->GetIntegerField(TEXT("job_count")), Result->GetIntegerField(TEXT("job_capacity")));
	TestEqual(TEXT("Job count is 128"), Result->GetIntegerField(TEXT("job_count")), 128);
	TestEqual(TEXT("Returned count matches job count"),
		Result->GetIntegerField(TEXT("returned_count")), Result->GetIntegerField(TEXT("job_count")));

	// The 12 oldest submissions (0..11) should have been evicted; the newest remains.
	TSharedPtr<FJsonObject> Oldest = Registry.GetJobJson(SubmittedIds[0]);
	TestEqual(TEXT("Oldest submitted job was evicted"), Oldest->GetStringField(TEXT("status")), TEXT("not_found"));
	TSharedPtr<FJsonObject> Newest = Registry.GetJobJson(SubmittedIds[139]);
	TestNotEqual(TEXT("Newest submitted job is retained"), Newest->GetStringField(TEXT("status")), TEXT("not_found"));

	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
