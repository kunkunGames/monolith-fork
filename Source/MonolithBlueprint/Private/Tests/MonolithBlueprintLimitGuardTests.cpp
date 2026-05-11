#include "Misc/AutomationTest.h"
#include "MonolithBlueprintActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FMonolithActionResult ExecuteSearchFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithBlueprintActions::RegisterActions();
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("search_functions"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintSearchFunctionsLimitTest, "Monolith.LimitGuard.Blueprint.SearchFunctionsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBlueprintSearchFunctionsLimitTest::RunTest(const FString& Parameters)
{
	// Test very large limit upper bound
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // Query param to match many native functions (e.g., Get)
        Params->SetStringField(TEXT("query"), TEXT("Get"));
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteSearchFunctions(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("match_count"), CountVal))
		{
			if (CountVal > 1000.0)
			{
				AddError(FString::Printf(TEXT("Huge limit was not clamped to 1000. Count was %f"), CountVal));
			}
		}
	}

	return true;
}

namespace
{
FMonolithActionResult ExecuteBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("blueprint"), TEXT("batch_execute")))
	{
		FMonolithBlueprintActions::RegisterActions();
	}
	return Registry.ExecuteAction(TEXT("blueprint"), TEXT("batch_execute"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintBatchExecuteLimitTest, "Monolith.LimitGuard.Blueprint.BatchExecuteRejectsOversizedArray", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBlueprintBatchExecuteLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent.NonExistent"));

	TArray<TSharedPtr<FJsonValue>> OpsArray;
	for (int32 i = 0; i < 501; ++i)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), TEXT("add_node"));
		OpsArray.Add(MakeShared<FJsonValueObject>(Op));
	}
	Params->SetArrayField(TEXT("operations"), OpsArray);

	FMonolithActionResult Result = ExecuteBatchExecute(Params);

	TestTrue(TEXT("Should fail on oversized array"), !Result.bSuccess);
	TestTrue(TEXT("Error message should mention max 500"), Result.ErrorMessage.Contains(TEXT("max 500")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
