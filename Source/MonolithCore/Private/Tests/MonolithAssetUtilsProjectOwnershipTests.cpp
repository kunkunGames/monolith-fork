#include "MonolithAssetUtils.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetUtilsProjectOwnershipTest,
	"Monolith.Core.AssetUtils.ProjectOwnedPackageMounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetUtilsProjectOwnershipTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Project content mount is project-owned"),
		FMonolithAssetUtils::IsProjectOwnedPackage(TEXT("/Game/MonolithOwnershipTest")));
	TestTrue(TEXT("SpeedCore GameFeature mount is project-owned"),
		FMonolithAssetUtils::IsProjectOwnedPackage(TEXT("/SpeedCore/MonolithOwnershipTest")));
	TestTrue(TEXT("SpeedBox GameFeature mount is project-owned"),
		FMonolithAssetUtils::IsProjectOwnedPackage(TEXT("/SpeedBox/MonolithOwnershipTest")));
	TestFalse(TEXT("Engine content mount is not project-owned"),
		FMonolithAssetUtils::IsProjectOwnedPackage(TEXT("/Engine/MonolithOwnershipTest")));
	TestFalse(TEXT("Transient packages are not mounted project packages"),
		FMonolithAssetUtils::IsProjectOwnedPackage(TEXT("/Temp/MonolithOwnershipTest")));
	return true;
}

#endif
