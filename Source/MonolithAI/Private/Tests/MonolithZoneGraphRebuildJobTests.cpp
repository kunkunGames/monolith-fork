#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithAIMassZoneGraphActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// P1b (PRD Spec 10): ai.rebuild_zone_graph gating by
// UMonolithSettings::bEnableZoneGraphRebuildJob. Off must preserve the legacy
// "unavailable" report with NO async fields (contract preservation §9); on must
// mint a pollable job and drive it to an honest terminal state (completed when
// WITH_ZONEGRAPH and GEditor are available, failed otherwise) — never a faked
// completion.

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
	const bool bOriginal = Settings->bEnableZoneGraphRebuildJob;
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

	Settings->bEnableZoneGraphRebuildJob = bOriginal;
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
	const bool bOriginal = Settings->bEnableZoneGraphRebuildJob;
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
		// Honest terminal state only — completed (real rebuild) or failed
		// (ZoneGraph absent / no editor). Never anything else, never faked.
		TestTrue(TEXT("Status is an honest terminal state"), Status == TEXT("completed") || Status == TEXT("failed"));

		// The minted job is queryable and shares the same terminal state.
		const FString JobId = Result.Result->GetStringField(TEXT("job_id"));
		TSharedPtr<FJsonObject> JobRow = JobRegistry.GetJobJson(JobId);
		TestTrue(TEXT("Minted job row is retrievable"), JobRow.IsValid());
		if (JobRow.IsValid())
		{
			TestEqual(TEXT("Registry row matches result status"), JobRow->GetStringField(TEXT("status")), Status);
		}
	}

	Settings->bEnableZoneGraphRebuildJob = bOriginal;
	JobRegistry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
