#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScaffoldCustomAbilityTaskRejectsMalformedArraysTest, "Monolith.ParamGuard.GAS.ScaffoldCustomAbilityTaskRejectsMalformedArrays", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScaffoldCustomAbilityTaskRejectsMalformedArraysTest::RunTest(const FString& Parameters)
{
	auto ExecuteScaffoldCustomAbilityTask = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("scaffold_custom_ability_task"), Params);
	};

	// Test 1: parameters elements with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		ParamsArray.Add(MakeShared<FJsonValueNumber>(123)); // Not an object

		TArray<TSharedPtr<FJsonValue>> DelegatesArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("parameters"), ParamsArray);
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed parameters array item should not succeed"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention parameters array item type"), Result.ErrorMessage.Contains(TEXT("parameters")) && Result.ErrorMessage.Contains(TEXT("object")));
	}

	// Test 2: delegates elements with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> DelegatesArray;
		DelegatesArray.Add(MakeShared<FJsonValueString>(TEXT("MyDelegate"))); // Not an object

		TArray<TSharedPtr<FJsonValue>> ParamsArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);
		Params->SetArrayField(TEXT("parameters"), ParamsArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed delegates array item should not succeed"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention delegates array item type"), Result.ErrorMessage.Contains(TEXT("delegates")) && Result.ErrorMessage.Contains(TEXT("object")));
	}

	// Test 3: missing nested string fields in parameters
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetNumberField(TEXT("name"), 123); // not a string
		ParamObj->SetNumberField(TEXT("type"), 456);

		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));

		TArray<TSharedPtr<FJsonValue>> DelegatesArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("parameters"), ParamsArray);
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed parameters item string fields should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention parameters item string fields missing or empty"), Result.ErrorMessage.Contains(TEXT("parameters")) && Result.ErrorMessage.Contains(TEXT("name is missing, empty, or not a string")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
