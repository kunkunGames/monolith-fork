#include "Misc/AutomationTest.h"
#include "MonolithMeshAdvancedLevelActions.h"
#include "Dom/JsonObject.h"
#include "MonolithPackagePathValidator.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshCrashguardValidatePackagePathTest, "Monolith.Crashguard.MonolithMesh.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshCrashguardValidatePackagePathTest::RunTest(const FString& Parameters)
{
	// Test malformed paths that would otherwise crash CreatePackage()
	TArray<FString> MalformedPaths = {
		TEXT(""),
		TEXT("MalformedNoSlash"),
		TEXT("Game/MissingLeadingSlash"),
		TEXT("//Game/DoubleSlash"),
		TEXT("/Game/TrailingSlash/"),
		TEXT("/Game/IllegalChars*?")
	};

	for (const FString& BadPath : MalformedPaths)
	{
		// Test the core validator explicitly
		const FString ErrorMsg = MonolithCore::ValidatePackagePath(BadPath);
		TestFalse(FString::Printf(TEXT("Path '%s' should be rejected"), *BadPath), ErrorMsg.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
