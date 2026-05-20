#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithTestSupport.h"
#include "MonolithWorldConditionsActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldConditionsTypedParamsTest, "Monolith.ParamValidation.MonolithWorldConditions.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldConditionsTypedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(TEXT("world_conditions"));

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("world_conditions"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithWorldConditionsActions::RegisterActions(Registry);
		},
		{
			{ TEXT("get_status"), true, TEXT("world_conditions.get_status is registered") },
			{ TEXT("list_query_owners"), true, TEXT("world_conditions.list_query_owners is registered") },
			{ TEXT("describe_query"), true, TEXT("world_conditions.describe_query is registered") },
			{ TEXT("describe_condition_types"), true, TEXT("world_conditions.describe_condition_types is registered") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("world_conditions"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithWorldConditionsActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("list_query_owners"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("path_filter"), 42.0);
				},
				TEXT("path_filter"),
				TEXT("world_conditions.list_query_owners rejects non-string path_filter")
			},
			{
				TEXT("list_query_owners"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 501.0);
				},
				TEXT("limit"),
				TEXT("world_conditions.list_query_owners rejects limit above range")
			},
			{
				TEXT("describe_query"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 42.0);
				},
				TEXT("asset_path"),
				TEXT("world_conditions.describe_query rejects non-string asset_path")
			},
			{
				TEXT("describe_query"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/SO_Test.SO_Test"));
					Params->SetStringField(TEXT("query"), TEXT("bogus"));
				},
				TEXT("query"),
				TEXT("world_conditions.describe_query rejects unknown query")
			},
			{
				TEXT("describe_query"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/SO_Test.SO_Test"));
					Params->SetStringField(TEXT("query"), TEXT("slot_selection_preconditions"));
					Params->SetStringField(TEXT("slot_index"), TEXT("0"));
				},
				TEXT("slot_index"),
				TEXT("world_conditions.describe_query rejects non-integer slot_index")
			},
			{
				TEXT("describe_condition_types"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 501.0);
				},
				TEXT("limit"),
				TEXT("world_conditions.describe_condition_types rejects limit above range")
			}
		});

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
