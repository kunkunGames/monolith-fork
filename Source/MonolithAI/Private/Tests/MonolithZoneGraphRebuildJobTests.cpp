#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithAIMassZoneGraphActions.h"
#include "Dom/JsonObject.h"
#include "Editor.h"

#if WITH_DEV_AUTOMATION_TESTS

// P1b (PRD Spec 10): ai.rebuild_zone_graph gating by
// UMonolithSettings::bEnableAsyncJobs and bEnableZoneGraphRebuildJob. Either
// flag off must preserve the legacy "unavailable" report with NO async fields
// (contract preservation §9); both flags on must mint a pollable job and drive
// it to an honest terminal state (completed when WITH_ZONEGRAPH and GEditor are
// available, failed otherwise) — never a faked completion.

namespace
{
	void EnsureRebuildZoneGraphRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("ai"), TEXT("rebuild_zone_graph")))
		{
			FMonolithAIMassZoneGraphActions::RegisterActions(Registry);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRebuildZoneGraphDisabledTest,
	"Monolith.AI.ZoneGraphRebuildJob.Disabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebuildZoneGraphDisabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	EnsureRebuildZoneGraphRegistered();
	FMonolithAsyncJobRegistry::Get().ResetForTests();
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	const bool bOriginalAsyncJobs = Settings->bEnableAsyncJobs;
	const bool bOriginalZoneGraph = Settings->bEnableZoneGraphRebuildJob;
	Settings->bEnableAsyncJobs = true;
	Settings->bEnableZoneGraphRebuildJob = false;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("rebuild_zone_graph"), MakeShared<FJsonObject>());
	TestTrue(TEXT("rebuild_zone_graph succeeds as a report when disabled"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Legacy unavailable status preserved"), Result.Result->GetStringField(TEXT("status")), TEXT("unavailable"));
		// Contract preservation: no async fields leak into the disabled response.
		TestFalse(TEXT("No job_id when disabled"), Result.Result->HasField(TEXT("job_id")));
		TestFalse(TEXT("No poll_action when disabled"), Result.Result->HasField(TEXT("poll_action")));
	}

	Settings->bEnableAsyncJobs = bOriginalAsyncJobs;
	Settings->bEnableZoneGraphRebuildJob = bOriginalZoneGraph;
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	FMonolithAsyncJobRegistry::Get().ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRebuildZoneGraphAsyncJobsDisabledTest,
	"Monolith.AI.ZoneGraphRebuildJob.AsyncJobsDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebuildZoneGraphAsyncJobsDisabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	EnsureRebuildZoneGraphRegistered();
	FMonolithAsyncJobRegistry::Get().ResetForTests();
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	const bool bOriginalAsyncJobs = Settings->bEnableAsyncJobs;
	const bool bOriginalZoneGraph = Settings->bEnableZoneGraphRebuildJob;
	Settings->bEnableAsyncJobs = false;
	Settings->bEnableZoneGraphRebuildJob = true;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("rebuild_zone_graph"), MakeShared<FJsonObject>());
	TestTrue(TEXT("rebuild_zone_graph succeeds as a report when async jobs are disabled"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Legacy unavailable status preserved"), Result.Result->GetStringField(TEXT("status")), TEXT("unavailable"));
		TestFalse(TEXT("No job_id when async jobs are disabled"), Result.Result->HasField(TEXT("job_id")));
		TestFalse(TEXT("No poll_action when async jobs are disabled"), Result.Result->HasField(TEXT("poll_action")));
	}

	Settings->bEnableAsyncJobs = bOriginalAsyncJobs;
	Settings->bEnableZoneGraphRebuildJob = bOriginalZoneGraph;
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	FMonolithAsyncJobRegistry::Get().ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRebuildZoneGraphEnabledTest,
	"Monolith.AI.ZoneGraphRebuildJob.Enabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebuildZoneGraphEnabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	EnsureRebuildZoneGraphRegistered();
	FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
	JobRegistry.ResetForTests();
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	const bool bOriginalAsyncJobs = Settings->bEnableAsyncJobs;
	const bool bOriginalZoneGraph = Settings->bEnableZoneGraphRebuildJob;
	Settings->bEnableAsyncJobs = true;
	Settings->bEnableZoneGraphRebuildJob = true;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("rebuild_zone_graph"), MakeShared<FJsonObject>());
	TestTrue(TEXT("rebuild_zone_graph succeeds when enabled"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		// A pollable job is always minted regardless of the WITH_ZONEGRAPH/GEditor
		// outcome.
		TestTrue(TEXT("job_id present when enabled"), Result.Result->HasField(TEXT("job_id")));
		TestEqual(TEXT("poll_action points at monolith.get_job"), Result.Result->GetStringField(TEXT("poll_action")), TEXT("monolith.get_job"));

		const FString Status = Result.Result->GetStringField(TEXT("status"));
		// Honest terminal state only — completed (real rebuild), failed
		// (ZoneGraph absent / no editor), or cancelled if a concurrent caller
		// requested cancellation before completion. Never anything else, never faked.
		TestTrue(TEXT("Status is an honest terminal state"),
			Status == TEXT("completed") || Status == TEXT("failed") || Status == TEXT("cancelled"));

		// The minted job is queryable and shares the same terminal state.
		const FString JobId = Result.Result->GetStringField(TEXT("job_id"));
		TSharedPtr<FJsonObject> JobRow = JobRegistry.GetJobJson(JobId);
		TestTrue(TEXT("Minted job row is retrievable"), JobRow.IsValid());
		if (JobRow.IsValid())
		{
			TestEqual(TEXT("Registry row matches result status"), JobRow->GetStringField(TEXT("status")), Status);
		}
	}

	Settings->bEnableAsyncJobs = bOriginalAsyncJobs;
	Settings->bEnableZoneGraphRebuildJob = bOriginalZoneGraph;
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	JobRegistry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRebuildZoneGraphCancelledAfterSubmitTest,
	"Monolith.AI.ZoneGraphRebuildJob.CancelledAfterSubmit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebuildZoneGraphCancelledAfterSubmitTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	EnsureRebuildZoneGraphRegistered();
	FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
	JobRegistry.ResetForTests();
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	const bool bOriginalAsyncJobs = Settings->bEnableAsyncJobs;
	const bool bOriginalZoneGraph = Settings->bEnableZoneGraphRebuildJob;
	Settings->bEnableAsyncJobs = true;
	Settings->bEnableZoneGraphRebuildJob = true;

	FMonolithAIMassZoneGraphActions::SetRebuildZoneGraphJobSubmittedHookForTests(
		[&JobRegistry](const FString& JobId)
		{
			JobRegistry.RequestCancel(JobId);
		});

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("rebuild_zone_graph"), MakeShared<FJsonObject>());
	TestTrue(TEXT("rebuild_zone_graph returns a cancellation report"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Action result is cancelled"), Result.Result->GetStringField(TEXT("status")), TEXT("cancelled"));
		const FString JobId = Result.Result->GetStringField(TEXT("job_id"));
		TSharedPtr<FJsonObject> JobRow = JobRegistry.GetJobJson(JobId);
		TestTrue(TEXT("Registry row is retrievable after submit cancellation"), JobRow.IsValid());
		if (JobRow.IsValid())
		{
			TestEqual(TEXT("Registry row is cancelled"), JobRow->GetStringField(TEXT("status")), TEXT("cancelled"));
		}
	}

	Settings->bEnableAsyncJobs = bOriginalAsyncJobs;
	Settings->bEnableZoneGraphRebuildJob = bOriginalZoneGraph;
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	JobRegistry.ResetForTests();
	return true;
}

#if WITH_ZONEGRAPH
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRebuildZoneGraphCancelledBeforeBroadcastTest,
	"Monolith.AI.ZoneGraphRebuildJob.CancelledBeforeBroadcast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebuildZoneGraphCancelledBeforeBroadcastTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	EnsureRebuildZoneGraphRegistered();
	FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
	JobRegistry.ResetForTests();
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();

	if (!GEditor)
	{
		AddInfo(TEXT("Skipping pre-broadcast cancellation test because GEditor is unavailable."));
		return true;
	}

	const bool bOriginalAsyncJobs = Settings->bEnableAsyncJobs;
	const bool bOriginalZoneGraph = Settings->bEnableZoneGraphRebuildJob;
	Settings->bEnableAsyncJobs = true;
	Settings->bEnableZoneGraphRebuildJob = true;

	FMonolithAIMassZoneGraphActions::SetRebuildZoneGraphBeforeBroadcastHookForTests(
		[&JobRegistry](const FString& JobId)
		{
			JobRegistry.RequestCancel(JobId);
		});

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("rebuild_zone_graph"), MakeShared<FJsonObject>());
	TestTrue(TEXT("rebuild_zone_graph returns a pre-broadcast cancellation report"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Action result is cancelled before broadcast"), Result.Result->GetStringField(TEXT("status")), TEXT("cancelled"));
		const FString JobId = Result.Result->GetStringField(TEXT("job_id"));
		TSharedPtr<FJsonObject> JobRow = JobRegistry.GetJobJson(JobId);
		TestTrue(TEXT("Registry row is retrievable before broadcast cancellation"), JobRow.IsValid());
		if (JobRow.IsValid())
		{
			TestEqual(TEXT("Registry row is cancelled before broadcast"), JobRow->GetStringField(TEXT("status")), TEXT("cancelled"));
		}
	}

	Settings->bEnableAsyncJobs = bOriginalAsyncJobs;
	Settings->bEnableZoneGraphRebuildJob = bOriginalZoneGraph;
	FMonolithAIMassZoneGraphActions::ClearRebuildZoneGraphTestHooks();
	JobRegistry.ResetForTests();
	return true;
}
#endif // WITH_ZONEGRAPH

#endif // WITH_DEV_AUTOMATION_TESTS
