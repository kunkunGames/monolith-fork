#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverGraphActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.ConfigureTransitionFunctional
// Validates the functional 'happy path' for configuring a transition node's properties.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverConfigureTransitionFunctionalTest, "Monolith.LogicDriverKeeper.ConfigureTransitionFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverConfigureTransitionFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("configure_transition")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
		FMonolithLogicDriverNodeActions::RegisterActions(Registry);
	}

	FString AssetPath = TEXT("/Game/Tests/SM_ConfigureTransitionTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		TestTrue(TEXT("create_state_machine should succeed"), CreateResult.bSuccess);
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	FString StateA_Guid, StateB_Guid;
	{
		TSharedPtr<FJsonObject> AddStateParamsA = MakeShared<FJsonObject>();
		AddStateParamsA->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParamsA->SetStringField(TEXT("name"), TEXT("StateA"));
		FMonolithActionResult AddResultA = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParamsA);
		TestTrue(TEXT("add_state A should succeed"), AddResultA.bSuccess);
		if (AddResultA.bSuccess && AddResultA.Payload.IsValid())
		{
			AddResultA.Payload->TryGetStringField(TEXT("node_guid"), StateA_Guid);
		}

		TSharedPtr<FJsonObject> AddStateParamsB = MakeShared<FJsonObject>();
		AddStateParamsB->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParamsB->SetStringField(TEXT("name"), TEXT("StateB"));
		FMonolithActionResult AddResultB = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParamsB);
		TestTrue(TEXT("add_state B should succeed"), AddResultB.bSuccess);
		if (AddResultB.bSuccess && AddResultB.Payload.IsValid())
		{
			AddResultB.Payload->TryGetStringField(TEXT("node_guid"), StateB_Guid);
		}
	}

	FString TransitionGuid;
	if (!StateA_Guid.IsEmpty() && !StateB_Guid.IsEmpty())
	{
		TSharedPtr<FJsonObject> AddTransParams = MakeShared<FJsonObject>();
		AddTransParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddTransParams->SetStringField(TEXT("source_guid"), StateA_Guid);
		AddTransParams->SetStringField(TEXT("target_guid"), StateB_Guid);

		FMonolithActionResult TransResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_transition"), AddTransParams);
		TestTrue(TEXT("add_transition should succeed"), TransResult.bSuccess);
		if (TransResult.bSuccess && TransResult.Payload.IsValid())
		{
			TransResult.Payload->TryGetStringField(TEXT("node_guid"), TransitionGuid);
		}
	}

	if (!TransitionGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> ConfigParams = MakeShared<FJsonObject>();
		ConfigParams->SetStringField(TEXT("asset_path"), AssetPath);
		ConfigParams->SetStringField(TEXT("node_guid"), TransitionGuid);
		ConfigParams->SetNumberField(TEXT("priority"), 2.0);
		ConfigParams->SetBoolField(TEXT("can_eval_with_start_state"), true);

		FMonolithActionResult ConfigResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("configure_transition"), ConfigParams);
		TestTrue(TEXT("configure_transition should succeed"), ConfigResult.bSuccess);
		if (ConfigResult.bSuccess && ConfigResult.Payload.IsValid())
		{
			FString ConfigNodeType;
			ConfigResult.Payload->TryGetStringField(TEXT("node_type"), ConfigNodeType);
			TestEqual(TEXT("Node type should be transition"), ConfigNodeType, TEXT("transition"));

			const TArray<TSharedPtr<FJsonValue>>* PropsSet = nullptr;
			if (ConfigResult.Payload->TryGetArrayField(TEXT("properties_set"), PropsSet))
			{
				TestEqual(TEXT("Should set 2 properties"), PropsSet->Num(), 2);
			}
			else
			{
				AddError(TEXT("properties_set array missing from payload"));
			}
		}
	}

	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		FMonolithActionResult DelResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
		TestTrue(TEXT("Cleanup should succeed"), DelResult.bSuccess);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
