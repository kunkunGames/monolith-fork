#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityPathTest, "Monolith.Security.Material.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("//Game/MalformedPath/TestMaterial"));

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("Action should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialPreviewTexturesLimitTest, "Monolith.LimitGuard.Material.PreviewTexturesLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialPreviewTexturesLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (int32 i = 0; i < 101; ++i)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("/Game/Textures/Tex_%d"), i)));
	}
	Payload->SetArrayField(TEXT("asset_paths"), PathsArray);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("preview_textures"), Payload);

	TestFalse(TEXT("Action should fail on oversized input array"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
