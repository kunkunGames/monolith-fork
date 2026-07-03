// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUICommon.h"
#include "MonolithUIRegistryActions.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Hoisted/MonolithUITestFixtureUtils.h"

#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

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

	bool ArrayContainsStringWithSubstring(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		const FString& ExpectedSubstring)
	{
		if (!Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Actual;
			if (Value.IsValid() && Value->TryGetString(Actual) && Actual.Contains(ExpectedSubstring))
			{
				return true;
			}
		}
		return false;
	}

	bool GetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Obj.IsValid())
		{
			Obj->TryGetBoolField(FieldName, bValue);
		}
		return bValue;
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
	FMonolithUIDescribeWidgetTypeSchemaImageTest,
	"MonolithUI.Registry.DescribeWidgetTypeSchema.Image",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIDescribeWidgetTypeSchemaImageTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIRegistryActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("widget_class"), TEXT("Image"));
	Params->SetBoolField(TEXT("include_unsafe"), true);
	Params->SetBoolField(TEXT("include_inherited"), false);

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), Params);
	if (!TestTrue(TEXT("describe_widget_type_schema succeeds"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("widget_token"), Result.Result->GetStringField(TEXT("widget_token")), TEXT("Image"));
	TestFalse(TEXT("include_inherited is honoured"), Result.Result->GetBoolField(TEXT("include_inherited")));

	const double LocalPropertyCount = Result.Result->GetNumberField(TEXT("property_count"));
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties array exists"), Result.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties && Properties->Num() > 0);

	TSharedPtr<FJsonObject> BrushProperty;
	TestTrue(TEXT("Image Brush property is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("Brush"), &BrushProperty));
	if (BrushProperty.IsValid())
	{
		TestEqual(TEXT("Brush is allowlisted"), BrushProperty->GetStringField(TEXT("allowlist_status")), TEXT("allowed"));
		TestTrue(TEXT("Brush has cpp_type field"), BrushProperty->HasField(TEXT("cpp_type")));
	}

	TSharedPtr<FJsonObject> TintProperty;
	TestTrue(TEXT("Image ColorAndOpacity property is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("ColorAndOpacity"), &TintProperty));
	if (TintProperty.IsValid())
	{
		TestEqual(TEXT("ColorAndOpacity is allowlisted"), TintProperty->GetStringField(TEXT("allowlist_status")), TEXT("allowed"));
	}

	TSharedPtr<FJsonObject> UnsafeFlipProperty;
	TestTrue(TEXT("unsafe raw-mode-only Image property is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("bFlipForRightToLeftFlowDirection"), &UnsafeFlipProperty));
	if (UnsafeFlipProperty.IsValid())
	{
		TestEqual(TEXT("unsafe Image property requires raw mode"),
			UnsafeFlipProperty->GetStringField(TEXT("allowlist_status")), TEXT("requires_raw_mode"));
		TestFalse(TEXT("unsafe Image property is not advertised as settable"),
			GetBoolField(UnsafeFlipProperty, TEXT("settable"), true));
	}

	TSharedPtr<FJsonObject> InheritedParams = MakeShared<FJsonObject>();
	InheritedParams->SetStringField(TEXT("widget_class"), TEXT("Image"));
	InheritedParams->SetBoolField(TEXT("include_unsafe"), true);
	InheritedParams->SetBoolField(TEXT("include_inherited"), true);
	const FMonolithActionResult InheritedResult = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), InheritedParams);
	if (TestTrue(TEXT("describe_widget_type_schema include_inherited succeeds"), InheritedResult.bSuccess && InheritedResult.Result.IsValid()))
	{
		TestTrue(TEXT("include_inherited expands unsafe reflected properties"),
			InheritedResult.Result->GetNumberField(TEXT("property_count")) > LocalPropertyCount);
	}

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
	Params->SetBoolField(TEXT("include_unsafe"), true);

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

	TSharedPtr<FJsonObject> StyleProperty;
	TestTrue(TEXT("public Style alias is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("Style"), &StyleProperty));
	if (StyleProperty.IsValid())
	{
		TestEqual(TEXT("Style is allowlisted"), StyleProperty->GetStringField(TEXT("allowlist_status")), TEXT("allowed"));
	}

	TestFalse(TEXT("raw WidgetStyle engine alias is suppressed"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("WidgetStyle")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIDescribeWidgetTypeSchemaLiveSlotTest,
	"MonolithUI.Registry.DescribeWidgetTypeSchema.LiveSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIDescribeWidgetTypeSchemaLiveSlotTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), UMonolithUIRegistrySubsystem::Get()))
	{
		return false;
	}

	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_DescribeSchemaLiveSlot");
	FString Error;
	UWidget* ChildWidget = nullptr;
	if (!TestTrue(TEXT("fixture WBP created"),
		MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
			AssetPath,
			TEXT("SchemaImage"),
			UImage::StaticClass(),
			Error,
			&ChildWidget)))
	{
		AddError(Error);
		return false;
	}

	FMonolithActionResult LoadErr;
	UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, LoadErr);
	if (!TestNotNull(TEXT("fixture WBP load succeeds"), WBP))
	{
		return false;
	}
	UCanvasPanel* RootCanvas = WBP && WBP->WidgetTree
		? Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget)
		: nullptr;
	if (!TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas))
	{
		return false;
	}

	UVerticalBox* SchemaVBox = WBP->WidgetTree->ConstructWidget<UVerticalBox>(
		UVerticalBox::StaticClass(),
		TEXT("SchemaVBox"));
	UTextBlock* SchemaVBoxLabel = WBP->WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		TEXT("SchemaVBoxLabel"));
	UBorder* SchemaFullBorder = WBP->WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(),
		TEXT("SchemaFullBorder"));
	UTextBlock* SchemaBorderLabel = WBP->WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		TEXT("SchemaBorderLabel"));
	if (!TestNotNull(TEXT("VerticalBox fixture widget"), SchemaVBox)
		|| !TestNotNull(TEXT("VerticalBox label fixture widget"), SchemaVBoxLabel)
		|| !TestNotNull(TEXT("Border fixture widget"), SchemaFullBorder)
		|| !TestNotNull(TEXT("Border label fixture widget"), SchemaBorderLabel))
	{
		return false;
	}
	RootCanvas->AddChild(SchemaVBox);
	SchemaVBox->AddChild(SchemaVBoxLabel);
	RootCanvas->AddChild(SchemaFullBorder);
	SchemaFullBorder->AddChild(SchemaBorderLabel);
	MonolithUI::RegisterCreatedWidget(WBP, SchemaVBox);
	MonolithUI::RegisterCreatedWidget(WBP, SchemaVBoxLabel);
	MonolithUI::RegisterCreatedWidget(WBP, SchemaFullBorder);
	MonolithUI::RegisterCreatedWidget(WBP, SchemaBorderLabel);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIActions::RegisterActions(Registry);
	FMonolithUIRegistryActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("widget_name"), TEXT("SchemaImage"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), Params);
	if (!TestTrue(TEXT("describe live widget succeeds"), Result.bSuccess && Result.Result.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("resolved_from"), Result.Result->GetStringField(TEXT("resolved_from")), TEXT("live_widget"));
	TestEqual(TEXT("widget_token"), Result.Result->GetStringField(TEXT("widget_token")), TEXT("Image"));
	TestTrue(TEXT("live slot is CanvasPanelSlot"),
		Result.Result->GetStringField(TEXT("live_slot_class")).Contains(TEXT("CanvasPanelSlot")));

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties array exists"), Result.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties && Properties->Num() > 0);

	TSharedPtr<FJsonObject> PositionProperty;
	TestTrue(TEXT("Canvas Slot.Position is described"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("Slot.Position"), &PositionProperty));
	if (PositionProperty.IsValid())
	{
		TestTrue(TEXT("Slot.Position is settable for CanvasPanelSlot"), GetBoolField(PositionProperty, TEXT("settable")));
	}

	TSharedPtr<FJsonObject> PaddingProperty;
	TestTrue(TEXT("Box Slot.Padding is still discoverable"),
		ArrayContainsObjectWithString(Properties, TEXT("path"), TEXT("Slot.Padding"), &PaddingProperty));
	if (PaddingProperty.IsValid())
	{
		TestFalse(TEXT("Slot.Padding is blocked for CanvasPanelSlot"), GetBoolField(PaddingProperty, TEXT("settable"), true));
		TestTrue(TEXT("Slot.Padding reports incompatible slot context"),
			PaddingProperty->GetStringField(TEXT("slot_context")).Contains(TEXT("blocked for the supplied live widget slot class")));
		TestTrue(TEXT("Slot.Padding reports blocked_reason"), PaddingProperty->HasField(TEXT("blocked_reason")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	TestTrue(TEXT("warnings array exists"), Result.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);
	TestTrue(TEXT("incompatible live slot warning is surfaced"),
		ArrayContainsStringWithSubstring(Warnings, TEXT("Slot.* path(s) are not settable")));

	TSharedPtr<FJsonObject> RootParams = MakeShared<FJsonObject>();
	RootParams->SetStringField(TEXT("asset_path"), AssetPath);
	RootParams->SetStringField(TEXT("widget_name"), TEXT("RootCanvas"));
	const FMonolithActionResult RootResult = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), RootParams);
	if (!TestTrue(TEXT("describe root widget succeeds"), RootResult.bSuccess && RootResult.Result.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("root widget has no live slot class"), RootResult.Result->GetStringField(TEXT("live_slot_class")), FString());
	const TArray<TSharedPtr<FJsonValue>>* RootWarnings = nullptr;
	TestTrue(TEXT("root warnings array exists"), RootResult.Result->TryGetArrayField(TEXT("warnings"), RootWarnings) && RootWarnings);
	TestTrue(TEXT("root no-slot warning is surfaced"),
		ArrayContainsStringWithSubstring(RootWarnings, TEXT("Live widget has no parent slot")));

	const TArray<TSharedPtr<FJsonValue>>* RootProperties = nullptr;
	TestTrue(TEXT("root properties array exists"),
		RootResult.Result->TryGetArrayField(TEXT("properties"), RootProperties) && RootProperties && RootProperties->Num() > 0);
	TSharedPtr<FJsonObject> RootPositionProperty;
	TestTrue(TEXT("root Slot.Position is described as contextual"),
		ArrayContainsObjectWithString(RootProperties, TEXT("path"), TEXT("Slot.Position"), &RootPositionProperty));
	if (RootPositionProperty.IsValid())
	{
		TestFalse(TEXT("root Slot.Position is not settable"), GetBoolField(RootPositionProperty, TEXT("settable"), true));
	}

	TSharedPtr<FJsonObject> VerticalSlotParams = MakeShared<FJsonObject>();
	VerticalSlotParams->SetStringField(TEXT("asset_path"), AssetPath);
	VerticalSlotParams->SetStringField(TEXT("widget_name"), TEXT("SchemaVBoxLabel"));
	const FMonolithActionResult VerticalSlotResult = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), VerticalSlotParams);
	if (!TestTrue(TEXT("describe VerticalBox child succeeds"), VerticalSlotResult.bSuccess && VerticalSlotResult.Result.IsValid()))
	{
		return false;
	}

	TestTrue(TEXT("live slot is VerticalBoxSlot"),
		VerticalSlotResult.Result->GetStringField(TEXT("live_slot_class")).Contains(TEXT("VerticalBoxSlot")));
	const TArray<TSharedPtr<FJsonValue>>* VerticalSlotProperties = nullptr;
	TestTrue(TEXT("vertical slot properties array exists"),
		VerticalSlotResult.Result->TryGetArrayField(TEXT("properties"), VerticalSlotProperties)
		&& VerticalSlotProperties
		&& VerticalSlotProperties->Num() > 0);

	TSharedPtr<FJsonObject> VerticalPaddingProperty;
	TestTrue(TEXT("VerticalBox Slot.Padding is described"),
		ArrayContainsObjectWithString(VerticalSlotProperties, TEXT("path"), TEXT("Slot.Padding"), &VerticalPaddingProperty));
	if (VerticalPaddingProperty.IsValid())
	{
		TestTrue(TEXT("Slot.Padding is settable for VerticalBoxSlot"),
			GetBoolField(VerticalPaddingProperty, TEXT("settable")));
	}

	TSharedPtr<FJsonObject> VerticalPositionProperty;
	TestTrue(TEXT("Canvas Slot.Position remains discoverable for VerticalBoxSlot"),
		ArrayContainsObjectWithString(VerticalSlotProperties, TEXT("path"), TEXT("Slot.Position"), &VerticalPositionProperty));
	if (VerticalPositionProperty.IsValid())
	{
		TestFalse(TEXT("Slot.Position is blocked for VerticalBoxSlot"),
			GetBoolField(VerticalPositionProperty, TEXT("settable"), true));
		TestTrue(TEXT("Slot.Position reports blocked_reason for VerticalBoxSlot"),
			VerticalPositionProperty->HasField(TEXT("blocked_reason")));
	}

	TSharedPtr<FJsonObject> BorderCapacityParams = MakeShared<FJsonObject>();
	BorderCapacityParams->SetStringField(TEXT("asset_path"), AssetPath);
	BorderCapacityParams->SetStringField(TEXT("widget_name"), TEXT("SchemaFullBorder"));
	const FMonolithActionResult BorderCapacityResult = Registry.ExecuteAction(TEXT("ui"), TEXT("describe_widget_type_schema"), BorderCapacityParams);
	if (!TestTrue(TEXT("describe full single-child Border succeeds"), BorderCapacityResult.bSuccess && BorderCapacityResult.Result.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("Border reports max_children=1"), BorderCapacityResult.Result->GetNumberField(TEXT("max_children")), 1.0);
	TestEqual(TEXT("Border live_child_count=1"), BorderCapacityResult.Result->GetNumberField(TEXT("live_child_count")), 1.0);
	TestFalse(TEXT("full Border cannot add another child"),
		BorderCapacityResult.Result->GetBoolField(TEXT("live_can_add_child")));
	const TSharedPtr<FJsonObject>* ChildCapacity = nullptr;
	TestTrue(TEXT("live_child_capacity object exists"),
		BorderCapacityResult.Result->TryGetObjectField(TEXT("live_child_capacity"), ChildCapacity) && ChildCapacity && ChildCapacity->IsValid());
	if (ChildCapacity && ChildCapacity->IsValid())
	{
		TestEqual(TEXT("single-child capacity context"),
			(*ChildCapacity)->GetStringField(TEXT("capacity_context")),
			TEXT("single-child container is full"));
		TestEqual(TEXT("blocking_child is reported"),
			(*ChildCapacity)->GetStringField(TEXT("blocking_child")),
			TEXT("SchemaBorderLabel"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
