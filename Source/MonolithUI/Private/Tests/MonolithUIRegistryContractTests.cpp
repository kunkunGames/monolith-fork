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
	if (!Registry.GetActionSchema(TEXT("ui"), TEXT("rename_widget"), Schema))
	{
		AddError(TEXT("Action ui.rename_widget not found in registry"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!Schema->TryGetArrayField(TEXT("params"), ParamsArray))
	{
		AddError(TEXT("Action schema has no params array"));
		return false;
	}

	bool bFoundWbpPath = false;
	bool bHasAssetPathAlias = false;

	for (const TSharedPtr<FJsonValue>& ParamVal : *ParamsArray)
	{
		const TSharedPtr<FJsonObject> ParamObj = ParamVal->AsObject();
		if (!ParamObj.IsValid()) continue;

		FString ParamName;
		if (ParamObj->TryGetStringField(TEXT("name"), ParamName) && ParamName == TEXT("wbp_path"))
		{
			bFoundWbpPath = true;

			const TArray<TSharedPtr<FJsonValue>>* AliasesArray = nullptr;
			if (ParamObj->TryGetArrayField(TEXT("aliases"), AliasesArray))
			{
				for (const TSharedPtr<FJsonValue>& AliasVal : *AliasesArray)
				{
					if (AliasVal->AsString() == TEXT("asset_path"))
					{
						bHasAssetPathAlias = true;
						break;
					}
				}
			}
		}
	}

	TestTrue(TEXT("wbp_path param exists"), bFoundWbpPath);
	TestTrue(TEXT("wbp_path has asset_path alias"), bHasAssetPathAlias);

	return true;
}
