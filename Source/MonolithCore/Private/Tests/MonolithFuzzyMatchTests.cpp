// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "../../Public/MonolithFuzzyMatch.h"

// =============================================================================
//  FMonolithFuzzyMatch — shared fuzzy/distance engine unit tests.
//
//  These guard the primitives that monolith.find, FindSimilarActions, and
//  asset.find_assets all share. The legacy callers' own automation tests
//  (Monolith.Core.ErrorHints.FindSimilarActions, monolith.find golden tests)
//  remain the byte-for-byte parity guard at the call-site level.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyNormalizeTextTest,
	"Monolith.Core.FuzzyMatch.NormalizeText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyNormalizeTextTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("lowercase + punctuation -> space + collapse"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("Hello, World!")), FString(TEXT("hello world")));
	TestEqual(TEXT("c++ folds to cpp"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("C++ Code")), FString(TEXT("cpp code")));
	TestEqual(TEXT("whitespace collapses"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("Find  Caller   Graph")), FString(TEXT("find caller graph")));
	TestEqual(TEXT("empty stays empty"),
		FMonolithFuzzyMatch::NormalizeText(TEXT("")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyTokenizeTest,
	"Monolith.Core.FuzzyMatch.Tokenize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTokenizeTest::RunTest(const FString& Parameters)
{
	// Stopwords and <2-char tokens are dropped; duplicates de-duplicated.
	{
		const TArray<FString> Tokens = FMonolithFuzzyMatch::Tokenize(TEXT("the graph node"));
		TestEqual(TEXT("stopword removed -> 2 tokens"), Tokens.Num(), 2);
		TestTrue(TEXT("has graph"), Tokens.Contains(TEXT("graph")));
		TestTrue(TEXT("has node"), Tokens.Contains(TEXT("node")));
		TestFalse(TEXT("stopword 'the' dropped"), Tokens.Contains(TEXT("the")));
	}
	{
		const TArray<FString> Tokens = FMonolithFuzzyMatch::Tokenize(TEXT("graph graph"));
		TestEqual(TEXT("duplicates collapse"), Tokens.Num(), 1);
	}

	// No alias expansion without a table.
	{
		const TArray<FString> Tokens = FMonolithFuzzyMatch::Tokenize(TEXT("bp graph"));
		TestFalse(TEXT("no alias expansion without table"), Tokens.Contains(TEXT("blueprint")));
	}

	// Alias table expands matching base tokens (snapshot-based, deduped).
	{
		const TMap<FString, TArray<FString>> Aliases = {
			{ TEXT("bp"), { TEXT("blueprint") } },
			{ TEXT("vfx"), { TEXT("niagara") } }
		};
		const TArray<FString> Tokens = FMonolithFuzzyMatch::Tokenize(TEXT("bp vfx graph"), &Aliases);
		TestTrue(TEXT("bp expands to blueprint"), Tokens.Contains(TEXT("blueprint")));
		TestTrue(TEXT("vfx expands to niagara"), Tokens.Contains(TEXT("niagara")));
		TestTrue(TEXT("base tokens retained"), Tokens.Contains(TEXT("bp")) && Tokens.Contains(TEXT("graph")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyEditDistanceTest,
	"Monolith.Core.FuzzyMatch.EditDistanceBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyEditDistanceTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("classic kitten/sitting = 3"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("kitten"), TEXT("sitting"), 10, false), 3);
	TestEqual(TEXT("identical = 0"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("abc"), TEXT("abc"), 5, false), 0);

	// Case sensitivity flag.
	TestEqual(TEXT("case-sensitive ABC/abc = 3"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("ABC"), TEXT("abc"), 5, false), 3);
	TestEqual(TEXT("case-insensitive ABC/abc = 0"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("ABC"), TEXT("abc"), 5, true), 0);

	// Bounded early-out returns MaxDistance+1 once exceeded.
	const int32 Bounded = FMonolithFuzzyMatch::EditDistanceBounded(TEXT("abcdef"), TEXT("uvwxyz"), 2, false);
	TestEqual(TEXT("bounded early-out returns MaxDistance+1"), Bounded, 3);

	// Length-difference early-out.
	const int32 LenGap = FMonolithFuzzyMatch::EditDistanceBounded(TEXT("a"), TEXT("abcdef"), 2, false);
	TestTrue(TEXT("length gap exceeds bound"), LenGap > 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyTypoMatchTest,
	"Monolith.Core.FuzzyMatch.IsTypoMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTypoMatchTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("blueprint/blueprnt (len>=7 -> dist<=2)"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("blueprint"), TEXT("blueprnt")));
	TestFalse(TEXT("short tokens (<4) never typo-match"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("cat"), TEXT("cot")));
	TestFalse(TEXT("different first char rejected"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("abcd"), TEXT("xbcd")));
	TestTrue(TEXT("4-char dist 1 accepted"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("abcd"), TEXT("abce")));
	TestFalse(TEXT("4-char dist 2 exceeds bound (max 1)"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("abcd"), TEXT("abef")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyScoreTokensTest,
	"Monolith.Core.FuzzyMatch.ScoreTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyScoreTokensTest::RunTest(const FString& Parameters)
{
	const FMonolithFuzzyWeights Weights{ 10, 5, 2, 3 };
	const TArray<FString> FieldTokens = { TEXT("graph"), TEXT("node") };

	// Exact token match.
	{
		TArray<FString> Reasons; TSet<FString> Matched;
		const int32 Score = FMonolithFuzzyMatch::ScoreTokens(
			{ TEXT("graph") }, FieldTokens, TEXT("graph node"), Weights, TEXT("f"), Reasons, Matched);
		TestEqual(TEXT("exact -> Exact weight"), Score, 10);
		TestTrue(TEXT("reason recorded"), Reasons.Contains(TEXT("f")));
		TestTrue(TEXT("matched token recorded"), Matched.Contains(TEXT("graph")));
	}
	// Prefix match.
	{
		TArray<FString> Reasons; TSet<FString> Matched;
		const int32 Score = FMonolithFuzzyMatch::ScoreTokens(
			{ TEXT("grap") }, FieldTokens, TEXT("graph node"), Weights, TEXT("f"), Reasons, Matched);
		TestEqual(TEXT("prefix -> Prefix weight"), Score, 5);
	}
	// Contains (substring of field text, not a token prefix).
	{
		TArray<FString> Reasons; TSet<FString> Matched;
		const int32 Score = FMonolithFuzzyMatch::ScoreTokens(
			{ TEXT("aph") }, FieldTokens, TEXT("graph node"), Weights, TEXT("f"), Reasons, Matched);
		TestEqual(TEXT("contains -> Contains weight"), Score, 2);
	}
	// Fuzzy typo match + best distance out-param.
	{
		TArray<FString> Reasons; TSet<FString> Matched; int32 BestDistance = MAX_int32;
		const int32 Score = FMonolithFuzzyMatch::ScoreTokens(
			{ TEXT("blueprint") }, { TEXT("blueprnt") }, TEXT("blueprnt"), Weights, TEXT("f"), Reasons, Matched, &BestDistance);
		TestEqual(TEXT("fuzzy -> Fuzzy weight"), Score, 3);
		TestTrue(TEXT("fuzzy reason recorded"), Reasons.Contains(TEXT("f_fuzzy")));
		TestEqual(TEXT("best distance captured"), BestDistance, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyScoreCandidateTest,
	"Monolith.Core.FuzzyMatch.ScoreCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyScoreCandidateTest::RunTest(const FString& Parameters)
{
	FMonolithFuzzyField NameField;
	NameField.Text = TEXT("bb_punchbot");
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

	// query "punchbot": name phrase-contains (+90) + name token exact (+45) + path token exact (+22).
	const FMonolithFuzzyScore Score = FMonolithFuzzyMatch::ScoreCandidate(
		TEXT("punchbot"), { TEXT("punchbot") }, Fields);

	TestEqual(TEXT("composed score = 90+45+22"), Score.Score, 157);
	TestTrue(TEXT("name phrase reason"), Score.Reasons.Contains(TEXT("asset_name_phrase")));
	TestTrue(TEXT("name token reason"), Score.Reasons.Contains(TEXT("asset_name")));
	TestTrue(TEXT("path token reason"), Score.Reasons.Contains(TEXT("path")));
	TestEqual(TEXT("matched tokens deduped to one"), Score.MatchedTokens.Num(), 1);
	TestTrue(TEXT("matched token is punchbot"), Score.MatchedTokens.Contains(TEXT("punchbot")));

	// Exact whole-query phrase bonus tier.
	{
		FMonolithFuzzyField ExactField;
		ExactField.Text = TEXT("punchbot");
		ExactField.Tokens = { TEXT("punchbot") };
		ExactField.Weights = FMonolithFuzzyWeights{ 45, 30, 16, 8 };
		ExactField.ExactPhraseBonus = 200;
		ExactField.ContainsPhraseBonus = 90;
		ExactField.ReasonTag = TEXT("asset_name");
		const TArray<FMonolithFuzzyField> ExactFields = { ExactField };
		const FMonolithFuzzyScore ExactScore = FMonolithFuzzyMatch::ScoreCandidate(
			TEXT("punchbot"), { TEXT("punchbot") }, ExactFields);
		TestEqual(TEXT("exact phrase (200) + exact token (45)"), ExactScore.Score, 245);
		TestTrue(TEXT("exact phrase reason"), ExactScore.Reasons.Contains(TEXT("asset_name_exact")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFuzzyTranspositionTest,
	"Monolith.Core.FuzzyMatch.Transposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFuzzyTranspositionTest::RunTest(const FString& Parameters)
{
	// Adjacent transposition: plain Levenshtein = 2, OSA (transposition on) = 1.
	TestEqual(TEXT("plain: form/from = 2"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("form"), TEXT("from"), 3, false, false), 2);
	TestEqual(TEXT("OSA: form/from = 1"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("form"), TEXT("from"), 3, false, true), 1);
	TestEqual(TEXT("plain: receive/recieve = 2"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("receive"), TEXT("recieve"), 3, false, false), 2);
	TestEqual(TEXT("OSA: receive/recieve = 1"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("receive"), TEXT("recieve"), 3, false, true), 1);

	// The flag must not change non-transposition results (parity safety).
	TestEqual(TEXT("kitten/sitting still 3 with transposition on"),
		FMonolithFuzzyMatch::EditDistanceBounded(TEXT("kitten"), TEXT("sitting"), 10, false, true), 3);

	// Typo gate: 4-char single swap is rejected by plain (dist 2 > max 1), accepted by OSA.
	TestFalse(TEXT("plain typo gate rejects form/from"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("form"), TEXT("from"), false));
	TestTrue(TEXT("OSA typo gate accepts form/from"),
		FMonolithFuzzyMatch::IsTypoMatch(TEXT("form"), TEXT("from"), true));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
