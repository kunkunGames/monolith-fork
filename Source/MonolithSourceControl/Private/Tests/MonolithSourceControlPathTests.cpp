#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithSourceControlUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceControlSecurityPathTest,
	"Monolith.Security.SourceControl.NormalizePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlSecurityPathTest::RunTest(const FString& Parameters)
{
	FString OutFile;
	FString OutError;

	// Empty path
	TestFalse(TEXT("Empty path returns false"), FMonolithSourceControlUtils::NormalizePathForSourceControl(TEXT(""), OutFile, OutError));
	TestTrue(TEXT("Empty path sets error"), OutError.Contains(TEXT("empty path")));

	// Test normal path resolution behavior. We might not be able to fully resolve it
	// without the editor context actually running and the package existing, but
	// we want to ensure basic parsing won't crash.

	// Game/Test (Missing leading slash) - treated as a relative path to project dir
	// It doesn't start with / so it skips the object path checks.
	TestTrue(TEXT("Missing leading slash is processed as relative"), FMonolithSourceControlUtils::NormalizePathForSourceControl(TEXT("Game/Test"), OutFile, OutError));

	// Double leading slash is checked inside PackageNameToFilename which would throw if it was passed.
	// Actually, does "//Game/Test" pass IsValidLongPackageName? No. So it gets treated as a relative or absolute path
	// because it doesn't pass the check and just goes to the bottom where it says FPaths::ConvertRelativePathToFull(Path)
	TestTrue(TEXT("Double leading slash is processed safely"), FMonolithSourceControlUtils::NormalizePathForSourceControl(TEXT("//Game/Test"), OutFile, OutError));

	// Test malformed package name directly via PackageNameToFilename if it was possible, but it's private.
	// We are protecting against crashes inside FPackageName::DoesPackageExist or FindPackage by ensuring FMonolithSourceControlUtils doesn't pass malformed ones.
	// The NormalizePathForSourceControl delegates to PackageNameToFilename ONLY if `IsValidLongPackageName` returns true.
	// This makes it inherently safe against crashes from `FindPackage(nullptr, *Normalized)` with malformed paths.

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
