#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithCoreTools.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// P1b (PRD Spec 10): handler-level coverage for monolith.get_job /
// monolith.cancel_job and the reindex job_id wiring, gated by
// UMonolithSettings::bEnableAsyncJobs. The async registry's own lifecycle is
// covered by MonolithAsyncJobRegistryTests; here we assert the disabled/enabled
// handler contracts only.

namespace
{
	TSharedPtr<FJsonObject> MakeJobIdParams(const FString& JobId)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("job_id"), JobId);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGetJobDisabledTest,
	"Monolith.Core.AsyncJobActions.GetJobDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGetJobDisabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	FMonolithAsyncJobRegistry::Get().ResetForTests();
	const bool bOriginal = Settings->bEnableAsyncJobs;
	Settings->bEnableAsyncJobs = false;

	FMonolithActionResult Result = FMonolithCoreTools::HandleGetJob(MakeJobIdParams(TEXT("any-id")));
	TestTrue(TEXT("get_job succeeds as a report when disabled"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Disabled status reported"), Result.Result->GetStringField(TEXT("status")), TEXT("disabled"));
		TestEqual(TEXT("Requested job id echoed"), Result.Result->GetStringField(TEXT("requested_job_id")), TEXT("any-id"));
	}

	// Missing job_id is an invalid-params error regardless of the flag.
	FMonolithActionResult Missing = FMonolithCoreTools::HandleGetJob(MakeShared<FJsonObject>());
	TestFalse(TEXT("Missing job_id fails"), Missing.bSuccess);
	TestEqual(TEXT("Missing job_id error code"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	Settings->bEnableAsyncJobs = bOriginal;
	FMonolithAsyncJobRegistry::Get().ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGetJobEnabledTest,
	"Monolith.Core.AsyncJobActions.GetJobEnabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGetJobEnabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();
	const bool bOriginal = Settings->bEnableAsyncJobs;
	Settings->bEnableAsyncJobs = true;

	// A real submitted job is reflected back through the handler.
	const FString JobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	FMonolithActionResult Known = FMonolithCoreTools::HandleGetJob(MakeJobIdParams(JobId));
	TestTrue(TEXT("get_job succeeds when enabled"), Known.bSuccess);
	if (Known.Result.IsValid())
	{
		TestEqual(TEXT("Pending status surfaced"), Known.Result->GetStringField(TEXT("status")), TEXT("pending"));
		TestEqual(TEXT("Namespace echoed"), Known.Result->GetStringField(TEXT("namespace")), TEXT("project"));
		TestEqual(TEXT("Action echoed"), Known.Result->GetStringField(TEXT("action")), TEXT("reindex"));
	}

	// Unknown id surfaces the registry's not_found, never an error.
	FMonolithActionResult Unknown = FMonolithCoreTools::HandleGetJob(MakeJobIdParams(TEXT("does-not-exist")));
	TestTrue(TEXT("Unknown id still succeeds"), Unknown.bSuccess);
	if (Unknown.Result.IsValid())
	{
		TestEqual(TEXT("Unknown id is not_found"), Unknown.Result->GetStringField(TEXT("status")), TEXT("not_found"));
	}

	Settings->bEnableAsyncJobs = bOriginal;
	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCancelJobTest,
	"Monolith.Core.AsyncJobActions.CancelJob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCancelJobTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();
	const bool bOriginal = Settings->bEnableAsyncJobs;

	// Disabled: report-only, no registry mutation.
	Settings->bEnableAsyncJobs = false;
	FMonolithActionResult Disabled = FMonolithCoreTools::HandleCancelJob(MakeJobIdParams(TEXT("any-id")));
	TestTrue(TEXT("cancel_job succeeds as a report when disabled"), Disabled.bSuccess);
	if (Disabled.Result.IsValid())
	{
		TestEqual(TEXT("Disabled status reported"), Disabled.Result->GetStringField(TEXT("status")), TEXT("disabled"));
		TestFalse(TEXT("Disabled reports no cancel"), Disabled.Result->GetBoolField(TEXT("cancel_requested")));
	}

	// Enabled: cancellation flips the registry row to the double-l cancelled token.
	Settings->bEnableAsyncJobs = true;
	const FString JobId = Registry.SubmitJob(TEXT("ai"), TEXT("rebuild_zone_graph"));
	Registry.UpdateProgress(JobId, 5.0, TEXT("rebuilding"), TEXT("started"));
	TestFalse(TEXT("Cancel not requested initially"), Registry.IsCancelRequested(JobId));

	FMonolithActionResult Cancelled = FMonolithCoreTools::HandleCancelJob(MakeJobIdParams(JobId));
	TestTrue(TEXT("cancel_job succeeds when enabled"), Cancelled.bSuccess);
	TestTrue(TEXT("Cancel flag is set on the registry row"), Registry.IsCancelRequested(JobId));
	if (Cancelled.Result.IsValid())
	{
		TestEqual(TEXT("Returned row uses cancelled token"), Cancelled.Result->GetStringField(TEXT("status")), TEXT("cancelled"));
	}

	Settings->bEnableAsyncJobs = bOriginal;
	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
