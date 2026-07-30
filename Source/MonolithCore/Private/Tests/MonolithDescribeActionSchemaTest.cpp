// SPDX-License-Identifier: MIT
// RI ergonomics handover #6 (2026-05-29) automation tests for describe.action_schema.
// Handover: Plugins/Monolith/Docs/plans/2026-05-29-ri-ergonomics-improvements-handover.md
//
// Goals:
//   - `target_action` is the canonical param name and works on its own.
//   - `action` (legacy) still works via the K2 alias rewrite.
//   - `namespace` and `domain` still work as aliases for `target_namespace`.
//   - Supplying BOTH `target_action` and `action` is a collision error.
//   - Missing-required-param error lists ALL missing required params at once
//     (not one round-trip at a time, which was the bug).
//
// Lives under Private/Tests/ for the same UBT auto-include reason as the other
// MonolithCore tests in this folder.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithDescribeActionSchemaTestDetail
{
	/** Resolve the registered describe action_schema; assumes FMonolithBulkFillActions::RegisterAll fired at module startup. */
	static FMonolithActionResult Invoke(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("action_schema"), Params);
	}

	static FMonolithActionResult InvokeSchema(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("schema"), Params);
	}

	static FMonolithActionResult InvokeListTargets(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("list_targets"), Params);
	}
}

// ---------------------------------------------------------------------------
// Test 1: Canonical `target_action` resolves the schema successfully.
// We point it at `describe.list_targets` (an action this test module knows
// is always present because the BulkFillActions register pack ships it).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaTargetActionTest,
	"Monolith.Describe.ActionSchema.TargetAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaTargetActionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("target_action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestTrue(TEXT("target_action canonical resolves successfully"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString Ns, Act;
		R.Result->TryGetStringField(TEXT("namespace"), Ns);
		R.Result->TryGetStringField(TEXT("action"), Act);
		TestEqual(TEXT("namespace echoes describe"), Ns, FString(TEXT("describe")));
		TestEqual(TEXT("action echoes list_targets"), Act, FString(TEXT("list_targets")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Legacy `action` alias still works.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaLegacyActionAliasTest,
	"Monolith.Describe.ActionSchema.LegacyActionAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaLegacyActionAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestTrue(TEXT("legacy `action` alias resolves successfully"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString Act;
		R.Result->TryGetStringField(TEXT("action"), Act);
		TestEqual(TEXT("action echoes list_targets"), Act, FString(TEXT("list_targets")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Legacy `namespace` alias still works with both action spellings.
// This mirrors the high-frequency log pattern:
// `{namespace:"source", target_action:"search_source"}`.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaNamespaceAliasTest,
	"Monolith.Describe.ActionSchema.NamespaceAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaNamespaceAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("target_action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestTrue(TEXT("legacy `namespace` alias resolves successfully"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString Ns, Act;
		R.Result->TryGetStringField(TEXT("namespace"), Ns);
		R.Result->TryGetStringField(TEXT("action"), Act);
		TestEqual(TEXT("namespace echoes describe"), Ns, FString(TEXT("describe")));
		TestEqual(TEXT("action echoes list_targets"), Act, FString(TEXT("list_targets")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: `domain` alias works for target_namespace. This is less common than
// `namespace`, but it appears in invocation logs and should self-correct.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaDomainAliasTest,
	"Monolith.Describe.ActionSchema.DomainAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaDomainAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("domain"), TEXT("describe"));
	Params->SetStringField(TEXT("action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestTrue(TEXT("legacy `domain` alias resolves successfully"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString Ns, Act;
		R.Result->TryGetStringField(TEXT("namespace"), Ns);
		R.Result->TryGetStringField(TEXT("action"), Act);
		TestEqual(TEXT("namespace echoes describe"), Ns, FString(TEXT("describe")));
		TestEqual(TEXT("action echoes list_targets"), Act, FString(TEXT("list_targets")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: Supplying BOTH `target_action` and `action` collides (K2 contract).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaAliasCollisionTest,
	"Monolith.Describe.ActionSchema.AliasCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaAliasCollisionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("target_action"), TEXT("list_targets"));
	Params->SetStringField(TEXT("action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestFalse(TEXT("collision must error, not succeed"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestTrue(TEXT("error message mentions collision"),
			R.ErrorMessage.Contains(TEXT("collision")) ||
			R.ErrorMessage.Contains(TEXT("Param collision")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: Supplying BOTH `target_namespace` and `namespace` collides too.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaNamespaceAliasCollisionTest,
	"Monolith.Describe.ActionSchema.NamespaceAliasCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaNamespaceAliasCollisionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("namespace"), TEXT("describe"));
	Params->SetStringField(TEXT("target_action"), TEXT("list_targets"));

	const FMonolithActionResult R = Invoke(Params);
	TestFalse(TEXT("namespace alias collision must error, not succeed"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestTrue(TEXT("error message mentions collision"),
			R.ErrorMessage.Contains(TEXT("collision")) ||
			R.ErrorMessage.Contains(TEXT("Param collision")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: Missing BOTH required params — error must list both at once
// (the original RI handover #6 friction was discovering missing params
// one round-trip at a time).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaMissingBothTest,
	"Monolith.Describe.ActionSchema.MissingBoth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaMissingBothTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	// Empty params — nothing supplied.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FMonolithActionResult R = Invoke(Params);

	TestFalse(TEXT("missing required params must error"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestEqual(
			TEXT("missing required params use JSON-RPC invalid-params code"),
			R.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("error mentions target_namespace"),
			R.ErrorMessage.Contains(TEXT("target_namespace")));
		TestTrue(TEXT("error mentions target_action"),
			R.ErrorMessage.Contains(TEXT("target_action")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: describe.schema with empty params should guide instead of rejecting.
// Invocation logs showed agents occasionally call this surface with no
// namespace; returning registered namespace guidance avoids a retry loop.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeSchemaEmptyParamsGuidanceTest,
	"Monolith.Describe.Schema.EmptyParamsGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeSchemaEmptyParamsGuidanceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FMonolithActionResult R = InvokeSchema(Params);

	TestTrue(TEXT("empty describe.schema params return guidance success"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString MatchStatus;
		R.Result->TryGetStringField(TEXT("match_status"), MatchStatus);
		TestEqual(TEXT("empty params return namespace_index status"), MatchStatus, FString(TEXT("namespace_index")));
		TestTrue(TEXT("guidance contains available_namespaces"), R.Result->HasField(TEXT("available_namespaces")));
		TestTrue(TEXT("guidance contains next_actions"), R.Result->HasField(TEXT("next_actions")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: unknown describe namespace should be a structured no_adapter result,
// not a hard error that sends callers into a blind retry.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeSchemaUnknownNamespaceGuidanceTest,
	"Monolith.Describe.Schema.UnknownNamespaceGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeSchemaUnknownNamespaceGuidanceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_namespace"), TEXT("__test_missing_inventory_namespace"));
	const FMonolithActionResult R = InvokeSchema(Params);

	TestTrue(TEXT("unknown describe namespace returns guidance success"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString MatchStatus;
		R.Result->TryGetStringField(TEXT("match_status"), MatchStatus);
		TestEqual(TEXT("unknown namespace returns no_adapter status"), MatchStatus, FString(TEXT("no_adapter")));
		bool bRegistered = true;
		R.Result->TryGetBoolField(TEXT("registered"), bRegistered);
		TestFalse(TEXT("unknown namespace reports registered=false"), bRegistered);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: describe.list_targets gets the same empty-param guidance behavior.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeListTargetsEmptyParamsGuidanceTest,
	"Monolith.Describe.ListTargets.EmptyParamsGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeListTargetsEmptyParamsGuidanceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDescribeActionSchemaTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FMonolithActionResult R = InvokeListTargets(Params);

	TestTrue(TEXT("empty describe.list_targets params return guidance success"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		FString MatchStatus;
		R.Result->TryGetStringField(TEXT("match_status"), MatchStatus);
		TestEqual(TEXT("empty params return namespace_index status"), MatchStatus, FString(TEXT("namespace_index")));
		TestTrue(TEXT("guidance contains available_namespaces"), R.Result->HasField(TEXT("available_namespaces")));
		TestTrue(TEXT("guidance contains next_actions"), R.Result->HasField(TEXT("next_actions")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
