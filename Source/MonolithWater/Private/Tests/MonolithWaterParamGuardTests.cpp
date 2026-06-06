#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithWaterActions.h"

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
