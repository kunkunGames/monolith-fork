#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUISlotActions.h"

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
