#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithAssetUtils.h"

// =============================================================================
//  CC-05 — Error Hint regression tests
//
//  These tests verify:
//  1. FMonolithActionResult new fields default to empty (byte-identical legacy).
//  2. FindSimilarActions returns sensible "did you mean" candidates.
//  3. Unknown action dispatch separates candidate actions from recovery actions.
//  4. Chain helpers (WithHint / WithRelatedAction) preserve fluent style.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionResultDefaultsTest,
	"Monolith.Core.ErrorHints.ActionResultDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionResultDefaultsTest::RunTest(const FString& Parameters)
{
	// Legacy callers that only set ErrorMessage / ErrorCode must produce
	// results where the new fields are empty — preserves byte-identical
	// serialization on the wire when no hints are attached.
	FMonolithActionResult R = FMonolithActionResult::Error(TEXT("test error"));
	TestEqual(TEXT("ErrorMessage preserved"), R.ErrorMessage, FString(TEXT("test error")));
	TestFalse(TEXT("Defaults bSuccess false"), R.bSuccess);
	TestEqual(TEXT("RelatedActions empty by default"), R.RelatedActions.Num(), 0);
	TestEqual(TEXT("Hints empty by default"), R.Hints.Num(), 0);
	TestFalse(TEXT("ErrorData null by default"), R.ErrorData.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionResultChainHelpersTest,
	"Monolith.Core.ErrorHints.ChainHelpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionResultChainHelpersTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult R = FMonolithActionResult::Error(TEXT("err"))
		.WithHint(TEXT("try X"))
		.WithRelatedAction(TEXT("X"))
		.WithRelatedAction(TEXT("Y"));

	TestEqual(TEXT("Hint added"), R.Hints.Num(), 1);
	TestEqual(TEXT("Hint content"), R.Hints[0], FString(TEXT("try X")));
	TestEqual(TEXT("Two related actions"), R.RelatedActions.Num(), 2);
	TestEqual(TEXT("First related"), R.RelatedActions[0], FString(TEXT("X")));
	TestEqual(TEXT("Second related"), R.RelatedActions[1], FString(TEXT("Y")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFindSimilarActionsTest,
	"Monolith.Core.ErrorHints.FindSimilarActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFindSimilarActionsTest::RunTest(const FString& Parameters)
{
	// Use a private namespace so we don't pollute or depend on real registrations.
	const FString TestNs(TEXT("__cc05_test_ns__"));
	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	// Register a small set of synthetic actions.
	auto NoopHandler = FMonolithActionHandler::CreateLambda(
		[](const TSharedPtr<FJsonObject>&) { return FMonolithActionResult::Success(MakeShared<FJsonObject>()); });

	Reg.RegisterAction(TestNs, TEXT("list_graphs"),     TEXT("d"), NoopHandler);
	Reg.RegisterAction(TestNs, TEXT("get_graph_data"),  TEXT("d"), NoopHandler);
	Reg.RegisterAction(TestNs, TEXT("get_graph_summary"),TEXT("d"), NoopHandler);
	Reg.RegisterAction(TestNs, TEXT("search_nodes"),    TEXT("d"), NoopHandler);
	Reg.RegisterAction(TestNs, TEXT("compile_blueprint"),TEXT("d"), NoopHandler);

	// 1) Typo "list_grafs" should suggest "list_graphs" first.
	{
		const TArray<FString> Hits = Reg.FindSimilarActions(TestNs, TEXT("list_grafs"));
		TestTrue(TEXT("typo gets suggestions"), Hits.Num() > 0);
		if (Hits.Num() > 0)
		{
			TestEqual(TEXT("typo top hit is list_graphs"), Hits[0], FString(TEXT("list_graphs")));
		}
	}

	// 2) Prefix "get_graph" should match both get_graph_* actions.
	{
		const TArray<FString> Hits = Reg.FindSimilarActions(TestNs, TEXT("get_graph"));
		TestTrue(TEXT("prefix yields multiple hits"), Hits.Num() >= 2);
	}

	// 3) Wildly different name should yield few or no hits.
	{
		const TArray<FString> Hits = Reg.FindSimilarActions(TestNs, TEXT("zzzz_unrelated_xyz"));
		TestTrue(TEXT("very-distant name yields <= 1 hit"), Hits.Num() <= 1);
	}

	// 4) Empty input is safe.
	{
		const TArray<FString> Hits = Reg.FindSimilarActions(TestNs, TEXT(""));
		TestEqual(TEXT("empty name returns empty"), Hits.Num(), 0);
	}

	// 5) Unknown namespace is safe.
	{
		const TArray<FString> Hits = Reg.FindSimilarActions(TEXT("__not_a_namespace__"), TEXT("anything"));
		TestEqual(TEXT("unknown namespace returns empty"), Hits.Num(), 0);
	}

	// Cleanup so we don't leak registrations across test runs.
	Reg.UnregisterNamespace(TestNs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUnknownActionAutoHintsTest,
	"Monolith.Core.ErrorHints.UnknownActionAutoHints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUnknownActionAutoHintsTest::RunTest(const FString& Parameters)
{
	const FString TestNs(TEXT("__cc05_dispatch_ns__"));
	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	auto NoopHandler = FMonolithActionHandler::CreateLambda(
		[](const TSharedPtr<FJsonObject>&) { return FMonolithActionResult::Success(MakeShared<FJsonObject>()); });
	Reg.RegisterAction(TestNs, TEXT("list_graphs"), TEXT("d"), NoopHandler);
	Reg.RegisterAction(TestNs, TEXT("get_graph_data"), TEXT("d"), NoopHandler);

	// Calling a typoed action must (a) error out, (b) carry candidate actions
	// separately from recovery actions, (c) carry the proper JSON-RPC error code.
	FMonolithActionResult R = Reg.ExecuteAction(TestNs, TEXT("list_grafs"), MakeShared<FJsonObject>());
	TestFalse(TEXT("typo dispatch fails"), R.bSuccess);
	TestEqual(TEXT("typo dispatch uses MethodNotFound code"),
		R.ErrorCode, FMonolithJsonUtils::ErrMethodNotFound);
	TestTrue(TEXT("typo dispatch suggests discovery recovery"), R.RelatedActions.Contains(TEXT("monolith.discover")));
	TestTrue(TEXT("typo dispatch suggests find recovery"), R.RelatedActions.Contains(TEXT("monolith.find")));
	TestFalse(TEXT("candidate action is not mixed into related actions"), R.RelatedActions.Contains(TEXT("list_graphs")));
	TestTrue(TEXT("typo dispatch has structured error data"), R.ErrorData.IsValid());
	if (R.ErrorData.IsValid())
	{
		TestEqual(TEXT("typo failure cause"), R.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("unknown_action"));
		TestEqual(TEXT("typo retryability"), R.ErrorData->GetStringField(TEXT("retryability")), TEXT("retry_with_candidate_action_or_discovery"));
		const TArray<TSharedPtr<FJsonValue>>* CandidateActions = nullptr;
		TestTrue(TEXT("typo dispatch surfaces candidate actions"),
			R.ErrorData->TryGetArrayField(TEXT("candidate_actions"), CandidateActions) && CandidateActions && CandidateActions->Num() > 0);
		if (CandidateActions && CandidateActions->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstCandidate = (*CandidateActions)[0]->AsObject();
			TestTrue(TEXT("candidate action object valid"), FirstCandidate.IsValid());
			if (FirstCandidate.IsValid())
			{
				TestEqual(TEXT("candidate action id"), FirstCandidate->GetStringField(TEXT("action_id")), TestNs + TEXT(".list_graphs"));
				TestEqual(TEXT("candidate action name"), FirstCandidate->GetStringField(TEXT("action")), TEXT("list_graphs"));
			}
		}
	}

	// Calling something completely unrelated should still produce a guidance hint
	// pointing the agent at monolith_discover.
	FMonolithActionResult R2 = Reg.ExecuteAction(TestNs, TEXT("zzzz_xyzzy"), MakeShared<FJsonObject>());
	TestFalse(TEXT("unrelated dispatch fails"), R2.bSuccess);
	if (R2.RelatedActions.Num() == 0)
	{
		TestTrue(TEXT("unrelated dispatch falls back to discover hint"), R2.Hints.Num() > 0);
	}

	Reg.UnregisterNamespace(TestNs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithHandlerUnboundGuidanceTest,
	"Monolith.Core.ErrorHints.HandlerUnboundGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHandlerUnboundGuidanceTest::RunTest(const FString& Parameters)
{
	const FString TestNs(TEXT("__cc05_unbound_ns__"));
	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	Reg.RegisterAction(
		TestNs,
		TEXT("registered_without_handler"),
		TEXT("Registered action with intentionally unbound handler."),
		FMonolithActionHandler());

	FMonolithActionResult R = Reg.ExecuteAction(TestNs, TEXT("registered_without_handler"), MakeShared<FJsonObject>());
	TestFalse(TEXT("unbound dispatch fails"), R.bSuccess);
	TestTrue(TEXT("unbound failure has error data"), R.ErrorData.IsValid());
	if (R.ErrorData.IsValid())
	{
		TestEqual(TEXT("unbound action id"), R.ErrorData->GetStringField(TEXT("action_id")), TestNs + TEXT(".registered_without_handler"));
		TestEqual(TEXT("unbound namespace"), R.ErrorData->GetStringField(TEXT("namespace")), TestNs);
		TestEqual(TEXT("unbound action"), R.ErrorData->GetStringField(TEXT("action")), TEXT("registered_without_handler"));
		TestEqual(TEXT("unbound mcp tool"), R.ErrorData->GetStringField(TEXT("mcp_tool")), TestNs + TEXT("_query"));
		TestEqual(TEXT("unbound skill"), R.ErrorData->GetStringField(TEXT("skill")), TEXT("monolith-mcp"));
		TestEqual(TEXT("unbound failure stage"), R.ErrorData->GetStringField(TEXT("failure_stage")), TEXT("handler_unbound"));
		TestEqual(TEXT("unbound failure cause"), R.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("handler_unbound"));
		TestEqual(TEXT("unbound retryability"), R.ErrorData->GetStringField(TEXT("retryability")), TEXT("not_retryable_until_handler_is_registered"));
		TestEqual(TEXT("unbound bug class"), R.ErrorData->GetStringField(TEXT("bug_class")), TEXT("handler_unbound"));
		const TSharedPtr<FJsonObject>* DiscoverArgs = nullptr;
		TestTrue(TEXT("unbound discovery args included"), R.ErrorData->TryGetObjectField(TEXT("discover_args"), DiscoverArgs) && DiscoverArgs && DiscoverArgs->IsValid());
		const TArray<TSharedPtr<FJsonValue>>* PlanningSignals = nullptr;
		TestTrue(TEXT("unbound generated planning signals included"),
			R.ErrorData->TryGetArrayField(TEXT("planning_signals"), PlanningSignals) && PlanningSignals && PlanningSignals->Num() > 0);
	}
	TestTrue(TEXT("unbound related actions include discover"), R.RelatedActions.Contains(TEXT("monolith.discover")));
	TestTrue(TEXT("unbound related actions include find"), R.RelatedActions.Contains(TEXT("monolith.find")));
	TestTrue(TEXT("unbound hint explains schema will not fix it"),
		R.Hints.ContainsByPredicate([](const FString& Hint)
		{
			return Hint.Contains(TEXT("handler delegate is null")) && Hint.Contains(TEXT("Schema changes will not fix"));
		}));

	Reg.UnregisterNamespace(TestNs);
	return true;
}

// =============================================================================
//  ParseAssetCandidateInput — input-form tolerance regression
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseAssetCandidateInputTest,
	"Monolith.Core.ErrorHints.ParseAssetCandidateInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseAssetCandidateInputTest::RunTest(const FString& Parameters)
{
	using FKey = FMonolithAssetUtils::FAssetCandidateKey;

	// 1) Empty / whitespace → empty short name (function safe to call).
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT(""));
		TestEqual(TEXT("empty input has empty short name"), K.ShortName, FString());
		FKey W = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("   "));
		TestEqual(TEXT("whitespace input has empty short name"), W.ShortName, FString());
	}

	// 2) Bare short name → no hints, just the name.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("BB_PunchBot"));
		TestEqual(TEXT("bare short name preserved"), K.ShortName, FString(TEXT("BB_PunchBot")));
		TestEqual(TEXT("bare input has no path hints"), K.PathHints.Num(), 0);
	}

	// 3) Relative path → short name + intermediate segments as hints.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("AI/PunchBot/BB_PunchBot"));
		TestEqual(TEXT("relative short name"), K.ShortName, FString(TEXT("BB_PunchBot")));
		TestEqual(TEXT("relative hint count"), K.PathHints.Num(), 2);
		if (K.PathHints.Num() == 2)
		{
			TestEqual(TEXT("relative hint 0"), K.PathHints[0], FString(TEXT("AI")));
			TestEqual(TEXT("relative hint 1"), K.PathHints[1], FString(TEXT("PunchBot")));
		}
	}

	// 4) Wrong-prefix absolute path → preserved hints.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("/ShooterExplorer/AI/PunchBot/BB_PunchBot"));
		TestEqual(TEXT("abs short name"), K.ShortName, FString(TEXT("BB_PunchBot")));
		TestEqual(TEXT("abs hint count"), K.PathHints.Num(), 3);
	}

	// 5) Object path with redundant suffix and SubObject.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("/Game/Foo/Bar.Bar:Sub"));
		TestEqual(TEXT("subobject stripped, short name"), K.ShortName, FString(TEXT("Bar")));
	}

	// 6) Filesystem absolute path with .uasset and backslashes.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(
			TEXT("D:\\LyraStarterGame\\Content\\AI\\PunchBot\\BB_PunchBot.uasset"));
		TestEqual(TEXT("filesystem short name"), K.ShortName, FString(TEXT("BB_PunchBot")));
		TestTrue(TEXT("filesystem path produces hints"), K.PathHints.Num() >= 2);
		// "/Content/" rewrites to "/Game/" — Game should be in hints.
		bool bSawGame = false;
		for (const FString& H : K.PathHints) { if (H.Equals(TEXT("Game"), ESearchCase::IgnoreCase)) bSawGame = true; }
		TestTrue(TEXT("filesystem prefix rewritten to /Game/"), bSawGame);
	}

	// 7) Mixed separators (forward + back).
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("/Game\\AI/PunchBot\\BB_PunchBot"));
		TestEqual(TEXT("mixed-sep short name"), K.ShortName, FString(TEXT("BB_PunchBot")));
		TestEqual(TEXT("mixed-sep hint count"), K.PathHints.Num(), 3);
	}

	// 8) Trailing whitespace and tabs are trimmed.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("   /Game/Foo/Bar   "));
		TestEqual(TEXT("trim works"), K.ShortName, FString(TEXT("Bar")));
	}

	// 9) Extension only (no path) → still works.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("BP_Hero.uasset"));
		TestEqual(TEXT("extension stripped from bare name"), K.ShortName, FString(TEXT("BP_Hero")));
		TestEqual(TEXT("bare-with-ext has no hints"), K.PathHints.Num(), 0);
	}

	// 10) /Content/ in middle of path (filesystem-style) gets rewritten.
	{
		FKey K = FMonolithAssetUtils::ParseAssetCandidateInput(TEXT("/some/random/Content/Foo/Bar"));
		TestEqual(TEXT("/Content/ rewrite short name"), K.ShortName, FString(TEXT("Bar")));
		// First hint after rewrite should be "Game".
		if (K.PathHints.Num() > 0)
		{
			TestEqual(TEXT("/Content/ becomes /Game/"), K.PathHints[0], FString(TEXT("Game")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
