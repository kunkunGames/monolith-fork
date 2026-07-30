#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithAINavigationPackageUtils.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAINavigationSaveExtensionTest,
	"Monolith.AI.Navigation.PackageSaveExtension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAINavigationSaveExtensionTest::RunTest(const FString& Parameters)
{
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* AssetPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/MonolithAINavigationSaveExtension_%s_Asset"),
		*UniqueSuffix));
	UPackage* MapPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/MonolithAINavigationSaveExtension_%s_Map"),
		*UniqueSuffix));

	if (!TestNotNull(TEXT("Asset test package"), AssetPackage)
		|| !TestNotNull(TEXT("Map test package"), MapPackage))
	{
		return false;
	}

	MapPackage->SetPackageFlags(PKG_ContainsMap);

	const FString AssetFilename = MonolithAINavigationPackages::ResolveSaveFilename(AssetPackage);
	const FString MapFilename = MonolithAINavigationPackages::ResolveSaveFilename(MapPackage);

	TestTrue(
		TEXT("Ordinary navigation package uses the canonical asset extension"),
		AssetFilename.EndsWith(FPackageName::GetAssetPackageExtension(), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("World navigation package preserves the canonical map extension"),
		MapFilename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::CaseSensitive));
	TestFalse(
		TEXT("World navigation package is never resolved as an asset file"),
		MapFilename.EndsWith(FPackageName::GetAssetPackageExtension(), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("Null package fails closed without a filename"),
		MonolithAINavigationPackages::ResolveSaveFilename(nullptr).IsEmpty());

	AssetPackage->MarkAsGarbage();
	MapPackage->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
