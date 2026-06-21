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

	const FString JobId = Registry.SubmitJob(TEXT("source"), TEXT("build_crg_graph"));
	Registry.UpdateProgress(JobId, 25.0, TEXT("building"), TEXT("phase 1"));

	TestFalse(TEXT("Cancel not requested initially"), Registry.IsCancelRequested(JobId));
	Registry.RequestCancel(JobId);
	TestTrue(TEXT("Cancel flag set"), Registry.IsCancelRequested(JobId));

	TSharedPtr<FJsonObject> Cancelled = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Status uses double-l cancelled token"), Cancelled->GetStringField(TEXT("status")), TEXT("cancelled"));

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
