#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithComboGraphActions.h"
#include "Dom/JsonObject.h"
#include "MonolithJsonUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_COMBOGRAPH

namespace
{
	FMonolithActionResult ExecuteComboGraphAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("combograph"), Action))
		{
			FMonolithComboGraphActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("combograph"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithComboGraphParamGuardTest, "Monolith.ParamGuard.ComboGraph.InvalidParamsCheck", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithComboGraphParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullParams = nullptr;

	TArray<FString> Actions = {
		TEXT("list_combo_graphs"),
		TEXT("get_combo_graph_info"),
		TEXT("get_combo_node_effects"),
		TEXT("validate_combo_graph"),
		TEXT("create_combo_graph"),
		TEXT("add_combo_node"),
		TEXT("add_combo_edge"),
		TEXT("set_combo_node_effects"),
		TEXT("set_combo_node_cues"),
		TEXT("create_combo_ability"),
		TEXT("link_ability_to_combo_graph"),
		TEXT("scaffold_combo_from_montages"),
		TEXT("layout_combo_graph")
	};

	for (const FString& Action : Actions)
	{
		FMonolithActionResult Result = ExecuteComboGraphAction(Action, NullParams);
		TestFalse(*FString::Printf(TEXT("Action %s should fail on null params"), *Action), Result.bSuccess);
		// Check that the error correctly reports missing params
		TestTrue(*FString::Printf(TEXT("Action %s should return invalid parameters error"), *Action),
			Result.ErrorMessage.Contains(TEXT("Invalid parameters")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_COMBOGRAPH
