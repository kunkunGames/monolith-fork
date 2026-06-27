#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUISecurityPathTest, "Monolith.Security.UI.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISecurityPathTest::RunTest(const FString& Parameters)
{
	auto RunTestCase = [this](const FString& TestCaseName, const FString& Path, bool bShouldSucceed)
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("save_path"), Path);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("create_widget_blueprint"), Payload);

		if (bShouldSucceed)
		{
			// Note: We only check if it didn't fail DUE TO validation. A valid path might still fail to create a package in a headless test environment.
			TestFalse(FString::Printf(TEXT("%s: Should not fail validation"), *TestCaseName), Result.ErrorMessage.Contains(TEXT("Invalid package path")));
		}
		else
		{
			TestFalse(FString::Printf(TEXT("%s: Action should fail"), *TestCaseName), Result.bSuccess);
			const bool bPathError =
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")) ||
				(Path.IsEmpty() && Result.ErrorMessage.Contains(TEXT("save_path")));
			TestTrue(FString::Printf(TEXT("%s: Error should complain about invalid package path"), *TestCaseName), bPathError);
		}
	};

	RunTestCase(TEXT("Double Slash Path"), TEXT("//Game/MalformedPath/TestWidget"), false);
	RunTestCase(TEXT("Empty Path"), TEXT(""), false);
	RunTestCase(TEXT("Missing Leading Slash"), TEXT("Game/MalformedPath/TestWidget"), false);
	RunTestCase(TEXT("Invalid Characters"), TEXT("/Game/MalformedPath/Test#Widget"), false);
	RunTestCase(TEXT("Trailing Slash"), TEXT("/Game/MalformedPath/TestWidget/"), false);
	RunTestCase(TEXT("Valid Path"), TEXT("/Game/ValidPath/TestWidget"), true);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
