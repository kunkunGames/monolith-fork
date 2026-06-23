#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithWaterActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWaterLimitClampTest, "Monolith.Security.MonolithWater.LimitClamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWaterLimitClampTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Valid limit should pass through"), FMonolithWaterActions::ClampWaterLimit(100.0), 100);
	TestEqual(TEXT("Negative limit should clamp to 1"), FMonolithWaterActions::ClampWaterLimit(-5.0), 1);
	TestEqual(TEXT("Zero limit should clamp to 1"), FMonolithWaterActions::ClampWaterLimit(0.0), 1);
	TestEqual(TEXT("Over 500 limit should clamp to 500"), FMonolithWaterActions::ClampWaterLimit(1000.0), 500);
	TestEqual(TEXT("Exact 500 limit should pass through"), FMonolithWaterActions::ClampWaterLimit(500.0), 500);
	TestEqual(TEXT("Fractional limit should floor and clamp"), FMonolithWaterActions::ClampWaterLimit(50.9), 50);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWaterListBodiesParamGuardTest, "Monolith.Registry.MonolithWater.ParamGuardRejectsInvalidTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWaterListBodiesParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithWaterActions::RegisterActions(Registry);

	// Test 1: Invalid limit type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("water"), TEXT("list_bodies"), Params);
		TestFalse(TEXT("Should fail with invalid limit type"), Result.bSuccess);
		TestTrue(TEXT("Error should mention limit"), Result.Error.Contains(TEXT("limit must be a number")));
	}

	// Test 2: Invalid actor_name_filter type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("actor_name_filter"), true);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("water"), TEXT("list_bodies"), Params);
		TestFalse(TEXT("Should fail with invalid actor_name_filter type"), Result.bSuccess);
		TestTrue(TEXT("Error should mention actor_name_filter"), Result.Error.Contains(TEXT("actor_name_filter must be a string")));
	}

	return true;
}
