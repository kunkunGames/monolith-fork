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
	TestFalse(TEXT("create_user_defined_struct should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("create_user_defined_struct error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test create_data_asset
	TSharedPtr<FJsonObject> DataAssetPayload = MakeShared<FJsonObject>();
	DataAssetPayload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/DA_TestAsset"));
	DataAssetPayload->SetStringField(TEXT("class_name"), TEXT("PrimaryDataAsset"));

	FMonolithActionResult DataAssetResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("create_data_asset"), DataAssetPayload);
	TestFalse(TEXT("create_data_asset should fail on malformed path"), DataAssetResult.bSuccess);
	TestTrue(TEXT("create_data_asset error should complain about invalid package path"), DataAssetResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test create_blueprint
	TSharedPtr<FJsonObject> BlueprintPayload = MakeShared<FJsonObject>();
	BlueprintPayload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/BP_TestClass"));
	BlueprintPayload->SetStringField(TEXT("parent_class"), TEXT("Actor"));

	FMonolithActionResult BlueprintResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("create_blueprint"), BlueprintPayload);
	TestFalse(TEXT("create_blueprint should fail on malformed path"), BlueprintResult.bSuccess);
	TestTrue(TEXT("create_blueprint error should complain about invalid package path"), BlueprintResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test seed_data_asset
	TSharedPtr<FJsonObject> SeedDataAssetPayload = MakeShared<FJsonObject>();
	SeedDataAssetPayload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/DA_TestSeedAsset"));
	SeedDataAssetPayload->SetStringField(TEXT("class_name"), TEXT("PrimaryDataAsset"));
	SeedDataAssetPayload->SetObjectField(TEXT("tree"), MakeShared<FJsonObject>());

	FMonolithActionResult SeedDataAssetResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("seed_data_asset"), SeedDataAssetPayload);
	TestFalse(TEXT("seed_data_asset should fail on malformed path"), SeedDataAssetResult.bSuccess);
	TestTrue(TEXT("seed_data_asset error should complain about invalid package path"), SeedDataAssetResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintScaffoldLocomotionCrashguardTest, "Monolith.Crashguard.Blueprint.ScaffoldLocomotionInputRejectMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintScaffoldLocomotionCrashguardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("bp_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_Character"));
	Payload->SetStringField(TEXT("imc_path"), TEXT("//Game/MalformedPath/IMC_Input"));

	TArray<TSharedPtr<FJsonValue>> Actions;
	TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
	Action->SetStringField(TEXT("name"), TEXT("Move"));
	Action->SetStringField(TEXT("value_type"), TEXT("Vector2D"));
	Actions.Add(MakeShared<FJsonValueObject>(Action));
	Payload->SetArrayField(TEXT("actions"), Actions);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("scaffold_locomotion_input"), Payload);

	TestFalse(TEXT("scaffold_locomotion_input should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("scaffold_locomotion_input error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
