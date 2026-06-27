#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"

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
