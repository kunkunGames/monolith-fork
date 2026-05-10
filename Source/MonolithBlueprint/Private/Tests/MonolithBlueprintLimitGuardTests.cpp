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

#endif // WITH_DEV_AUTOMATION_TESTS
