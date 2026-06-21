#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"
#include "MonolithMcpSessionTracker.h"
#include "MonolithToolProfileManager.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// P1c MCP session-mode spec-correctness slice. These tests cover the pure,
// static session gate (FMonolithHttpServer::EvaluateSessionGate), the additive
// session-tracker lifecycle methods, and the ProfileManager tool-list revision
// counter. The gate is verified to be a no-op pass-through when session mode is
// off so wire behavior stays byte-identical to legacy.

namespace
{
	bool JsonContainsRawString(const TSharedPtr<FJsonObject>& Obj, const FString& Raw)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FString Serialized;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Serialized.Contains(Raw);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionGateDisabledPassesTest,
	"Monolith.Core.McpSessionGate.DisabledPasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionGateDisabledPassesTest::RunTest(const FString& Parameters)
{
	// With session mode off the gate must never reject, regardless of inputs that
	// would be rejected when enabled (missing session id + post-init method,
	// unknown session, unsupported protocol version).
	const TArray<FString> PostInit = { TEXT("tools/call") };

	FMonolithHttpServer::FSessionGateResult R1 = FMonolithHttpServer::EvaluateSessionGate(
		PostInit, TEXT(""), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/false);
	TestFalse(TEXT("Disabled: missing session id does not reject"), R1.bReject);

	FMonolithHttpServer::FSessionGateResult R2 = FMonolithHttpServer::EvaluateSessionGate(
		PostInit, TEXT("sess-1"), TEXT("9999-99-99"), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/false);
	TestFalse(TEXT("Disabled: unknown session + bad version does not reject"), R2.bReject);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionGateMissingIdRejectsTest,
	"Monolith.Core.McpSessionGate.MissingIdRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionGateMissingIdRejectsTest::RunTest(const FString& Parameters)
{
	// Post-initialize method with no MCP-Session-Id header -> InvalidRequest + 400.
	const TArray<FString> PostInit = { TEXT("tools/call") };
	FMonolithHttpServer::FSessionGateResult R = FMonolithHttpServer::EvaluateSessionGate(
		PostInit, TEXT(""), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestTrue(TEXT("Missing id rejects"), R.bReject);
	TestEqual(TEXT("HTTP 400"), static_cast<int32>(R.HttpCode), static_cast<int32>(EHttpServerResponseCodes::BadRequest));
	TestEqual(TEXT("JSON-RPC InvalidRequest"), R.RpcCode, FMonolithJsonUtils::ErrInvalidRequest);

	// initialize / notifications/initialized / ping are exempt (no id required).
	const TArray<FString> Handshake = { TEXT("initialize"), TEXT("notifications/initialized"), TEXT("ping") };
	for (const FString& Method : Handshake)
	{
		FMonolithHttpServer::FSessionGateResult Pass = FMonolithHttpServer::EvaluateSessionGate(
			{ Method }, TEXT(""), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
		TestFalse(FString::Printf(TEXT("Handshake method exempt: %s"), *Method), Pass.bReject);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionGateUnknownSessionRejectsTest,
	"Monolith.Core.McpSessionGate.UnknownSessionRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionGateUnknownSessionRejectsTest::RunTest(const FString& Parameters)
{
	const TArray<FString> PostInit = { TEXT("tools/call") };

	// Supplied session id that matches no observed row -> 404.
	FMonolithHttpServer::FSessionGateResult Unknown = FMonolithHttpServer::EvaluateSessionGate(
		PostInit, TEXT("sess-unknown"), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestTrue(TEXT("Unknown session rejects"), Unknown.bReject);
	TestEqual(TEXT("HTTP 404"), static_cast<int32>(Unknown.HttpCode), static_cast<int32>(EHttpServerResponseCodes::NotFound));

	// Known session id + post-init method -> pass.
	FMonolithHttpServer::FSessionGateResult Known = FMonolithHttpServer::EvaluateSessionGate(
		PostInit, TEXT("sess-known"), TEXT(""), /*bSessionKnown=*/true, /*bSessionModeEnabled=*/true);
	TestFalse(TEXT("Known session passes"), Known.bReject);

	// initialize carrying a not-yet-observed (new) session id must NOT be rejected: that is how the
	// client registers its id (the gate runs before MarkInitialize, and the server echoes no id). A
	// live MCP handshake confirmed that without this exemption, enabling session mode makes every
	// session impossible to establish (initialize itself 404s). Regression guard.
	FMonolithHttpServer::FSessionGateResult NewInit = FMonolithHttpServer::EvaluateSessionGate(
		{ TEXT("initialize") }, TEXT("sess-brand-new"), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestFalse(TEXT("initialize with a new (unknown) session id is exempt from the 404"), NewInit.bReject);

	// notifications/initialized with that same new id is likewise exempt.
	FMonolithHttpServer::FSessionGateResult NewInitialized = FMonolithHttpServer::EvaluateSessionGate(
		{ TEXT("notifications/initialized") }, TEXT("sess-brand-new"), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestFalse(TEXT("notifications/initialized with a new id is exempt"), NewInitialized.bReject);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionGateProtocolVersionTest,
	"Monolith.Core.McpSessionGate.ProtocolVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionGateProtocolVersionTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Init = { TEXT("initialize") };

	// Unsupported protocol version header -> InvalidRequest + 400 even for initialize.
	FMonolithHttpServer::FSessionGateResult Bad = FMonolithHttpServer::EvaluateSessionGate(
		Init, TEXT(""), TEXT("1999-01-01"), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestTrue(TEXT("Unsupported version rejects"), Bad.bReject);
	TestEqual(TEXT("HTTP 400"), static_cast<int32>(Bad.HttpCode), static_cast<int32>(EHttpServerResponseCodes::BadRequest));
	TestEqual(TEXT("JSON-RPC InvalidRequest"), Bad.RpcCode, FMonolithJsonUtils::ErrInvalidRequest);

	// A supported protocol version passes (use the server-preferred latest).
	const FString Supported = FMonolithHttpServer::GetSupportedProtocolVersions().Last();
	FMonolithHttpServer::FSessionGateResult Good = FMonolithHttpServer::EvaluateSessionGate(
		Init, TEXT(""), Supported, /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestFalse(TEXT("Supported version passes"), Good.bReject);

	// An empty protocol version header is not validated (omitted header is legal).
	FMonolithHttpServer::FSessionGateResult Empty = FMonolithHttpServer::EvaluateSessionGate(
		Init, TEXT(""), TEXT(""), /*bSessionKnown=*/false, /*bSessionModeEnabled=*/true);
	TestFalse(TEXT("Empty version passes"), Empty.bReject);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionTrackerLifecycleTest,
	"Monolith.Core.McpSessionTracker.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionTrackerLifecycleTest::RunTest(const FString& Parameters)
{
	FMonolithMcpSessionTracker& Tracker = FMonolithMcpSessionTracker::Get();
	Tracker.ResetForTests();

	const FString RawSessionId = TEXT("session_lifecycle_abcdef123456");
	TestFalse(TEXT("Unknown session before initialize"), Tracker.IsKnownSession(RawSessionId));

	// MarkInitialize seeds the row, marks initializing, records capability booleans.
	Tracker.MarkInitialize(RawSessionId, TEXT("2025-06-18"), /*roots=*/true, /*sampling=*/false, /*elicitation=*/true);
	TestTrue(TEXT("Known session after initialize"), Tracker.IsKnownSession(RawSessionId));

	TSharedPtr<FJsonObject> AfterInit = Tracker.ListSessionsJson(10);
	TestFalse(TEXT("Raw session id never serialized"), JsonContainsRawString(AfterInit, RawSessionId));

	const TArray<TSharedPtr<FJsonValue>>* Sessions = nullptr;
	TestTrue(TEXT("sessions present"), AfterInit->TryGetArrayField(TEXT("sessions"), Sessions));
	if (Sessions && Sessions->Num() == 1)
	{
		TSharedPtr<FJsonObject> Row = (*Sessions)[0]->AsObject();
		TestEqual(TEXT("Status is initializing"), Row->GetStringField(TEXT("lifecycle_status")), TEXT("initializing"));

		const TSharedPtr<FJsonObject>* Caps = nullptr;
		TestTrue(TEXT("client_capabilities present"), Row->TryGetObjectField(TEXT("client_capabilities"), Caps));
		if (Caps)
		{
			TestTrue(TEXT("roots capability recorded"), (*Caps)->GetBoolField(TEXT("roots")));
			TestFalse(TEXT("sampling capability recorded"), (*Caps)->GetBoolField(TEXT("sampling")));
			TestTrue(TEXT("elicitation capability recorded"), (*Caps)->GetBoolField(TEXT("elicitation")));
		}
	}

	// MarkInitialized advances to initialized.
	Tracker.MarkInitialized(RawSessionId);
	TSharedPtr<FJsonObject> AfterInitialized = Tracker.ListSessionsJson(10);
	const TArray<TSharedPtr<FJsonValue>>* Sessions2 = nullptr;
	if (AfterInitialized->TryGetArrayField(TEXT("sessions"), Sessions2) && Sessions2 && Sessions2->Num() == 1)
	{
		TSharedPtr<FJsonObject> Row = (*Sessions2)[0]->AsObject();
		TestEqual(TEXT("Status is initialized"), Row->GetStringField(TEXT("lifecycle_status")), TEXT("initialized"));
	}

	// MarkInitialized on an unknown session is a no-op (does not seed a row).
	Tracker.MarkInitialized(TEXT("never_seen_session_id"));
	TestFalse(TEXT("Unknown session not created by MarkInitialized"), Tracker.IsKnownSession(TEXT("never_seen_session_id")));

	Tracker.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolListRevisionTest,
	"Monolith.Core.ToolProfile.ToolListRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolListRevisionTest::RunTest(const FString& Parameters)
{
	FMonolithToolProfileManager& Manager = FMonolithToolProfileManager::Get();

	const int64 Before = Manager.GetToolListRevision();

	// Upsert a custom (non-built-in) profile so subsequent mutators are permitted.
	FMonolithToolProfile Profile;
	Profile.Id = TEXT("p1c_revision_test_profile");
	Profile.DisplayName = TEXT("P1c Revision Test");
	Profile.Mode = TEXT("denylist");
	FString Error;
	const bool bUpserted = Manager.UpsertProfile(Profile, /*bCreateOnly=*/false, Error);
	TestTrue(FString::Printf(TEXT("Upsert succeeds: %s"), *Error), bUpserted);

	const int64 AfterUpsert = Manager.GetToolListRevision();
	TestTrue(TEXT("Upsert bumps revision"), AfterUpsert > Before);

	// A namespace-enable mutation on the custom profile bumps again.
	const bool bNsSet = Manager.SetNamespaceEnabled(Profile.Id, TEXT("blueprint"), /*bEnabled=*/false, Error);
	TestTrue(FString::Printf(TEXT("SetNamespaceEnabled succeeds: %s"), *Error), bNsSet);
	const int64 AfterNs = Manager.GetToolListRevision();
	TestTrue(TEXT("SetNamespaceEnabled bumps revision"), AfterNs > AfterUpsert);

	// Clean up the test profile (delete also bumps; revision is monotonic).
	const bool bDeleted = Manager.DeleteProfile(Profile.Id, Error);
	TestTrue(FString::Printf(TEXT("Delete cleanup succeeds: %s"), *Error), bDeleted);
	TestTrue(TEXT("Delete bumps revision"), Manager.GetToolListRevision() > AfterNs);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
