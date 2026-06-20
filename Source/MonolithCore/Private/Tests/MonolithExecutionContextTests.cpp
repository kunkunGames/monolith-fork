#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithExecutionContext.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithExecutionContextScopedAccessorTest,
	"Monolith.Core.ExecutionContext.ScopedAccessor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithExecutionContextScopedAccessorTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("No current context before scope"), FMonolithExecutionContext::HasCurrent());

	FMonolithExecutionContext::FParams OuterParams;
	OuterParams.JsonRpcId = TEXT("42");
	OuterParams.ToolCallId = TEXT("local-test-outer");
	OuterParams.SessionIdRedacted = FMonolithExecutionContext::RedactSessionId(TEXT("session-outer"));
	OuterParams.SourceToolName = TEXT("blueprint_query");
	OuterParams.Namespace = TEXT("blueprint");
	OuterParams.Action = TEXT("compile_blueprint");
	OuterParams.ProgressToken = TEXT("progress-outer");
	OuterParams.bCancellable = true;
	FMonolithExecutionContext OuterContext(OuterParams);

	{
		FScopedMonolithExecutionContext OuterScope(OuterContext);
		const FMonolithExecutionContext* Current = FMonolithExecutionContext::GetCurrent();
		TestNotNull(TEXT("Current context exists"), Current);
		if (Current)
		{
			TestEqual(TEXT("JSON-RPC id preserved"), Current->GetJsonRpcId(), FString(TEXT("42")));
			TestEqual(TEXT("Progress token preserved"), Current->GetProgressToken(), FString(TEXT("progress-outer")));
			TestTrue(TEXT("Context is cancellable"), Current->IsCancellable());
			TestFalse(TEXT("Cancellation not requested initially"), Current->IsCancellationRequested());
		}

		OuterContext.RequestCancellation(TEXT("unit test"));
		TestTrue(TEXT("Cancellation flag is observable"), OuterContext.IsCancellationRequested());
		TestEqual(TEXT("Cancellation reason preserved"), OuterContext.GetCancellationReason(), FString(TEXT("unit test")));

		FMonolithExecutionContext::FParams InnerParams;
		InnerParams.JsonRpcId = TEXT("43");
		InnerParams.ToolCallId = TEXT("local-test-inner");
		FMonolithExecutionContext InnerContext(InnerParams);
		{
			FScopedMonolithExecutionContext InnerScope(InnerContext);
			TestEqual(
				TEXT("Nested scope replaces current context"),
				FMonolithExecutionContext::GetCurrent()->GetJsonRpcId(),
				FString(TEXT("43")));
		}

		TestEqual(
			TEXT("Outer scope restored after nested scope"),
			FMonolithExecutionContext::GetCurrent()->GetJsonRpcId(),
			FString(TEXT("42")));
	}

	TestFalse(TEXT("No current context after scope"), FMonolithExecutionContext::HasCurrent());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithExecutionContextHelpersTest,
	"Monolith.Core.ExecutionContext.Helpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithExecutionContextHelpersTest::RunTest(const FString& Parameters)
{
	// Session id redaction never leaks the raw id and is stable.
	const FString Redacted = FMonolithExecutionContext::RedactSessionId(TEXT("super-secret-session"));
	TestTrue(TEXT("Redacted id is sha256-prefixed"), Redacted.StartsWith(TEXT("sha256:")));
	TestFalse(TEXT("Redacted id omits the raw session"), Redacted.Contains(TEXT("super-secret-session")));
	TestEqual(TEXT("Redaction is deterministic"),
		FMonolithExecutionContext::RedactSessionId(TEXT("super-secret-session")), Redacted);
	TestEqual(TEXT("Empty session is stateless"),
		FMonolithExecutionContext::RedactSessionId(FString()), FString(TEXT("stateless")));

	// Locally generated ToolCall ids are unique and prefixed.
	const FString IdA = FMonolithExecutionContext::GenerateLocalToolCallId();
	const FString IdB = FMonolithExecutionContext::GenerateLocalToolCallId();
	TestTrue(TEXT("Generated id is local-prefixed"), IdA.StartsWith(TEXT("local-")));
	TestNotEqual(TEXT("Generated ids are unique"), IdA, IdB);

	// Progress token extraction reads params._meta.progressToken.
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("progressToken"), TEXT("progress-7"));
	TSharedPtr<FJsonObject> RpcParams = MakeShared<FJsonObject>();
	RpcParams->SetObjectField(TEXT("_meta"), Meta);
	TestEqual(TEXT("Progress token extracted from _meta"),
		FMonolithExecutionContext::ExtractProgressToken(RpcParams), FString(TEXT("progress-7")));
	TestEqual(TEXT("Missing _meta yields empty token"),
		FMonolithExecutionContext::ExtractProgressToken(MakeShared<FJsonObject>()), FString());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithExecutionContextToolCallRecordTest,
	"Monolith.Core.ExecutionContext.ToolCallRecordUsesContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithExecutionContextToolCallRecordTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	// bEnableAdvancedToolCallRecords gates record RETRIEVAL (GetToolCallRecordsJson),
	// not append — rows are appended unconditionally by EndAction.
	const bool bOriginalRecords = Settings->bEnableAdvancedToolCallRecords;
	Settings->bEnableAdvancedToolCallRecords = true;

	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();

	const FString RedactedSession = FMonolithExecutionContext::RedactSessionId(TEXT("record-session"));

	FMonolithExecutionContext::FParams ContextParams;
	ContextParams.JsonRpcId = TEXT("99");
	ContextParams.ToolCallId = TEXT("local-record-context");
	ContextParams.SessionIdRedacted = RedactedSession;
	ContextParams.SourceToolName = TEXT("material_query");
	ContextParams.Namespace = TEXT("material");
	ContextParams.Action = TEXT("inspect_material");
	ContextParams.ProgressToken = TEXT("progress-record");
	FMonolithExecutionContext Context(ContextParams);

	{
		FScopedMonolithExecutionContext ContextScope(Context);
		FMonolithActionExecutionGuard::FExecutionScope Scope = Guard.BeginAction(TEXT("material"), TEXT("inspect_material"));
		TSharedPtr<FJsonObject> SuccessPayload = MakeShared<FJsonObject>();
		SuccessPayload->SetBoolField(TEXT("ok"), true);
		Guard.SetActionOutcome(Scope, true, 0, SuccessPayload, FString());
		Guard.EndAction(Scope);
	}

	TSharedPtr<FJsonObject> Records = Guard.GetToolCallRecordsJson(10, FString(), FString());
	TestTrue(TEXT("Records result object exists"), Records.IsValid());
	if (Records.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("Records array exists"), Records->TryGetArrayField(TEXT("records"), Rows));
		TestTrue(TEXT("One row returned"), Rows && Rows->Num() == 1);
		if (Rows && Rows->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			TestTrue(TEXT("Row is object"), (*Rows)[0]->TryGetObject(Row));
			if (Row && Row->IsValid())
			{
				TestEqual(TEXT("JSON-RPC id copied from context"), (*Row)->GetStringField(TEXT("json_rpc_id")), FString(TEXT("99")));
				TestEqual(TEXT("ToolCall id copied from context"), (*Row)->GetStringField(TEXT("tool_call_id")), FString(TEXT("local-record-context")));
				TestEqual(TEXT("Session id is redacted from context"), (*Row)->GetStringField(TEXT("session_id_redacted")), RedactedSession);
				TestEqual(TEXT("Progress token copied from context"), (*Row)->GetStringField(TEXT("progress_token")), FString(TEXT("progress-record")));
			}
		}
	}

	Settings->bEnableAdvancedToolCallRecords = bOriginalRecords;
	Guard.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
