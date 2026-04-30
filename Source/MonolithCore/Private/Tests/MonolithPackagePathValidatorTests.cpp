#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithPackagePathValidator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPackagePathValidatorTest,
	"Monolith.Core.PackagePathValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPackagePathValidatorTest::RunTest(const FString& Parameters)
{
	using namespace MonolithCore;

	// Happy path
	TestTrue(TEXT("Valid package path returns empty string"), ValidatePackagePath(TEXT("/Game/Test")).IsEmpty());

	// Empty string
	TestEqual(TEXT("Empty string returns specific error"), ValidatePackagePath(TEXT("")), TEXT("Package path is empty"));

	// Double leading slash (the crash site reported in the header)
	TestTrue(TEXT("Double leading slash returns error"), !ValidatePackagePath(TEXT("//Game/Test")).IsEmpty());
	TestTrue(TEXT("Double leading slash error contains context"), ValidatePackagePath(TEXT("//Game/Test")).Contains(TEXT("//Game/Test")));

	// Missing leading slash
	TestTrue(TEXT("Missing leading slash returns error"), !ValidatePackagePath(TEXT("Game/Test")).IsEmpty());

	// Invalid characters
	TestTrue(TEXT("Invalid characters return error"), !ValidatePackagePath(TEXT("/Game/Test#Invalid")).IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
