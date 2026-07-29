// SPDX-License-Identifier: MIT

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithFuzzyMatch.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyNormalizeTextTest,
	"Monolith.Core.FuzzyMatch.NormalizeText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyNormalizeTextTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Lowercase punctuation and collapse whitespace"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("Hello,  World!")),
		FString(TEXT("hello world")));
	TestEqual(
		TEXT("C++ folds to cpp"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("C++ Code")),
		FString(TEXT("cpp code")));
	TestEqual(
		TEXT("Empty input remains empty"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("")),
		FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyTokenizeTest,
	"Monolith.Core.FuzzyMatch.Tokenize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTokenizeTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Basic = FMonolithFuzzyMatch::Tokenize(TEXT("the graph graph node"));
	TestEqual(TEXT("Stopwords and duplicates are removed"), Basic.Num(), 2);
	TestTrue(TEXT("Graph retained"), Basic.Contains(TEXT("graph")));
	TestTrue(TEXT("Node retained"), Basic.Contains(TEXT("node")));

	const TMap<FString, TArray<FString>> Aliases = {
		{ TEXT("bp"), { TEXT("blueprint") } },
		{ TEXT("vfx"), { TEXT("niagara") } }
	};
	const TArray<FString> Expanded =
		FMonolithFuzzyMatch::Tokenize(TEXT("bp vfx graph"), &Aliases);
	TestTrue(TEXT("Base token retained"), Expanded.Contains(TEXT("bp")));
	TestTrue(TEXT("Blueprint alias expanded"), Expanded.Contains(TEXT("blueprint")));
	TestTrue(TEXT("Niagara alias expanded"), Expanded.Contains(TEXT("niagara")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyEditDistanceTest,
	"Monolith.Core.FuzzyMatch.EditDistanceBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyEditDistanceTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Classic kitten/sitting distance"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("kitten"), TEXT("sitting"), 10),
		3);
	TestEqual(
		TEXT("Case-sensitive distance"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("ABC"), TEXT("abc"), 5, false),
		3);
	TestEqual(
		TEXT("Case-insensitive distance"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("ABC"), TEXT("abc"), 5, true),
		0);
	TestEqual(
		TEXT("Bounded early-out returns MaxDistance plus one"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("abcdef"), TEXT("uvwxyz"), 2),
		3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyTypoMatchTest,
	"Monolith.Core.FuzzyMatch.IsTypoMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTypoMatchTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Long token accepts a small typo"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("blueprint"), TEXT("blueprnt")));
	TestFalse(
		TEXT("Short tokens never typo-match"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("cat"), TEXT("cot")));
	TestFalse(
		TEXT("Different first characters are rejected"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("abcd"), TEXT("xbcd")));
	TestTrue(
		TEXT("Four-character distance one is accepted"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("abcd"), TEXT("abce")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyScoreTokensTest,
	"Monolith.Core.FuzzyMatch.ScoreTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyScoreTokensTest::RunTest(const FString& Parameters)
{
	const FMonolithFuzzyWeights Weights{ 10, 5, 2, 3 };
	const TArray<FString> FieldTokens = { TEXT("graph"), TEXT("node") };

	TArray<FString> Reasons;
	TSet<FString> Matched;
	const int32 ExactScore = FMonolithFuzzyMatch::ScoreTokens(
		{ TEXT("graph") },
		FieldTokens,
		TEXT("graph node"),
		Weights,
		TEXT("field"),
		Reasons,
		Matched);
	TestEqual(TEXT("Exact token uses exact weight"), ExactScore, 10);
	TestTrue(TEXT("Reason recorded"), Reasons.Contains(TEXT("field")));
	TestTrue(TEXT("Matched token recorded"), Matched.Contains(TEXT("graph")));

	Reasons.Reset();
	Matched.Reset();
	int32 BestDistance = MAX_int32;
	const int32 FuzzyScore = FMonolithFuzzyMatch::ScoreTokens(
		{ TEXT("blueprint") },
		{ TEXT("blueprnt") },
		TEXT("blueprnt"),
		Weights,
		TEXT("field"),
		Reasons,
		Matched,
		&BestDistance);
	TestEqual(TEXT("Typo uses fuzzy weight"), FuzzyScore, 3);
	TestTrue(TEXT("Fuzzy reason recorded"), Reasons.Contains(TEXT("field_fuzzy")));
	TestEqual(TEXT("Best contributing distance recorded"), BestDistance, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyScoreCandidateTest,
	"Monolith.Core.FuzzyMatch.ScoreCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyScoreCandidateTest::RunTest(const FString& Parameters)
{
	FMonolithFuzzyField NameField;
	NameField.Text = TEXT("bb punchbot");
	NameField.Tokens = { TEXT("bb"), TEXT("punchbot") };
	NameField.Weights = FMonolithFuzzyWeights{ 45, 30, 16, 8 };
	NameField.ContainsPhraseBonus = 90;
	NameField.ReasonTag = TEXT("asset_name");

	FMonolithFuzzyField PathField;
	PathField.Text = TEXT("game ai punchbot");
	PathField.Tokens = { TEXT("game"), TEXT("ai"), TEXT("punchbot") };
	PathField.Weights = FMonolithFuzzyWeights{ 22, 14, 8, 4 };
	PathField.ReasonTag = TEXT("path");

	const TArray<FMonolithFuzzyField> Fields = { NameField, PathField };
	const FMonolithFuzzyScore Score = FMonolithFuzzyMatch::ScoreCandidate(
		TEXT("punchbot"),
		{ TEXT("punchbot") },
		Fields);

	TestEqual(TEXT("Phrase and token scores compose"), Score.Score, 157);
	TestTrue(TEXT("Phrase reason recorded"), Score.Reasons.Contains(TEXT("asset_name_phrase")));
	TestTrue(TEXT("Name reason recorded"), Score.Reasons.Contains(TEXT("asset_name")));
	TestTrue(TEXT("Path reason recorded"), Score.Reasons.Contains(TEXT("path")));
	TestEqual(TEXT("Matched tokens deduplicate"), Score.MatchedTokens.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFuzzyTranspositionTest,
	"Monolith.Core.FuzzyMatch.Transposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTranspositionTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Plain Levenshtein counts adjacent swap as two"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("form"), TEXT("from"), 3, false, false),
		2);
	TestEqual(
		TEXT("OSA counts adjacent swap as one"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("form"), TEXT("from"), 3, false, true),
		1);
	TestFalse(
		TEXT("Plain typo gate rejects four-character swap"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("form"), TEXT("from"), false));
	TestTrue(
		TEXT("Transposition typo gate accepts four-character swap"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("form"), TEXT("from"), true));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
