#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMaterialMoveOperationsTests, "Monolith.ParamGuard.Material.MoveExpression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMaterialMoveOperationsTests::RunTest(const FString& Parameters)
{
	// Test 1: Single move with wrong type for pos_x
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetStringField(TEXT("expression_name"), TEXT("MyExpr"));
		Payload->SetBoolField(TEXT("pos_x"), true); // Should be number

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail on wrong type for pos_x"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention pos_x"), Result.ErrorMessage.Contains(TEXT("pos_x")));
	}

	// Test 2: Expressions array missing name
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));

		TArray<TSharedPtr<FJsonValue>> ExprArray;
		TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
		ExprObj->SetNumberField(TEXT("x"), 100);
		ExprArray.Add(MakeShared<FJsonValueObject>(ExprObj));

		Payload->SetArrayField(TEXT("expressions"), ExprArray);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail when expression misses name"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention name"), Result.ErrorMessage.Contains(TEXT("name")));
	}

	// Test 3: Expressions array with wrong type for x
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));

		TArray<TSharedPtr<FJsonValue>> ExprArray;
		TSharedPtr<FJsonObject> ExprObj = MakeShared<FJsonObject>();
		ExprObj->SetStringField(TEXT("name"), TEXT("MyExpr"));
		ExprObj->SetStringField(TEXT("x"), TEXT("100")); // Should be number
		ExprArray.Add(MakeShared<FJsonValueObject>(ExprObj));

		Payload->SetArrayField(TEXT("expressions"), ExprArray);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail when expression x is wrong type"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention x"), Result.ErrorMessage.Contains(TEXT("x")));
	}

	// Test 4: Single move with wrong type for expression_name
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotMatter"));
		Payload->SetNumberField(TEXT("expression_name"), 123); // Should be string
		Payload->SetNumberField(TEXT("pos_x"), 100);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("move_expression"), Payload);

		TestFalse(TEXT("Should fail on wrong type for expression_name"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention expression_name"), Result.ErrorMessage.Contains(TEXT("expression_name")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
