#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"
#include "Network/FNetworkQueryAdapter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNetworkQueryLimitContractTest,
	"Monolith.ReflectionIntel.Network.LimitContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNetworkQueryLimitContractTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("network"), TEXT("list_replicated_classes")))
	{
		FNetworkQueryAdapter::RegisterActions(Registry);
	}

	const TArray<FString> Actions = {
		TEXT("list_replicated_classes"),
		TEXT("list_rpc_functions"),
		TEXT("list_onrep_handlers"),
		TEXT("audit_unbalanced_onreps"),
	};
	const TArray<TPair<FString, TSharedPtr<FJsonValue>>> InvalidLimits = {
		{ TEXT("below range"), MakeShared<FJsonValueNumber>(0.0) },
		{ TEXT("above range"), MakeShared<FJsonValueNumber>(201.0) },
		{ TEXT("fractional"), MakeShared<FJsonValueNumber>(1.5) },
		{ TEXT("string-coerced"), MakeShared<FJsonValueString>(TEXT("1")) },
	};
	const TArray<FMonolithActionInfo> RegisteredActions = Registry.GetActions(TEXT("network"));

	for (const FString& Action : Actions)
	{
		const FMonolithActionInfo* ActionInfo = RegisteredActions.FindByPredicate(
			[&Action](const FMonolithActionInfo& Candidate)
			{
				return Candidate.Action == Action;
			});
		TestNotNull(FString::Printf(TEXT("%s is registered"), *Action), ActionInfo);
		if (ActionInfo && ActionInfo->ParamSchema.IsValid())
		{
			const TSharedPtr<FJsonObject>* LimitSchema = nullptr;
			TestTrue(
				FString::Printf(TEXT("%s limit schema exists"), *Action),
				ActionInfo->ParamSchema->TryGetObjectField(TEXT("limit"), LimitSchema));
			if (LimitSchema && LimitSchema->IsValid())
			{
				double Minimum = 0.0;
				double Maximum = 0.0;
				TestTrue(TEXT("minimum exists"), (*LimitSchema)->TryGetNumberField(TEXT("minimum"), Minimum));
				TestTrue(TEXT("maximum exists"), (*LimitSchema)->TryGetNumberField(TEXT("maximum"), Maximum));
				TestEqual(TEXT("minimum is 1"), Minimum, 1.0);
				TestEqual(TEXT("maximum is 200"), Maximum, 200.0);
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& InvalidLimit : InvalidLimits)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetField(TEXT("limit"), InvalidLimit.Value);
			const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("network"), Action, Params);
			TestFalse(
				FString::Printf(TEXT("%s rejects %s limit"), *Action, *InvalidLimit.Key),
				Result.bSuccess);
			TestTrue(
				FString::Printf(TEXT("%s %s error names limit"), *Action, *InvalidLimit.Key),
				Result.ErrorMessage.Contains(TEXT("limit")));
		}
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNetworkQueryStringContractTest,
	"Monolith.ParamGuard.ReflectionIntel.NetworkStringContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNetworkQueryStringContractTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("network"), TEXT("list_replicated_classes")))
	{
		FNetworkQueryAdapter::RegisterActions(Registry);
	}

	const TArray<TPair<FString, FString>> StringContracts = {
		{ TEXT("list_replicated_classes"), TEXT("cursor") },
		{ TEXT("list_rpc_functions"), TEXT("class_name") },
		{ TEXT("list_rpc_functions"), TEXT("rpc_kind") },
		{ TEXT("list_rpc_functions"), TEXT("cursor") },
		{ TEXT("list_onrep_handlers"), TEXT("class_name") },
		{ TEXT("list_onrep_handlers"), TEXT("cursor") },
		{ TEXT("audit_unbalanced_onreps"), TEXT("cursor") },
	};

	for (const TPair<FString, FString>& Contract : StringContracts)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetField(Contract.Value, MakeShared<FJsonValueNumber>(123.0));
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("network"), Contract.Key, Params);

		TestFalse(
			FString::Printf(
				TEXT("%s rejects non-string %s"),
				*Contract.Key,
				*Contract.Value),
			Result.bSuccess);
		TestEqual(
			FString::Printf(
				TEXT("%s %s uses invalid-params code"),
				*Contract.Key,
				*Contract.Value),
			Result.ErrorCode,
			-32602);
		TestTrue(
			FString::Printf(
				TEXT("%s %s error names the field"),
				*Contract.Key,
				*Contract.Value),
			Result.ErrorMessage.Contains(Contract.Value));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
