#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithDataflowActions.h"

#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithToolRegistry& DataflowRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("dataflow"), TEXT("get_status")))
		{
			FMonolithDataflowActions::RegisterActions(Registry);
		}
		return Registry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataflowRegistryTest,
	"Monolith.Dataflow.RegistryAndSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = DataflowRegistry();
	const TArray<FString> ExpectedActions =
	{
		TEXT("get_status"),
		TEXT("list_assets"),
		TEXT("get_dataflow_graph"),
		TEXT("list_dataflow_node_types"),
		TEXT("get_dataflow_node_schema"),
		TEXT("validate_dataflow_graph"),
		TEXT("list_dataflow_variables"),
		TEXT("list_dataflow_comments")
	};

	for (const FString& Action : ExpectedActions)
	{
		TestTrue(
			*FString::Printf(TEXT("dataflow.%s is registered"), *Action),
			Registry.HasAction(TEXT("dataflow"), Action));
	}

	const TArray<FMonolithActionInfo> Actions =
		Registry.GetActions(TEXT("dataflow"));
	TestEqual(TEXT("dataflow registers exactly eight actions"), Actions.Num(), 8);
	for (const FMonolithActionInfo& Action : Actions)
	{
		TestTrue(
			*FString::Printf(TEXT("%s has a description"), *Action.Action),
			!Action.Description.IsEmpty());
		TestTrue(
			*FString::Printf(TEXT("%s has a parameter schema"), *Action.Action),
			Action.ParamSchema.IsValid());
	}

	const FMonolithActionInfo* GraphAction = Actions.FindByPredicate(
		[](const FMonolithActionInfo& Action)
		{
			return Action.Action == TEXT("get_dataflow_graph");
		});
	TestNotNull(TEXT("get_dataflow_graph action info exists"), GraphAction);
	if (GraphAction && GraphAction->ParamSchema.IsValid())
	{
		const TSharedPtr<FJsonObject>* AssetPathSchema = nullptr;
		TestTrue(
			TEXT("asset_path schema exists"),
			GraphAction->ParamSchema->TryGetObjectField(
				TEXT("asset_path"),
				AssetPathSchema)
				&& AssetPathSchema
				&& AssetPathSchema->IsValid());
		if (AssetPathSchema && AssetPathSchema->IsValid())
		{
			bool bRequired = false;
			(*AssetPathSchema)->TryGetBoolField(TEXT("required"), bRequired);
			TestTrue(TEXT("asset_path is required"), bRequired);
		}

		const TSharedPtr<FJsonObject>* NodeLimitSchema = nullptr;
		TestTrue(
			TEXT("node_limit schema exists"),
			GraphAction->ParamSchema->TryGetObjectField(
				TEXT("node_limit"),
				NodeLimitSchema)
				&& NodeLimitSchema
				&& NodeLimitSchema->IsValid());
		if (NodeLimitSchema && NodeLimitSchema->IsValid())
		{
			TestEqual(
				TEXT("node_limit minimum is discoverable"),
				static_cast<int32>((*NodeLimitSchema)->GetNumberField(TEXT("minimum"))),
				1);
			TestEqual(
				TEXT("node_limit maximum is discoverable"),
				static_cast<int32>((*NodeLimitSchema)->GetNumberField(TEXT("maximum"))),
				500);
		}
	}

	struct FExpectedRange
	{
		const TCHAR* Action;
		const TCHAR* Param;
		int32 Minimum;
		int32 Maximum;
	};
	const FExpectedRange ExpectedRanges[] =
	{
		{TEXT("list_assets"), TEXT("limit"), 1, 500},
		{TEXT("get_dataflow_graph"), TEXT("node_limit"), 1, 500},
		{TEXT("get_dataflow_graph"), TEXT("connection_limit"), 1, 5000},
		{TEXT("get_dataflow_graph"), TEXT("pin_limit"), 1, 500},
		{TEXT("get_dataflow_graph"), TEXT("property_limit"), 1, 500},
		{TEXT("list_dataflow_node_types"), TEXT("limit"), 1, 1000},
		{TEXT("list_dataflow_node_types"), TEXT("pin_limit"), 1, 500},
		{TEXT("get_dataflow_node_schema"), TEXT("pin_limit"), 1, 500},
		{TEXT("get_dataflow_node_schema"), TEXT("property_limit"), 1, 500},
		{TEXT("validate_dataflow_graph"), TEXT("node_scan_limit"), 1, 100000},
		{TEXT("validate_dataflow_graph"), TEXT("connection_scan_limit"), 1, 250000},
		{TEXT("validate_dataflow_graph"), TEXT("issue_limit"), 1, 1000},
		{TEXT("list_dataflow_variables"), TEXT("limit"), 1, 1000},
		{TEXT("list_dataflow_comments"), TEXT("comment_limit"), 1, 1000},
		{TEXT("list_dataflow_comments"), TEXT("node_limit"), 1, 500},
		{TEXT("list_dataflow_comments"), TEXT("graph_node_scan_limit"), 1, 50000}
	};
	for (const FExpectedRange& Expected : ExpectedRanges)
	{
		const FMonolithActionInfo* Action = Actions.FindByPredicate(
			[&Expected](const FMonolithActionInfo& Candidate)
			{
				return Candidate.Action == Expected.Action;
			});
		TestNotNull(
			*FString::Printf(
				TEXT("%s action exists for range validation"),
				Expected.Action),
			Action);
		if (!Action || !Action->ParamSchema.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ParamSchema = nullptr;
		TestTrue(
			*FString::Printf(
				TEXT("%s.%s range schema exists"),
				Expected.Action,
				Expected.Param),
			Action->ParamSchema->TryGetObjectField(
				Expected.Param,
				ParamSchema)
				&& ParamSchema
				&& ParamSchema->IsValid());
		if (!ParamSchema || !ParamSchema->IsValid())
		{
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s.%s minimum"),
				Expected.Action,
				Expected.Param),
			static_cast<int32>(
				(*ParamSchema)->GetNumberField(TEXT("minimum"))),
			Expected.Minimum);
		TestEqual(
			*FString::Printf(
				TEXT("%s.%s maximum"),
				Expected.Action,
				Expected.Param),
			static_cast<int32>(
				(*ParamSchema)->GetNumberField(TEXT("maximum"))),
			Expected.Maximum);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
