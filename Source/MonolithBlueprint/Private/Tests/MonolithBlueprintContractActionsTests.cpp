#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintContractActionsTests, "Monolith.Blueprint.ContractActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintContractActionsTests::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("blueprint"), TEXT("compare_class_variable_contract"), Params);
		TestFalse(TEXT("compare_class_variable_contract missing left param should error"), Result.bSuccess);
		TestTrue(TEXT("compare_class_variable_contract missing left param should indicate left in error msg"), Result.ErrorMessage.Contains(TEXT("Missing required parameter: left")));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("left"), TEXT("dummy"));
		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("blueprint"), TEXT("compare_class_variable_contract"), Params);
		TestFalse(TEXT("compare_class_variable_contract missing right param should error"), Result.bSuccess);
		TestTrue(TEXT("compare_class_variable_contract missing right param should indicate right in error msg"), Result.ErrorMessage.Contains(TEXT("Missing required parameter: right")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
