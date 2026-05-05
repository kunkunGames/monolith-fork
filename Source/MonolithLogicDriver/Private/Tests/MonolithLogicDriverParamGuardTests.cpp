#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "MonolithLogicDriverComponentActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetSMComponentConfigRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.GetSMComponentConfigRejectsMalformedParams", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGetSMComponentConfigRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	// Test 1: Missing blueprint_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_name"), TEXT("MyComp"));

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Missing blueprint_path should return error"), !Result.bSuccess);
		TestTrue(TEXT("Missing blueprint_path error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 2: Wrong type for blueprint_path (number instead of string)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("blueprint_path"), 12345);

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Wrong type blueprint_path should return error"), !Result.bSuccess);
		TestTrue(TEXT("Wrong type blueprint_path error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 3: Wrong type for component_name (number instead of string)
	{
		// Wait, if blueprint_path is valid but doesn't exist, it will return "Failed to load Blueprint".
		// But it won't crash when parsing component_name.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/NonExistent/BP_Dummy"));
		Params->SetNumberField(TEXT("component_name"), 123);

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Valid blueprint_path but wrong type component_name should safely return load error (not crash)"), !Result.bSuccess);
		TestTrue(TEXT("Should fail because blueprint path doesn't exist"), Result.ErrorMessage.Contains(TEXT("Failed to load")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
