// Copyright Monolith. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithScriptExceptionScope.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithScriptExceptionScopeClassifyTest,
	"Monolith.Core.ScriptExceptionScope.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithScriptExceptionScopeClassifyTest::RunTest(const FString& Parameters)
{
	using FScope = FMonolithScriptExceptionScope;

	// Debug / trace event types are ignored (not errors).
	TestFalse(TEXT("Breakpoint is ignored"),
		FScope::ClassifyException(EBlueprintExceptionType::Breakpoint, TEXT("x")).IsSet());
	TestFalse(TEXT("Tracepoint is ignored"),
		FScope::ClassifyException(EBlueprintExceptionType::Tracepoint, TEXT("x")).IsSet());
	TestFalse(TEXT("WireTracepoint is ignored"),
		FScope::ClassifyException(EBlueprintExceptionType::WireTracepoint, TEXT("x")).IsSet());

	// Error-class types are captured; non-empty description is used verbatim (trimmed).
	{
		const TOptional<FString> Msg =
			FScope::ClassifyException(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None 'Target'"));
		TestTrue(TEXT("AccessViolation is captured"), Msg.IsSet());
		if (Msg.IsSet())
		{
			TestEqual(TEXT("AccessViolation keeps description"), Msg.GetValue(), TEXT("Accessed None 'Target'"));
		}
	}
	{
		const TOptional<FString> Msg =
			FScope::ClassifyException(EBlueprintExceptionType::UserRaisedError, TEXT("  spaced message  "));
		TestTrue(TEXT("UserRaisedError is captured"), Msg.IsSet());
		if (Msg.IsSet())
		{
			TestEqual(TEXT("Description is trimmed"), Msg.GetValue(), TEXT("spaced message"));
		}
	}

	// Empty description falls back to a stable type name (never blank).
	{
		const TOptional<FString> Msg =
			FScope::ClassifyException(EBlueprintExceptionType::NonFatalError, FString());
		TestTrue(TEXT("NonFatalError with empty desc is captured"), Msg.IsSet());
		if (Msg.IsSet())
		{
			TestEqual(TEXT("Empty desc -> type name"), Msg.GetValue(), TEXT("NonFatalError"));
		}
	}
	{
		const TOptional<FString> Msg =
			FScope::ClassifyException(EBlueprintExceptionType::FatalError, TEXT("   "));
		TestTrue(TEXT("FatalError with whitespace desc is captured"), Msg.IsSet());
		if (Msg.IsSet())
		{
			TestEqual(TEXT("Whitespace desc -> type name"), Msg.GetValue(), TEXT("FatalError"));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithScriptExceptionScopeInitialStateTest,
	"Monolith.Core.ScriptExceptionScope.InitialState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithScriptExceptionScopeInitialStateTest::RunTest(const FString& Parameters)
{
	// A scope that captured nothing reports no error and an empty string. Constructing and
	// destroying it also exercises the OnScriptException subscribe/unsubscribe lifecycle.
	FMonolithScriptExceptionScope Scope;
	TestFalse(TEXT("Fresh scope has no error"), Scope.HasError());
	TestEqual(TEXT("Fresh scope error string is empty"), Scope.GetErrorString(), FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
