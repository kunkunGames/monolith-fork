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

#endif // WITH_DEV_AUTOMATION_TESTS
