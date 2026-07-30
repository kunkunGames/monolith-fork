// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "Misc/AutomationTest.h"
#include "MonolithAssetInspectionTestHooks.h"

/**
 * MonolithAsset.InspectAsset.MountedSoftReferenceExistence
 *
 * Missing references under plugin and Engine mounts must be reported as
 * unresolved. Existing Engine assets and /Script class references remain
 * valid without treating every non-/Game path as present.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetInspectMountedSoftReferenceExistenceTest,
	"MonolithAsset.InspectAsset.MountedSoftReferenceExistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetInspectMountedSoftReferenceExistenceTest::RunTest(const FString& Parameters)
{
	using MonolithAsset::Tests::DoesSoftReferenceExistForTest;

	TestFalse(
		TEXT("invalid soft path does not exist"),
		DoesSoftReferenceExistForTest(FSoftObjectPath()));
	TestFalse(
		TEXT("missing plugin-mounted asset does not exist"),
		DoesSoftReferenceExistForTest(
			FSoftObjectPath(TEXT("/MonolithMissingPlugin/Tests/T_Missing.T_Missing"))));
	TestFalse(
		TEXT("missing Engine asset does not exist"),
		DoesSoftReferenceExistForTest(
			FSoftObjectPath(TEXT("/Engine/MonolithTests/T_Missing.T_Missing"))));
	TestTrue(
		TEXT("existing Engine asset resolves"),
		DoesSoftReferenceExistForTest(
			FSoftObjectPath(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"))));
	TestTrue(
		TEXT("/Script class reference is an intentional non-asset path"),
		DoesSoftReferenceExistForTest(
			FSoftObjectPath(TEXT("/Script/Engine.Texture2D"))));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
