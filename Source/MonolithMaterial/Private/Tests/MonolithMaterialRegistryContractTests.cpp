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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRefreshCopiedGraphsSchemaTest, "Monolith.Registry.Material.RefreshCopiedMaterialGraphsSchema", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRefreshCopiedGraphsSchemaTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("material"), TEXT("refresh_copied_material_graphs")))
	{
		FMonolithMaterialActions::RegisterActions(Registry);
	}

	TestTrue(TEXT("material.refresh_copied_material_graphs action is registered"),
		Registry.HasAction(TEXT("material"), TEXT("refresh_copied_material_graphs")));
	TestEqual(TEXT("refresh_copied_material_graphs is guarded mutating"),
		Registry.GetActionExecutionPolicy(TEXT("material"), TEXT("refresh_copied_material_graphs")).PolicyId,
		FString(TEXT("transaction_optional")));

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("material")))
	{
		if (Info.Action == TEXT("refresh_copied_material_graphs"))
		{
			Schema = Info.ParamSchema;
			break;
		}
	}

	TestNotNull(TEXT("Refresh schema should exist"), Schema.Get());
	if (Schema)
	{
		bool bValidateTypes = false;
		TestTrue(TEXT("Refresh schema should expose _validate_types"), Schema->TryGetBoolField(TEXT("_validate_types"), bValidateTypes));
		TestTrue(TEXT("Refresh schema should enable type validation"), bValidateTypes);

		const TSharedPtr<FJsonObject>* AssetPathParam = nullptr;
		TestTrue(TEXT("asset_path param should exist"), Schema->TryGetObjectField(TEXT("asset_path"), AssetPathParam) && AssetPathParam != nullptr);
		if (AssetPathParam && *AssetPathParam)
		{
			FString Kind;
			(*AssetPathParam)->TryGetStringField(TEXT("kind"), Kind);
			TestEqual(TEXT("asset_path should be tagged as AssetPath"), Kind, FString(TEXT("AssetPath")));
		}

		const TSharedPtr<FJsonObject>* PackageMapParam = nullptr;
		TestTrue(TEXT("package_map param should exist"), Schema->TryGetObjectField(TEXT("package_map"), PackageMapParam) && PackageMapParam != nullptr);
		if (PackageMapParam && *PackageMapParam)
		{
			FString Type;
			(*PackageMapParam)->TryGetStringField(TEXT("type"), Type);
			TestEqual(TEXT("package_map should accept object or array"), Type, FString(TEXT("object|array")));
		}

		const TSharedPtr<FJsonObject>* DryRunParam = nullptr;
		TestTrue(TEXT("dry_run param should exist"), Schema->TryGetObjectField(TEXT("dry_run"), DryRunParam) && DryRunParam != nullptr);
		if (DryRunParam && *DryRunParam)
		{
			FString DefaultValue;
			(*DryRunParam)->TryGetStringField(TEXT("default"), DefaultValue);
			TestEqual(TEXT("dry_run should default true"), DefaultValue, FString(TEXT("true")));
		}

		const TSharedPtr<FJsonObject>* ConfirmParam = nullptr;
		TestTrue(TEXT("confirm param should exist"), Schema->TryGetObjectField(TEXT("confirm"), ConfirmParam) && ConfirmParam != nullptr);
		if (ConfirmParam && *ConfirmParam)
		{
			FString DefaultValue;
			(*ConfirmParam)->TryGetStringField(TEXT("default"), DefaultValue);
			TestEqual(TEXT("confirm should default false"), DefaultValue, FString(TEXT("false")));
		}
	}

	return true;
}
