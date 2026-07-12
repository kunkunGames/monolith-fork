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

#endif // WITH_DEV_AUTOMATION_TESTS
