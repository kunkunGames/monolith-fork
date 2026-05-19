#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithMaterialActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialConnectExpressionsAcceptsAliasTest, "Monolith.Registry.Material.ConnectExpressionsAcceptsAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialConnectExpressionsAcceptsAliasTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("material")))
	{
		if (Info.Action == TEXT("connect_expressions"))
		{
			Schema = Info.ParamSchema;
			break;
		}
	}
	TestNotNull(TEXT("Schema should exist"), Schema.Get());
	if (Schema)
	{
		const TSharedPtr<FJsonObject>* FromOutputParam = nullptr;
		bool bFoundFrom = Schema->TryGetObjectField(TEXT("from_output"), FromOutputParam);
		TestTrue(TEXT("from_output param should exist"), bFoundFrom);

		if (bFoundFrom && *FromOutputParam)
		{
			const TArray<TSharedPtr<FJsonValue>>* AliasesArr = nullptr;
			bool bHasAliases = (*FromOutputParam)->TryGetArrayField(TEXT("aliases"), AliasesArr);
			TestTrue(TEXT("from_output should have aliases array"), bHasAliases);

			if (bHasAliases && AliasesArr)
			{
				bool bFoundAlias = false;
				for (const auto& Val : *AliasesArr)
				{
					if (Val->AsString() == TEXT("from_pin")) bFoundAlias = true;
				}
				TestTrue(TEXT("from_output should have from_pin alias"), bFoundAlias);
			}
		}

		const TSharedPtr<FJsonObject>* ToInputParam = nullptr;
		bool bFoundTo = Schema->TryGetObjectField(TEXT("to_input"), ToInputParam);
		TestTrue(TEXT("to_input param should exist"), bFoundTo);

		if (bFoundTo && *ToInputParam)
		{
			const TArray<TSharedPtr<FJsonValue>>* AliasesArr = nullptr;
			bool bHasAliases = (*ToInputParam)->TryGetArrayField(TEXT("aliases"), AliasesArr);
			TestTrue(TEXT("to_input should have aliases array"), bHasAliases);

			if (bHasAliases && AliasesArr)
			{
				bool bFoundAlias = false;
				for (const auto& Val : *AliasesArr)
				{
					if (Val->AsString() == TEXT("to_pin")) bFoundAlias = true;
				}
				TestTrue(TEXT("to_input should have to_pin alias"), bFoundAlias);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialPaper2DAssetRejectsUnsafePathTest, "Monolith.ParamGuard.MonolithMaterial.Paper2DAssetRejectsUnsafePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialPaper2DAssetRejectsUnsafePathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithMaterialActions::RegisterActions(Registry);

	TestTrue(TEXT("material.get_paper2d_asset should be registered"), Registry.HasAction(TEXT("material"), TEXT("get_paper2d_asset")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PaperSprite.uasset"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("material"), TEXT("get_paper2d_asset"), Params);
	TestFalse(TEXT("Unsafe filesystem paths should be rejected"), Result.bSuccess);
	TestTrue(TEXT("Error should name the /Game guard"), Result.ErrorMessage.Contains(TEXT("/Game")));

	return true;
}
