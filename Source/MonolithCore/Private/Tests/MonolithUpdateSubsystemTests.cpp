#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithUpdateSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseVersionFromTagTest,
	"Monolith.Core.Update.ParseVersionFromTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseVersionFromTagTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Parses v-prefix"), UMonolithUpdateSubsystem::ParseVersionFromTag(TEXT("v1.2.0")), TEXT("1.2.0"));
	TestEqual(TEXT("Parses V-prefix"), UMonolithUpdateSubsystem::ParseVersionFromTag(TEXT("V2.0.0")), TEXT("2.0.0"));
	TestEqual(TEXT("Trims whitespace"), UMonolithUpdateSubsystem::ParseVersionFromTag(TEXT("  1.2.3  ")), TEXT("1.2.3"));
	TestEqual(TEXT("Parses plain version"), UMonolithUpdateSubsystem::ParseVersionFromTag(TEXT("1.0.0")), TEXT("1.0.0"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCompareVersionsTest,
	"Monolith.Core.Update.CompareVersions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCompareVersionsTest::RunTest(const FString& Parameters)
{
	// Remote is newer (> 0)
	TestTrue(TEXT("Patch newer"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.0.0"), TEXT("1.0.1")) > 0);
	TestTrue(TEXT("Minor newer"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.0.0"), TEXT("1.1.0")) > 0);
	TestTrue(TEXT("Major newer"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.0.0"), TEXT("2.0.0")) > 0);

	// Equal (== 0)
	TestTrue(TEXT("Versions equal"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.2.3"), TEXT("1.2.3")) == 0);

	// Current is newer (< 0)
	TestTrue(TEXT("Patch older"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.0.1"), TEXT("1.0.0")) < 0);
	TestTrue(TEXT("Minor older"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.1.0"), TEXT("1.0.0")) < 0);
	TestTrue(TEXT("Major older"), UMonolithUpdateSubsystem::CompareVersions(TEXT("2.0.0"), TEXT("1.0.0")) < 0);

	// Missing/Malformed
	TestTrue(TEXT("Remote malformed against valid current"), UMonolithUpdateSubsystem::CompareVersions(TEXT("1.0.0"), TEXT("")) < 0);
	TestTrue(TEXT("Current malformed against valid remote"), UMonolithUpdateSubsystem::CompareVersions(TEXT(""), TEXT("1.0.0")) > 0);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseSha256Test,
	"Monolith.Core.Update.ParseSha256FromReleaseNotes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseSha256Test::RunTest(const FString& Parameters)
{
	const FString ValidHash = TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	const FString WindowsMarker = UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT(""), TEXT("Windows"));
	const FString MacMarker = UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT(""), TEXT("macOS"));
	const FString LinuxMarker = UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT(""), TEXT("Linux"));

	TestEqual(TEXT("Builds Windows v2 marker"), WindowsMarker, TEXT("Monolith-SHA256-v2"));
	TestEqual(TEXT("Builds Win64 alias"),
		UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT("UE5.8"), TEXT("Win64")),
		TEXT("Monolith-SHA256-v2-UE5.8"));
	TestEqual(TEXT("Builds macOS engine marker"),
		UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT("UE5.8"), TEXT("macOS")),
		TEXT("Monolith-macOS-SHA256-v2-UE5.8"));
	TestEqual(TEXT("Builds Linux engine marker"),
		UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT("UE5.8"), TEXT("Linux")),
		TEXT("Monolith-Linux-SHA256-v2-UE5.8"));
	TestEqual(TEXT("Rejects invalid engine tag"),
		UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT("UE5.8-extra"), TEXT("Windows")),
		TEXT(""));
	TestEqual(TEXT("Rejects unknown platform"),
		UMonolithUpdateSubsystem::BuildSha256MarkerName(TEXT("UE5.8"), TEXT("Solaris")),
		TEXT(""));

#if PLATFORM_MAC
	const FString ExpectedCurrentMarker = TEXT("Monolith-macOS-SHA256-v2");
#elif PLATFORM_LINUX
	const FString ExpectedCurrentMarker = TEXT("Monolith-Linux-SHA256-v2");
#elif PLATFORM_WINDOWS
	const FString ExpectedCurrentMarker = TEXT("Monolith-SHA256-v2");
#else
	// v2 generation (Issues #90/#94) — the pre-v2 "Monolith-SHA256:" name is retired.
	const FString ExpectedPrefix = TEXT("Monolith-SHA256-v2: ");
#endif

	const FString WindowsPrefix = WindowsMarker + TEXT(": ");
	TestEqual(TEXT("Parses valid Windows hash"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(WindowsPrefix + ValidHash, TEXT(""), TEXT("Windows")),
		ValidHash);
	TestEqual(TEXT("Parses the exact canonical macOS workflow marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Monolith-macOS-SHA256-v2: ") + ValidHash, TEXT(""), TEXT("macOS")),
		ValidHash);
	TestEqual(TEXT("Parses valid hash with surrounding text"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Some notes\n") + WindowsPrefix + ValidHash + TEXT("\nMore notes"), TEXT(""), TEXT("Windows")),
		ValidHash);
	TestEqual(TEXT("Parses whitespace and returns lowercase hash"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			WindowsMarker + TEXT(":\t\n") + ValidHash.ToUpper(), TEXT(""), TEXT("Windows")),
		ValidHash);
	TestEqual(TEXT("Parses exact engine marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Monolith-SHA256-v2-UE5.8: ") + ValidHash, TEXT("UE5.8"), TEXT("Windows")),
		ValidHash);

	// Missing/Invalid
	TestEqual(TEXT("Returns empty for missing marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Some random release notes"), TEXT(""), TEXT("Windows")),
		TEXT(""));
	TestEqual(TEXT("Rejects pre-v2 marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Monolith-SHA256: ") + ValidHash, TEXT(""), TEXT("Windows")),
		TEXT(""));
	TestEqual(TEXT("Rejects another platform marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			MacMarker + TEXT(": ") + ValidHash, TEXT(""), TEXT("Windows")),
		TEXT(""));
	TestEqual(TEXT("Rejects another engine marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			TEXT("Monolith-SHA256-v2-UE5.7: ") + ValidHash, TEXT("UE5.8"), TEXT("Windows")),
		TEXT(""));

	// Too short
	const FString ShortHash = ValidHash.Left(63);
	TestEqual(TEXT("Rejects short hash"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			WindowsPrefix + ShortHash, TEXT(""), TEXT("Windows")),
		TEXT(""));

	// Too long tests the exact hex boundary.
	const FString LongHash = ValidHash + TEXT("a");
	TestEqual(TEXT("Rejects overly long hash"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			WindowsPrefix + LongHash, TEXT(""), TEXT("Windows")),
		TEXT(""));

	TestEqual(TEXT("Malformed marker does not hide later valid marker"),
		UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(
			WindowsPrefix + ShortHash + TEXT("\n") + WindowsPrefix + ValidHash,
			TEXT(""), TEXT("Windows")),
		ValidHash);

	// These values are referenced above so all platform marker literals stay
	// covered even when the test binary was compiled for only one platform.
	TestEqual(TEXT("macOS marker remains distinct"), MacMarker, TEXT("Monolith-macOS-SHA256-v2"));
	TestEqual(TEXT("Linux marker remains distinct"), LinuxMarker, TEXT("Monolith-Linux-SHA256-v2"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
