// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithBlueprintNodeActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithBlueprintResolveNodeParamTestDetail
{
	struct FInvalidStringCase
	{
		FString NodeType;
		FString Field;
		TSharedPtr<FJsonValue> Value;
	};

	static TSharedPtr<FJsonValue> MakeEmptyArray()
	{
		return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{});
	}

	static TSharedPtr<FJsonValue> MakeEmptyObject()
	{
		return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
	}

	static TSharedPtr<FJsonObject> MakeParams(const FInvalidStringCase& TestCase)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (TestCase.Field != TEXT("node_type"))
		{
			Params->SetStringField(TEXT("node_type"), TestCase.NodeType);
		}
		Params->SetField(TestCase.Field, TestCase.Value);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintResolveNodeStrictStringParamsTest,
	"Monolith.Blueprint.ResolveNode.StrictStringParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintResolveNodeStrictStringParamsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBlueprintResolveNodeParamTestDetail;

	const TArray<FInvalidStringCase> Cases = {
		{ TEXT(""),                    TEXT("node_type"),              MakeShared<FJsonValueNumber>(42.0) },
		{ TEXT("CallFunction"),        TEXT("function_name"),          MakeShared<FJsonValueNumber>(42.0) },
		{ TEXT("CallFunction"),        TEXT("target_class"),           MakeShared<FJsonValueBoolean>(true) },
		{ TEXT("CallFunction"),        TEXT("asset_path"),             MakeEmptyArray() },
		{ TEXT("SwitchOnEnum"),        TEXT("enum_type"),              MakeEmptyObject() },
		{ TEXT("VariableGet"),         TEXT("variable_name"),          MakeShared<FJsonValueNumber>(42.0) },
		{ TEXT("CustomEvent"),         TEXT("replication"),            MakeEmptyArray() },
		{ TEXT("CustomEvent"),         TEXT("event_name"),             MakeShared<FJsonValueBoolean>(true) },
		{ TEXT("MacroInstance"),       TEXT("macro_name"),             MakeShared<FJsonValueNumber>(42.0) },
		{ TEXT("MacroInstance"),       TEXT("macro_blueprint"),        MakeEmptyObject() },
		{ TEXT("ComponentBoundEvent"), TEXT("component_name"),         MakeShared<FJsonValueNumber>(42.0) },
		{ TEXT("ComponentBoundEvent"), TEXT("delegate_property_name"), MakeShared<FJsonValueBoolean>(true) }
	};

	bool bPassed = true;
	for (const FInvalidStringCase& TestCase : Cases)
	{
		const FMonolithActionResult Result =
			FMonolithBlueprintNodeActions::HandleResolveNode(MakeParams(TestCase));
		const FString Context = FString::Printf(
			TEXT("%s rejects a non-string %s"),
			*TestCase.NodeType,
			*TestCase.Field);

		bPassed &= TestFalse(*Context, Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("%s uses invalid-params"), *Context),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("%s identifies the field"), *Context),
			Result.ErrorMessage.Contains(TestCase.Field));
	}

	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintResolveNodeStrictStringDispatchTest,
	"Monolith.Blueprint.ResolveNode.StrictStringDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintResolveNodeStrictStringDispatchTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!TestTrue(
		TEXT("blueprint.resolve_node is registered"),
		Registry.HasAction(TEXT("blueprint"), TEXT("resolve_node"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	InvalidParams->SetNumberField(TEXT("function_name"), 42.0);

	const FMonolithActionResult InvalidResult = Registry.ExecuteAction(
		TEXT("blueprint"),
		TEXT("resolve_node"),
		InvalidParams);
	TestFalse(TEXT("registry dispatch rejects malformed function_name"), InvalidResult.bSuccess);
	TestEqual(
		TEXT("registry dispatch preserves invalid-params code"),
		InvalidResult.ErrorCode,
		FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(
		TEXT("registry dispatch identifies function_name"),
		InvalidResult.ErrorMessage.Contains(TEXT("function_name")));

	TSharedPtr<FJsonObject> ValidParams = MakeShared<FJsonObject>();
	ValidParams->SetStringField(TEXT("node_type"), TEXT("Branch"));
	const FMonolithActionResult ValidResult = Registry.ExecuteAction(
		TEXT("blueprint"),
		TEXT("resolve_node"),
		ValidParams);
	TestTrue(TEXT("valid Branch preview still succeeds"), ValidResult.bSuccess);
	if (TestTrue(TEXT("valid Branch preview has a payload"), ValidResult.Result.IsValid()))
	{
		FString ResolvedType;
		TestTrue(
			TEXT("valid Branch preview reports resolved_type"),
			ValidResult.Result->TryGetStringField(TEXT("resolved_type"), ResolvedType));
		TestEqual(TEXT("resolved_type remains Branch"), ResolvedType, FString(TEXT("Branch")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
