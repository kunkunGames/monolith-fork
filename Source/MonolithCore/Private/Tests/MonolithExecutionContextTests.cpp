#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithExecutionContext.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
