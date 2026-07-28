#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverGraphActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.ConfigureStateFunctional
// Validates the functional 'happy path' for configuring a state node's properties.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverConfigureStateFunctionalTest, "Monolith.LogicDriverKeeper.ConfigureStateFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverConfigureStateFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("configure_state")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
		FMonolithLogicDriverNodeActions::RegisterActions(Registry);
	}

	FString AssetPath = TEXT("/Game/Tests/SM_ConfigureStateTest");
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

	FString StateGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("MyConfigState"));
		FMonolithActionResult AddResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), AddResult.bSuccess);
		if (AddResult.bSuccess && AddResult.Payload.IsValid())
		{
			AddResult.Payload->TryGetStringField(TEXT("node_guid"), StateGuid);
		}
	}

	if (!StateGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> ConfigParams = MakeShared<FJsonObject>();
		ConfigParams->SetStringField(TEXT("asset_path"), AssetPath);
		ConfigParams->SetStringField(TEXT("node_guid"), StateGuid);
		ConfigParams->SetBoolField(TEXT("always_update"), true);
		ConfigParams->SetBoolField(TEXT("disable_tick_transition"), true);
		ConfigParams->SetBoolField(TEXT("exclude_from_any_state"), true);

		FMonolithActionResult ConfigResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("configure_state"), ConfigParams);
		TestTrue(TEXT("configure_state should succeed"), ConfigResult.bSuccess);
		if (ConfigResult.bSuccess && ConfigResult.Payload.IsValid())
		{
			FString ConfigNodeType;
			ConfigResult.Payload->TryGetStringField(TEXT("node_type"), ConfigNodeType);
			TestEqual(TEXT("Node type should be state"), ConfigNodeType, TEXT("state"));

			const TArray<TSharedPtr<FJsonValue>>* PropsSet = nullptr;
			if (ConfigResult.Payload->TryGetArrayField(TEXT("properties_set"), PropsSet))
			{
				TestEqual(TEXT("Should set 3 properties"), PropsSet->Num(), 3);
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
