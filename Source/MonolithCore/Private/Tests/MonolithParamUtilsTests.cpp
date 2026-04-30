#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithParamUtils.h"
#include "Engine/EngineTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseMobilityTest,
	"Monolith.ParamUtils.ParseMobility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseMobilityTest::RunTest(const FString& Parameters)
{
	struct FTestCase
	{
		FString Input;
		bool bExpectedResult;
		EComponentMobility::Type ExpectedMobility;
	};

	const TArray<FTestCase> TestCases = {
		// Exact matches
		{ TEXT("static"), true, EComponentMobility::Static },
		{ TEXT("stationary"), true, EComponentMobility::Stationary },
		{ TEXT("movable"), true, EComponentMobility::Movable },

		// Case insensitivity
		{ TEXT("STATIC"), true, EComponentMobility::Static },
		{ TEXT("Stationary"), true, EComponentMobility::Stationary },
		{ TEXT("mOvAbLe"), true, EComponentMobility::Movable },

		// Invalid/Edge cases
		{ TEXT(""), false, EComponentMobility::Static },
		{ TEXT("invalid"), false, EComponentMobility::Static },
		{ TEXT("static "), false, EComponentMobility::Static }, // Trailing space
		{ TEXT(" static"), false, EComponentMobility::Static }, // Leading space
	};

	for (const FTestCase& TestCase : TestCases)
	{
		EComponentMobility::Type ActualMobility = EComponentMobility::Static; // Initialize to default

		const bool bActualResult = MonolithParamUtils::ParseMobility(TestCase.Input, ActualMobility);

		TestEqual(FString::Printf(TEXT("ParseMobility('%s') returns expected boolean"), *TestCase.Input), bActualResult, TestCase.bExpectedResult);

		if (TestCase.bExpectedResult)
		{
			TestEqual(FString::Printf(TEXT("ParseMobility('%s') sets correct mobility"), *TestCase.Input), ActualMobility, TestCase.ExpectedMobility);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamUtilsNormalizePathTest,
	"Monolith.ParamUtils.NormalizeBlueprintClassPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamUtilsNormalizePathTest::RunTest(const FString& Parameters)
{
	// Test case: no dot
	FString NoDot = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar"));
	TestEqual(TEXT("No dot adds .BaseName_C"), NoDot, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	// Test case: has dot, no _C
	FString HasDotNoC = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar.BP_Bar"));
	TestEqual(TEXT("Has dot but no _C appends _C"), HasDotNoC, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	// Test case: has dot, has _C
	FString HasDotHasC = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));
	TestEqual(TEXT("Has dot and _C does not change"), HasDotHasC, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
