#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationSecurityPathTest, "Monolith.Security.Animation.ValidatePackagePath", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("//Game/MalformedPath/TestBlendSpace"));
	// skeleton_path is required by create_blend_space
	Payload->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("create_blend_space"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("Action should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
