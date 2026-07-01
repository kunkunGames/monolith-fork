// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUIRegistryActions.h"
#include "Registry/MonolithUIRegistrySubsystem.h"

namespace
{
	bool ArrayContainsObjectWithString(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		const FString& FieldName,
		const FString& ExpectedValue,
		TSharedPtr<FJsonObject>* OutObject = nullptr)
	{
		if (!Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}

			FString Actual;
			if (Obj->TryGetStringField(FieldName, Actual) && Actual == ExpectedValue)
			{
				if (OutObject)
				{
					*OutObject = Obj;
				}
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIDescribeWidgetTypeSchemaTextBlockTest,
	"MonolithUI.Registry.DescribeWidgetTypeSchema.TextBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIDescribeWidgetTypeSchemaTextBlockTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUIRegistryActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), Params);
	if (!TestTrue(TEXT("describe_widget_type_schema succeeds"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("schema_version"), Result.Result->GetStringField(TEXT("schema_version")), TEXT("ui_widget_type_schema.v1"));
	TestEqual(TEXT("widget_token"), Result.Result->GetStringField(TEXT("widget_token")), TEXT("TextBlock"));
	TestTrue(TEXT("registered"), Result.Result->GetBoolField(TEXT("registered")));

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties array exists"), Result.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties && Properties->Num() > 0);

	TSharedPtr<FJsonObject> TextProperty;
	TestTrue(TEXT("Text property is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("Text"), &TextProperty));
	if (TextProperty.IsValid())
	{
		TestEqual(TEXT("Text is allowlisted"), TextProperty->GetStringField(TEXT("allowlist_status")), TEXT("allowed"));
		TestTrue(TEXT("Text has cpp_type field"), TextProperty->HasField(TEXT("cpp_type")));
	}

	const TArray<TSharedPtr<FJsonValue>>* NextActions = nullptr;
	TestTrue(TEXT("next_actions array exists"), Result.Result->TryGetArrayField(TEXT("next_actions"), NextActions) && NextActions);
	TestTrue(TEXT("next action points to set_widget_property"),
		ArrayContainsObjectWithString(NextActions, TEXT("tool"), TEXT("ui.set_widget_property")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIDescribeWidgetTypeSchemaButtonEnumTest,
	"MonolithUI.Registry.DescribeWidgetTypeSchema.ButtonEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIDescribeWidgetTypeSchemaButtonEnumTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIRegistryActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("widget_class"), TEXT("Button"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), Params);
	if (!TestTrue(TEXT("describe_widget_type_schema succeeds"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties array exists"), Result.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties && Properties->Num() > 0);

	TSharedPtr<FJsonObject> ClickMethodProperty;
	TestTrue(TEXT("ClickMethod property is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("ClickMethod"), &ClickMethodProperty));
	if (ClickMethodProperty.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		TestTrue(TEXT("ClickMethod enum_values array exists"),
			ClickMethodProperty->TryGetArrayField(TEXT("enum_values"), EnumValues) && EnumValues && EnumValues->Num() > 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
