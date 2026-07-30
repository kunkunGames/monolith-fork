#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUICommon.h"
#include "MonolithUISlotActions.h"
#include "Registry/MonolithUIRegistrySubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIRenameWidgetAcceptsAliasTest, "Monolith.Registry.UI.RenameWidgetAcceptsAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIRenameWidgetAcceptsAliasTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("ui")))
	{
		if (ActionInfo.Action == TEXT("rename_widget"))
		{
			Schema = ActionInfo.ParamSchema;
			break;
		}
	}

	if (!Schema.IsValid())
	{
		AddError(TEXT("Action ui.rename_widget schema not found in registry"));
		return false;
	}

	const TSharedPtr<FJsonObject>* WbpPathParam = nullptr;
	if (!Schema->TryGetObjectField(TEXT("wbp_path"), WbpPathParam) || !WbpPathParam || !WbpPathParam->IsValid())
	{
		AddError(TEXT("Action schema has no wbp_path param"));
		return false;
	}

	bool bHasAssetPathAlias = false;
	const TArray<TSharedPtr<FJsonValue>>* AliasesArray = nullptr;
	if ((*WbpPathParam)->TryGetArrayField(TEXT("aliases"), AliasesArray) && AliasesArray)
	{
		for (const TSharedPtr<FJsonValue>& AliasVal : *AliasesArray)
		{
			if (AliasVal.IsValid() && AliasVal->AsString() == TEXT("asset_path"))
			{
				bHasAssetPathAlias = true;
				break;
			}
		}
	}

	TestTrue(TEXT("wbp_path param exists"), WbpPathParam && WbpPathParam->IsValid());
	TestTrue(TEXT("wbp_path has asset_path alias"), bHasAssetPathAlias);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUISetSlotPropertyBoxSizeSchemaTest,
	"Monolith.Registry.UI.SetSlotPropertyBoxSizeSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetSlotPropertyBoxSizeSchemaTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUISlotActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("ui")))
	{
		if (ActionInfo.Action == TEXT("set_slot_property"))
		{
			Schema = ActionInfo.ParamSchema;
			break;
		}
	}
	if (!TestTrue(TEXT("set_slot_property schema found"), Schema.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* SizeRuleParam = nullptr;
	if (!TestTrue(TEXT("size_rule schema exists"), Schema->TryGetObjectField(TEXT("size_rule"), SizeRuleParam) && SizeRuleParam && SizeRuleParam->IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("size_rule schema type"), (*SizeRuleParam)->GetStringField(TEXT("type")), TEXT("string"));
	const TArray<TSharedPtr<FJsonValue>>* SizeRuleEnum = nullptr;
	if (!TestTrue(TEXT("size_rule enum exists"), (*SizeRuleParam)->TryGetArrayField(TEXT("enum"), SizeRuleEnum) && SizeRuleEnum))
	{
		return false;
	}
	TSet<FString> SizeRuleTokens;
	for (const TSharedPtr<FJsonValue>& Value : *SizeRuleEnum)
	{
		if (Value.IsValid())
		{
			SizeRuleTokens.Add(Value->AsString());
		}
	}
	TestEqual(TEXT("size_rule enum has exactly two tokens"), SizeRuleTokens.Num(), 2);
	TestTrue(TEXT("size_rule enum contains Automatic"), SizeRuleTokens.Contains(TEXT("Automatic")));
	TestTrue(TEXT("size_rule enum contains Fill"), SizeRuleTokens.Contains(TEXT("Fill")));

	const TSharedPtr<FJsonObject>* FillWeightParam = nullptr;
	if (!TestTrue(TEXT("fill_weight schema exists"), Schema->TryGetObjectField(TEXT("fill_weight"), FillWeightParam) && FillWeightParam && FillWeightParam->IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("fill_weight schema type"), (*FillWeightParam)->GetStringField(TEXT("type")), TEXT("number"));
	double Minimum = -1.0;
	TestTrue(TEXT("fill_weight minimum exists"), (*FillWeightParam)->TryGetNumberField(TEXT("minimum"), Minimum));
	TestTrue(TEXT("fill_weight minimum is zero"), FMath::IsNearlyZero(Minimum));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIListWidgetTypesUsesLiveRegistryTest,
	"Monolith.Registry.UI.ListWidgetTypesUsesLiveRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIListWidgetTypesUsesLiveRegistryTest::RunTest(const FString& Parameters)
{
	UMonolithUIRegistrySubsystem* RegistrySubsystem = UMonolithUIRegistrySubsystem::Get();
	if (!TestNotNull(TEXT("UI registry subsystem is initialized"), RegistrySubsystem))
	{
		return false;
	}
	RegistrySubsystem->RescanWidgetTypes();
	TestNotNull(
		TEXT("core short token resolves through the live registry"),
		MonolithUI::WidgetClassFromName(TEXT("TextBlock")));
	TestNotNull(
		TEXT("exact native class path remains supported"),
		MonolithUI::WidgetClassFromName(TEXT("/Script/UMG.TextBlock")));
	TestNull(
		TEXT("unknown bare token fails closed"),
		MonolithUI::WidgetClassFromName(TEXT("DefinitelyNotAWidgetType")));
#if WITH_COMMONUI
	TestNotNull(
		TEXT("CommonUI short token resolves through the live registry"),
		MonolithUI::WidgetClassFromName(TEXT("CommonNumericTextBlock")));
#endif

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIActions::RegisterActions(Registry);

	const FMonolithActionResult AllResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("list_widget_types"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("unfiltered registry discovery succeeds"), AllResult.bSuccess && AllResult.Result.IsValid()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidFilterParams = MakeShared<FJsonObject>();
	InvalidFilterParams->SetStringField(TEXT("filter"), TEXT("definitely-not-a-category"));
	const FMonolithActionResult InvalidFilterResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("list_widget_types"), InvalidFilterParams);
	TestFalse(TEXT("unknown category filters fail closed"), InvalidFilterResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* AllRows = nullptr;
	if (!TestTrue(
			TEXT("unfiltered discovery returns widget_types"),
			AllResult.Result->TryGetArrayField(TEXT("widget_types"), AllRows) && AllRows))
	{
		return false;
	}
	TestEqual(
		TEXT("unfiltered count matches the live type registry"),
		AllRows->Num(),
		RegistrySubsystem->GetTypeRegistry().Num());
	TestEqual(
		TEXT("total_registered matches the live type registry"),
		static_cast<int32>(AllResult.Result->GetNumberField(TEXT("total_registered"))),
		RegistrySubsystem->GetTypeRegistry().Num());

	TSet<FString> SeenTokens;
	FString PreviousToken;
	bool bFoundTextBlock = false;
#if WITH_COMMONUI
	bool bFoundCommonNumericText = false;
#endif
	for (const TSharedPtr<FJsonValue>& RowValue : *AllRows)
	{
		const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
		if (!TestTrue(TEXT("widget type row is an object"), Row.IsValid()))
		{
			return false;
		}

		const FString Token = Row->GetStringField(TEXT("name"));
		TestFalse(TEXT("widget type tokens are unique"), SeenTokens.Contains(Token));
		SeenTokens.Add(Token);
		if (!PreviousToken.IsEmpty())
		{
			TestTrue(TEXT("widget type rows are token-sorted"), PreviousToken < Token);
		}
		PreviousToken = Token;

		TestTrue(TEXT("widget type row exposes class_path"), Row->HasTypedField<EJson::String>(TEXT("class_path")));
		TestTrue(TEXT("widget type row exposes module"), Row->HasTypedField<EJson::String>(TEXT("module")));
		TestTrue(TEXT("widget type row exposes container_kind"), Row->HasTypedField<EJson::String>(TEXT("container_kind")));
		TestTrue(TEXT("widget type row exposes max_children"), Row->HasTypedField<EJson::Number>(TEXT("max_children")));

		if (Token == TEXT("TextBlock"))
		{
			bFoundTextBlock = true;
			TestEqual(TEXT("TextBlock is a display widget"), Row->GetStringField(TEXT("category")), TEXT("display"));
			TestEqual(TEXT("TextBlock belongs to UMG"), Row->GetStringField(TEXT("module")), TEXT("UMG"));
		}
#if WITH_COMMONUI
		if (Token == TEXT("CommonNumericTextBlock"))
		{
			bFoundCommonNumericText = true;
			TestEqual(
				TEXT("CommonNumericTextBlock inherits the display category"),
				Row->GetStringField(TEXT("category")),
				TEXT("display"));
			TestEqual(
				TEXT("CommonNumericTextBlock reports its owning module"),
				Row->GetStringField(TEXT("module")),
				TEXT("CommonUI"));
		}
#endif
	}
	TestTrue(TEXT("core UMG TextBlock is discoverable"), bFoundTextBlock);
#if WITH_COMMONUI
	TestTrue(TEXT("loaded CommonUI types are discoverable"), bFoundCommonNumericText);
#endif

	TSharedPtr<FJsonObject> CommonDisplayParams = MakeShared<FJsonObject>();
	CommonDisplayParams->SetStringField(TEXT("filter"), TEXT("DISPLAY"));
	CommonDisplayParams->SetStringField(TEXT("module_filter"), TEXT("commonui"));
	const FMonolithActionResult CommonDisplayResult =
		Registry.ExecuteAction(TEXT("ui"), TEXT("list_widget_types"), CommonDisplayParams);
	if (!TestTrue(
		TEXT("case-insensitive CommonUI display filter succeeds"),
		CommonDisplayResult.bSuccess && CommonDisplayResult.Result.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CommonDisplayRows = nullptr;
	if (!TestTrue(
		TEXT("CommonUI display filter returns widget_types"),
		CommonDisplayResult.Result->TryGetArrayField(TEXT("widget_types"), CommonDisplayRows)
			&& CommonDisplayRows))
	{
		return false;
	}
#if WITH_COMMONUI
	TestTrue(TEXT("CommonUI display filter is non-empty"), CommonDisplayRows->Num() > 0);
#endif
	for (const TSharedPtr<FJsonValue>& RowValue : *CommonDisplayRows)
	{
		const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
		if (!TestTrue(TEXT("filtered widget type row is an object"), Row.IsValid()))
		{
			return false;
		}
		TestEqual(TEXT("category filter is exact"), Row->GetStringField(TEXT("category")), TEXT("display"));
		TestTrue(
			TEXT("module filter is case-insensitive"),
			Row->GetStringField(TEXT("module")).Contains(TEXT("commonui"), ESearchCase::IgnoreCase));
	}

	return true;
}
