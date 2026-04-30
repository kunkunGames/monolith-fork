#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetUtilsResolvePathTest,
	"Monolith.Core.AssetUtils.ResolveAssetPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetUtilsResolvePathTest::RunTest(const FString& Parameters)
{
	// 1. Basic path
	TestEqual(TEXT("Basic /Game/ path remains unchanged"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("/Game/MyFolder/MyAsset")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 2. Trimming
	TestEqual(TEXT("Whitespace is trimmed"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("  /Game/MyFolder/MyAsset  \t")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 3. Backslash normalization
	TestEqual(TEXT("Backslashes are normalized to forward slashes"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("\\Game\\MyFolder\\MyAsset")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 4. /Content/ prefix
	TestEqual(TEXT("/Content/ prefix is converted to /Game/"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("/Content/MyFolder/MyAsset")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 5. Relative path
	TestEqual(TEXT("Relative paths assume /Game/ prefix"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("MyFolder/MyAsset")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 6. .uasset extension stripping
	TestEqual(TEXT(".uasset extension is stripped"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("/Game/MyFolder/MyAsset.uasset")),
		TEXT("/Game/MyFolder/MyAsset"));

	// 7. .umap extension stripping
	TestEqual(TEXT(".umap extension is stripped"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("/Game/MyFolder/MyMap.umap")),
		TEXT("/Game/MyFolder/MyMap"));

	// 8. Combination of features
	TestEqual(TEXT("Combination of whitespace, backslashes, /Content/, and .uasset"),
		FMonolithAssetUtils::ResolveAssetPath(TEXT("  \\Content\\MyFolder\\MyAsset.uasset  ")),
		TEXT("/Game/MyFolder/MyAsset"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
