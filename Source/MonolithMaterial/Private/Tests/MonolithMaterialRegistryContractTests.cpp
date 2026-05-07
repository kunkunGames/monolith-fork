#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithMaterialActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialConnectExpressionsAcceptsAliasTest, "Monolith.Registry.Material.ConnectExpressionsAcceptsAlias", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialConnectExpressionsAcceptsAliasTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.Clear();
	FMonolithMaterialActions::RegisterActions(Registry);

	auto Schema = Registry.GetActionSchema(TEXT("material"), TEXT("connect_expressions"));
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
