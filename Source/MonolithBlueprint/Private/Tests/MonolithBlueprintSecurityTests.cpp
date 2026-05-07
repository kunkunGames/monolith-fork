#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintSecurityPathTest, "Monolith.Security.Blueprint.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/S_TestStruct"));

	// We also need a dummy fields array for create_user_defined_struct
	TArray<TSharedPtr<FJsonValue>> Fields;
	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("name"), TEXT("DummyField"));
	Field->SetStringField(TEXT("type"), TEXT("int"));
	Fields.Add(MakeShared<FJsonValueObject>(Field));
	Payload->SetArrayField(TEXT("fields"), Fields);

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("create_user_defined_struct"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("Action should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
