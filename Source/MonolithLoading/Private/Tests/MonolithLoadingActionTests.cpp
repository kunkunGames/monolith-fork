#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MonolithLoadingActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLoadingRegistryAndValidationTest,
	"Monolith.Loading.RegistryAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLoadingRegistryAndValidationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	bool bHasStatus = false;
	bool bHasProcessors = false;
	bool bHasValidate = false;
	bool bHasTrace = false;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("loading")))
	{
		if (ActionInfo.Action == TEXT("get_status"))
		{
			bHasStatus = true;
		}
		else if (ActionInfo.Action == TEXT("describe_loading_processors"))
		{
			bHasProcessors = true;
		}
		else if (ActionInfo.Action == TEXT("validate_loading_reason_contract"))
		{
			bHasValidate = true;
		}
		else if (ActionInfo.Action == TEXT("trace_loading_screen_blockers"))
		{
			bHasTrace = true;
		}
	}

	TestTrue(TEXT("loading.get_status registered"), bHasStatus);
	TestTrue(TEXT("loading.describe_loading_processors registered"), bHasProcessors);
	TestTrue(TEXT("loading.validate_loading_reason_contract registered"), bHasValidate);
	TestTrue(TEXT("loading.trace_loading_screen_blockers registered"), bHasTrace);
	for (const TCHAR* Action : {
		TEXT("get_status"),
		TEXT("describe_loading_processors"),
		TEXT("validate_loading_reason_contract"),
		TEXT("trace_loading_screen_blockers")
	})
	{
		TestEqual(
			FString::Printf(TEXT("loading.%s is read-only"), Action),
			Registry.GetActionExecutionPolicy(TEXT("loading"), Action).PolicyId,
			FString(TEXT("read_only")));
	}

	const FMonolithActionResult StatusResult = FMonolithLoadingActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), StatusResult.bSuccess);
	TestTrue(TEXT("get_status result object is valid"), StatusResult.Result.IsValid());

	const FMonolithActionResult ProcessorResult = FMonolithLoadingActions::DescribeLoadingProcessors(MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_loading_processors succeeds"), ProcessorResult.bSuccess);
	TestTrue(TEXT("describe_loading_processors result object is valid"), ProcessorResult.Result.IsValid());

	TSharedPtr<FJsonObject> ValidationParams = MakeShared<FJsonObject>();
	ValidationParams->SetBoolField(TEXT("include_known_lyra"), true);
	ValidationParams->SetBoolField(TEXT("strict"), false);
	const FMonolithActionResult ValidationResult = FMonolithLoadingActions::ValidateLoadingReasonContract(ValidationParams);
	TestTrue(TEXT("validate_loading_reason_contract succeeds"), ValidationResult.bSuccess);
	TestTrue(TEXT("validate_loading_reason_contract result object is valid"), ValidationResult.Result.IsValid());
	if (ValidationResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("validation ok field exists"), ValidationResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("CommonLoadingScreen reason contract is valid"), bOk);
	}

	TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
	TraceParams->SetStringField(TEXT("world_context"), TEXT("pie"));
	TraceParams->SetBoolField(TEXT("include_processor_candidates"), false);
	const FMonolithActionResult TraceResult = FMonolithLoadingActions::TraceLoadingScreenBlockers(TraceParams);
	TestTrue(TEXT("trace_loading_screen_blockers succeeds without requiring PIE"), TraceResult.bSuccess);
	TestTrue(TEXT("trace_loading_screen_blockers result object is valid"), TraceResult.Result.IsValid());
	if (TraceResult.Result.IsValid())
	{
		FString Status;
		TestTrue(TEXT("trace status exists"), TraceResult.Result->TryGetStringField(TEXT("status"), Status));
		TestTrue(TEXT("trace returns a status string"), !Status.IsEmpty());
	}

	TSharedPtr<FJsonObject> BadTraceParams = MakeShared<FJsonObject>();
	BadTraceParams->SetStringField(TEXT("world_context"), TEXT("editor"));
	const FMonolithActionResult BadTraceResult = FMonolithLoadingActions::TraceLoadingScreenBlockers(BadTraceParams);
	TestFalse(TEXT("trace rejects unsupported world_context"), BadTraceResult.bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
