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

	// Basic valid markers per platform
#if PLATFORM_MAC
	const FString ExpectedPrefix = TEXT("Monolith-macOS-SHA256: ");
#elif PLATFORM_LINUX
	const FString ExpectedPrefix = TEXT("Monolith-Linux-SHA256: ");
#else
	// v2 generation (Issues #90/#94) — the pre-v2 "Monolith-SHA256:" name is retired.
	const FString ExpectedPrefix = TEXT("Monolith-SHA256-v2: ");
#endif

	TestEqual(TEXT("Parses valid hash"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(ExpectedPrefix + ValidHash), ValidHash);
	TestEqual(TEXT("Parses valid hash with surrounding text"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(TEXT("Some notes\n") + ExpectedPrefix + ValidHash + TEXT("\nMore notes")), ValidHash);
	TestEqual(TEXT("Parses case-insensitive hash and returns lower"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(ExpectedPrefix + ValidHash.ToUpper()), ValidHash.ToLower());

	// Missing/Invalid
	TestEqual(TEXT("Returns empty for missing marker"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(TEXT("Some random release notes")), TEXT(""));

	// Too short
	const FString ShortHash = ValidHash.Left(63);
	TestEqual(TEXT("Rejects short hash"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(ExpectedPrefix + ShortHash), TEXT(""));

	// Too long (tests the negative lookahead boundary)
	const FString LongHash = ValidHash + TEXT("a");
	TestEqual(TEXT("Rejects overly long hash"), UMonolithUpdateSubsystem::ParseSha256FromReleaseNotes(ExpectedPrefix + LongHash), TEXT(""));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
