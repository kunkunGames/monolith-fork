#if WITH_DEV_AUTOMATION_TESTS && WITH_METASOUND

#include "Misc/AutomationTest.h"
#include "MonolithAudioMetaSoundAssetResolver.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAudioMetaSoundPackagePathResolutionTest,
	"Monolith.Audio.MetaSound.AssetResolver.CanonicalizesPackageAndObjectPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioMetaSoundPackagePathResolutionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAudio::MetaSoundAssetResolver;

	FResolvedAssetPath PackageResolved;
	FString Error;
	TestTrue(
		TEXT("Package-only path resolves"),
		ResolveAssetPath(
			TEXT("/SpeedBox/TagChase/Audio/sfx_Runner_Drop_meta"),
			PackageResolved,
			Error));
	TestEqual(
		TEXT("Package path remains exact"),
		PackageResolved.PackagePath,
		FString(TEXT("/SpeedBox/TagChase/Audio/sfx_Runner_Drop_meta")));
	TestEqual(
		TEXT("Package-only path gains the canonical object suffix"),
		PackageResolved.ObjectPath,
		FString(TEXT("/SpeedBox/TagChase/Audio/sfx_Runner_Drop_meta.sfx_Runner_Drop_meta")));

	FResolvedAssetPath ObjectResolved;
	TestTrue(
		TEXT("Canonical object path resolves"),
		ResolveAssetPath(PackageResolved.ObjectPath, ObjectResolved, Error));
	TestEqual(
		TEXT("Canonical object path is idempotent"),
		ObjectResolved.ObjectPath,
		PackageResolved.ObjectPath);

	FResolvedAssetPath ExportTextResolved;
	TestTrue(
		TEXT("Export-text object path resolves"),
		ResolveAssetPath(
			TEXT("MetaSoundSource'/SpeedBox/TagChase/Audio/sfx_Runner_Drop_meta.sfx_Runner_Drop_meta'"),
			ExportTextResolved,
			Error));
	TestEqual(
		TEXT("Export-text syntax normalizes to the same object path"),
		ExportTextResolved.ObjectPath,
		PackageResolved.ObjectPath);

	FResolvedAssetPath InvalidResolved;
	TestFalse(
		TEXT("Relative paths fail closed"),
		ResolveAssetPath(TEXT("SpeedBox/TagChase/Audio/Drop"), InvalidResolved, Error));
	TestTrue(TEXT("Invalid path reports an actionable error"), Error.Contains(TEXT("expected")));
	return true;
}

#endif
