// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Actions/MonolithUIContextActions.h"
#include "Components/Button.h"
#include "Dom/JsonObject.h"
#include "MonolithExecutionContext.h"
#include "MonolithToolRegistry.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

namespace
{
	void EnsureContextActionsRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("ui"), TEXT("set_widget_context")))
		{
			MonolithUI::FContextActions::Register(Registry);
		}
	}

	FMonolithExecutionContext::FParams MakeTestContextParams(const FString& SessionId, const FString& ToolCallId)
	{
		FMonolithExecutionContext::FParams Params;
		Params.JsonRpcId = ToolCallId;
		Params.ToolCallId = ToolCallId;
		Params.SessionIdRedacted = FMonolithExecutionContext::RedactSessionId(SessionId);
		Params.Namespace = TEXT("ui");
		Params.Action = TEXT("context_test");
		return Params;
	}

	TSharedPtr<FJsonObject> MakeClearParams(const FString& Scope = TEXT("all"))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("scope"), Scope);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIContextSessionResolveTest,
	"MonolithUI.Context.SessionResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIContextSessionResolveTest::RunTest(const FString& /*Parameters*/)
{
	EnsureContextActionsRegistered();

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_ContextSessionResolve");
	FString Error;
	if (!TestTrue(TEXT("fixture WBP created"),
		MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
			AssetPath,
			TEXT("StartButton"),
			UButton::StaticClass(),
			Error)))
	{
		AddError(Error);
		return false;
	}

	FMonolithExecutionContext Context(MakeTestContextParams(TEXT("ui-context-session-a"), TEXT("tool-a")));
	FScopedMonolithExecutionContext Scope(Context);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetParams->SetStringField(TEXT("widget_name"), TEXT("StartButton"));
	SetParams->SetStringField(TEXT("animation_name"), TEXT("Intro"));
	SetParams->SetStringField(TEXT("scope"), TEXT("session"));
	SetParams->SetNumberField(TEXT("ttl_seconds"), 120);

	const FMonolithActionResult SetResult = Registry.ExecuteAction(TEXT("ui"), TEXT("set_widget_context"), SetParams);
	if (!TestTrue(TEXT("set_widget_context succeeds"), SetResult.bSuccess && SetResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("context is not usable as hidden mutation default"),
		SetResult.Result->GetBoolField(TEXT("usable_as_default_for_mutation")));
	TestEqual(TEXT("resolved_from"), SetResult.Result->GetStringField(TEXT("resolved_from")), TEXT("session_context"));

	const TSharedPtr<FJsonObject>* SetResolved = nullptr;
	TestTrue(TEXT("set resolved object exists"), SetResult.Result->TryGetObjectField(TEXT("resolved"), SetResolved));
	if (SetResolved && SetResolved->IsValid())
	{
		TestTrue(TEXT("asset exists"), (*SetResolved)->GetBoolField(TEXT("asset_exists")));
		TestTrue(TEXT("widget exists"), (*SetResolved)->GetBoolField(TEXT("widget_exists")));
		TestFalse(TEXT("animation missing is stale but non-fatal"), (*SetResolved)->GetBoolField(TEXT("animation_exists")));
		TestEqual(TEXT("resolved status"), (*SetResolved)->GetStringField(TEXT("status")), TEXT("stale_reference"));
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("scope"), TEXT("auto"));
	const FMonolithActionResult GetResult = Registry.ExecuteAction(TEXT("ui"), TEXT("get_widget_context"), GetParams);
	if (!TestTrue(TEXT("get_widget_context succeeds"), GetResult.bSuccess && GetResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("has context"), GetResult.Result->GetBoolField(TEXT("has_context")));
	TestEqual(TEXT("get resolved_from"), GetResult.Result->GetStringField(TEXT("resolved_from")), TEXT("session_context"));

	const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
	TestTrue(TEXT("recent contexts returned"),
		GetResult.Result->TryGetArrayField(TEXT("recent_contexts"), Recent) && Recent && Recent->Num() > 0);

	Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIContextSessionIsolationTest,
	"MonolithUI.Context.SessionIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIContextSessionIsolationTest::RunTest(const FString& /*Parameters*/)
{
	EnsureContextActionsRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	{
		FMonolithExecutionContext ContextA(MakeTestContextParams(TEXT("ui-context-isolation-a"), TEXT("tool-a")));
		FScopedMonolithExecutionContext ScopeA(ContextA);
		Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());

		TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_IsolationA"));
		SetParams->SetStringField(TEXT("widget_name"), TEXT("OnlyInA"));
		SetParams->SetStringField(TEXT("scope"), TEXT("session"));
		const FMonolithActionResult SetResult = Registry.ExecuteAction(TEXT("ui"), TEXT("set_widget_context"), SetParams);
		TestTrue(TEXT("set in session A succeeds"), SetResult.bSuccess);
	}

	{
		FMonolithExecutionContext ContextB(MakeTestContextParams(TEXT("ui-context-isolation-b"), TEXT("tool-b")));
		FScopedMonolithExecutionContext ScopeB(ContextB);
		TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
		GetParams->SetStringField(TEXT("scope"), TEXT("session"));
		const FMonolithActionResult GetResult = Registry.ExecuteAction(TEXT("ui"), TEXT("get_widget_context"), GetParams);
		if (!TestTrue(TEXT("get in session B succeeds"), GetResult.bSuccess && GetResult.Result.IsValid()))
		{
			return false;
		}
		TestFalse(TEXT("session B does not see session A context"), GetResult.Result->GetBoolField(TEXT("has_context")));
		Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());
	}

	{
		FMonolithExecutionContext ContextA(MakeTestContextParams(TEXT("ui-context-isolation-a"), TEXT("tool-a-cleanup")));
		FScopedMonolithExecutionContext ScopeA(ContextA);
		Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIContextRequestScopeDoesNotLeakTest,
	"MonolithUI.Context.RequestScopeDoesNotLeak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIContextRequestScopeDoesNotLeakTest::RunTest(const FString& /*Parameters*/)
{
	EnsureContextActionsRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	{
		FMonolithExecutionContext RequestA(MakeTestContextParams(TEXT("ui-context-request"), TEXT("tool-request-a")));
		FScopedMonolithExecutionContext ScopeA(RequestA);
		Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());

		TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_RequestScoped"));
		SetParams->SetStringField(TEXT("widget_name"), TEXT("RequestOnly"));
		SetParams->SetStringField(TEXT("scope"), TEXT("request"));
		const FMonolithActionResult SetResult = Registry.ExecuteAction(TEXT("ui"), TEXT("set_widget_context"), SetParams);
		TestTrue(TEXT("set request context succeeds"), SetResult.bSuccess);

		TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
		GetParams->SetStringField(TEXT("scope"), TEXT("request"));
		const FMonolithActionResult SameRequest = Registry.ExecuteAction(TEXT("ui"), TEXT("get_widget_context"), GetParams);
		TestTrue(TEXT("same request sees request context"), SameRequest.bSuccess && SameRequest.Result->GetBoolField(TEXT("has_context")));
	}

	{
		FMonolithExecutionContext RequestB(MakeTestContextParams(TEXT("ui-context-request"), TEXT("tool-request-b")));
		FScopedMonolithExecutionContext ScopeB(RequestB);
		TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
		GetParams->SetStringField(TEXT("scope"), TEXT("request"));
		const FMonolithActionResult OtherRequest = Registry.ExecuteAction(TEXT("ui"), TEXT("get_widget_context"), GetParams);
		if (!TestTrue(TEXT("other request get succeeds"), OtherRequest.bSuccess && OtherRequest.Result.IsValid()))
		{
			return false;
		}
		TestFalse(TEXT("request scope does not leak to next tool call"), OtherRequest.Result->GetBoolField(TEXT("has_context")));
		Registry.ExecuteAction(TEXT("ui"), TEXT("clear_widget_context"), MakeClearParams());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
