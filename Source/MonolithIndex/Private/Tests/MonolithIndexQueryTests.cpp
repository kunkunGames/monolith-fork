#include "Misc/AutomationTest.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectFindByTypeClampsLimitTest, "Monolith.IndexGuard.Project.FindByTypeClampsLimit", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FProjectFindByTypeClampsLimitTest::RunTest(const FString& Parameters)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Params->SetNumberField(TEXT("limit"), 50000); // Exceeds clamp
	Params->SetNumberField(TEXT("offset"), -10);   // Below 0

	FMonolithActionResult Result = FProjectFindByTypeAction::Execute(Params);

	if (Result.bSuccess && Result.ResultObject.IsValid())
	{
		int32 RetLimit = Result.ResultObject->GetIntegerField(TEXT("limit"));
		int32 RetOffset = Result.ResultObject->GetIntegerField(TEXT("offset"));

		TestEqual(TEXT("Limit is clamped to 1000"), RetLimit, 1000);
		TestEqual(TEXT("Offset is clamped to 0"), RetOffset, 0);
	}

	return true;
}
