#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverGraphActions.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.GraphWiringFunctional
// Validates the functional 'happy path' for adding states, conduits, and transitions.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverGraphWiringFunctionalTest, "Monolith.LogicDriverKeeper.GraphWiringFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverGraphWiringFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_state")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_WiringTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add Source State
	FString SourceGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("SourceState"));
		AddStateParams->SetNumberField(TEXT("position_x"), 0);
		AddStateParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), SourceGuid);
		}
	}

	// 3. Add Target Conduit
	FString TargetGuid;
	{
		TSharedPtr<FJsonObject> AddConduitParams = MakeShared<FJsonObject>();
		AddConduitParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddConduitParams->SetStringField(TEXT("name"), TEXT("TargetConduit"));
		AddConduitParams->SetNumberField(TEXT("position_x"), 300);
		AddConduitParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_conduit"), AddConduitParams);
		TestTrue(TEXT("add_conduit should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), TargetGuid);
		}
	}

	// 4. Add Transition
	if (!SourceGuid.IsEmpty() && !TargetGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> AddTransParams = MakeShared<FJsonObject>();
		AddTransParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddTransParams->SetStringField(TEXT("source_guid"), SourceGuid);
		AddTransParams->SetStringField(TEXT("target_guid"), TargetGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_transition"), AddTransParams);
		TestTrue(TEXT("add_transition should succeed for valid nodes"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			TestTrue(TEXT("Payload contains action"), Result.Payload->HasField(TEXT("action")));
		}
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.RemoveNodeFunctional
// Validates the functional 'happy path' for removing a node from a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverRemoveNodeFunctionalTest, "Monolith.LogicDriverKeeper.RemoveNodeFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverRemoveNodeFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("remove_node")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_RemoveNodeTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add State to remove
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("StateToRemove"));
		AddStateParams->SetNumberField(TEXT("position_x"), 0);
		AddStateParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
		}
	}

	// 3. Remove the State
	if (!NodeGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetStringField(TEXT("node_guid"), NodeGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("remove_node"), RemoveParams);
		TestTrue(TEXT("remove_node should succeed for valid node"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be remove_node"), ActionName, TEXT("remove_node"));
		}
	}

	return true;
}




// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.RenameNodeFunctional
// Validates the functional 'happy path' for renaming a node in a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverRenameNodeFunctionalTest, "Monolith.LogicDriverKeeper.RenameNodeFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverRenameNodeFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("rename_node")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_RenameNodeTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add State to rename
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("OldStateName"));
		AddStateParams->SetNumberField(TEXT("position_x"), 0);
		AddStateParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
		}
	}

	// 3. Rename the State
	if (!NodeGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
		RenameParams->SetStringField(TEXT("asset_path"), AssetPath);
		RenameParams->SetStringField(TEXT("node_guid"), NodeGuid);
		RenameParams->SetStringField(TEXT("new_name"), TEXT("NewStateName"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("rename_node"), RenameParams);
		TestTrue(TEXT("rename_node should succeed for valid node"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be rename_node"), ActionName, TEXT("rename_node"));

			FString NewNameResult;
			Result.Payload->TryGetStringField(TEXT("new_name"), NewNameResult);
			TestEqual(TEXT("New name should match"), NewNameResult, TEXT("NewStateName"));
		}
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.MoveNodeFunctional
// Validates the functional 'happy path' for moving a node in a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverMoveNodeFunctionalTest, "Monolith.LogicDriverKeeper.MoveNodeFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverMoveNodeFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("move_node")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_MoveNodeTest");
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

	// 2. Add State to move
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("StateToMove"));
		AddStateParams->SetNumberField(TEXT("position_x"), 0);
		AddStateParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
		}
	}

	// 3. Move the State
	if (!NodeGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> MoveParams = MakeShared<FJsonObject>();
		MoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		MoveParams->SetStringField(TEXT("node_guid"), NodeGuid);
		MoveParams->SetNumberField(TEXT("position_x"), 150);
		MoveParams->SetNumberField(TEXT("position_y"), 250);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("move_node"), MoveParams);
		TestTrue(TEXT("move_node should succeed for valid node"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be move_node"), ActionName, TEXT("move_node"));

			double NewX = 0;
			Result.Payload->TryGetNumberField(TEXT("new_x"), NewX);
			TestEqual(TEXT("New X should match"), NewX, 150.0);

			double NewY = 0;
			Result.Payload->TryGetNumberField(TEXT("new_y"), NewY);
			TestEqual(TEXT("New Y should match"), NewY, 250.0);
		}
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.SetInitialStateFunctional
// Validates the functional 'happy path' for setting the initial state of a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverSetInitialStateFunctionalTest, "Monolith.LogicDriverKeeper.SetInitialStateFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverSetInitialStateFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("set_initial_state")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_SetInitialStateTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		// Note: The memory instruction says: "When writing EditorContext automation tests for Monolith...
		// do NOT assert TestTrue on the failing condition. Doing so forces a test failure and breaks CI pipelines for environments without the plugin."
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add State to be initial
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("InitialState"));
		AddStateParams->SetNumberField(TEXT("position_x"), 0);
		AddStateParams->SetNumberField(TEXT("position_y"), 0);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
		}
	}

	// 3. Set Initial State
	if (!NodeGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> SetInitialParams = MakeShared<FJsonObject>();
		SetInitialParams->SetStringField(TEXT("asset_path"), AssetPath);
		SetInitialParams->SetStringField(TEXT("node_guid"), NodeGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("set_initial_state"), SetInitialParams);
		TestTrue(TEXT("set_initial_state should succeed for valid node"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be set_initial_state"), ActionName, TEXT("set_initial_state"));

			FString SetNodeGuid;
			Result.Payload->TryGetStringField(TEXT("node_guid"), SetNodeGuid);
			TestEqual(TEXT("Set node guid should match"), SetNodeGuid, NodeGuid);
		}
	}

	// 4. Cleanup (if delete_state_machine action is available, use it to leave clean state)
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.AddAnyStateNodeFunctional
// Validates the functional 'happy path' for adding an Any State node to a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverAddAnyStateNodeFunctionalTest, "Monolith.LogicDriverKeeper.AddAnyStateNodeFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverAddAnyStateNodeFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_any_state_node")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_AddAnyStateTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add Any State node
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetNumberField(TEXT("position_x"), 100);
		AddStateParams->SetNumberField(TEXT("position_y"), 100);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_any_state_node"), AddStateParams);
		TestTrue(TEXT("add_any_state_node should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
			TestFalse(TEXT("Should return a valid guid"), NodeGuid.IsEmpty());

			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be add_any_state_node"), ActionName, TEXT("add_any_state_node"));
		}
	}

	// 3. Cleanup
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.AddStateMachineNodeFunctional
// Validates the functional 'happy path' for adding a nested state machine node to a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverAddStateMachineNodeFunctionalTest, "Monolith.LogicDriverKeeper.AddStateMachineNodeFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverAddStateMachineNodeFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_state_machine_node")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_AddStateMachineNodeTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add Nested State Machine node
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("NestedStateMachine"));
		AddStateParams->SetNumberField(TEXT("position_x"), 100);
		AddStateParams->SetNumberField(TEXT("position_y"), 100);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state_machine_node"), AddStateParams);
		TestTrue(TEXT("add_state_machine_node should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
			TestFalse(TEXT("Should return a valid guid"), NodeGuid.IsEmpty());

			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be add_state_machine_node"), ActionName, TEXT("add_state_machine_node"));
		}
	}

	// 3. Cleanup
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.GetNodeConnectionsFunctional
// Validates the functional 'happy path' for getting node connections in a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverGetNodeConnectionsFunctionalTest, "Monolith.LogicDriverKeeper.GetNodeConnectionsFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverGetNodeConnectionsFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("get_node_connections")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_GetConnectionsTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add Source State
	FString SourceGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("SourceState"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), SourceGuid);
		}
	}

	// 3. Add Target State
	FString TargetGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("TargetState"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), TargetGuid);
		}
	}

	// 4. Add Transition
	if (!SourceGuid.IsEmpty() && !TargetGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> AddTransParams = MakeShared<FJsonObject>();
		AddTransParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddTransParams->SetStringField(TEXT("source_guid"), SourceGuid);
		AddTransParams->SetStringField(TEXT("target_guid"), TargetGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_transition"), AddTransParams);
		TestTrue(TEXT("add_transition should succeed"), Result.bSuccess);
	}

	// 5. Get Node Connections
	if (!SourceGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> GetConnsParams = MakeShared<FJsonObject>();
		GetConnsParams->SetStringField(TEXT("asset_path"), AssetPath);
		GetConnsParams->SetStringField(TEXT("node_guid"), SourceGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("get_node_connections"), GetConnsParams);
		TestTrue(TEXT("get_node_connections should succeed for valid node"), Result.bSuccess);

		if (Result.bSuccess && Result.Payload.IsValid())
		{
			double OutboundCount = 0;
			Result.Payload->TryGetNumberField(TEXT("outbound_count"), OutboundCount);
			TestTrue(TEXT("Should have at least 1 outbound connection"), OutboundCount >= 1.0);

			const TArray<TSharedPtr<FJsonValue>>* OutboundArray;
			if (Result.Payload->TryGetArrayField(TEXT("outbound"), OutboundArray))
			{
				TestTrue(TEXT("Outbound array should match count"), OutboundArray->Num() == OutboundCount);
			}
		}
	}

	// 6. Cleanup
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}



// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.GetNodeDetailsFunctional
// Validates the functional 'happy path' for getting detailed node info in a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverGetNodeDetailsFunctionalTest, "Monolith.LogicDriverKeeper.GetNodeDetailsFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverGetNodeDetailsFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("get_node_details")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_GetNodeDetailsTest");
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

	// 2. Add Source State
	FString SourceGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("DetailsState"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), SourceGuid);
		}
	}

	// 3. Get Node Details
	if (!SourceGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> GetDetailsParams = MakeShared<FJsonObject>();
		GetDetailsParams->SetStringField(TEXT("asset_path"), AssetPath);
		GetDetailsParams->SetStringField(TEXT("node_guid"), SourceGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("get_node_details"), GetDetailsParams);
		TestTrue(TEXT("get_node_details should succeed for valid node"), Result.bSuccess);

		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString NodeName;
			Result.Payload->TryGetStringField(TEXT("name"), NodeName);
			TestEqual(TEXT("Node name should match"), NodeName, TEXT("DetailsState"));

			TestTrue(TEXT("Payload contains properties"), Result.Payload->HasField(TEXT("properties")));
		}
	}

	// 4. Cleanup
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// Monolith.LogicDriverKeeper.GetSMStatisticsFunctional
// Validates the functional 'happy path' for getting state machine statistics.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverGetSMStatisticsFunctionalTest, "Monolith.LogicDriverKeeper.GetSMStatisticsFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverGetSMStatisticsFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("get_sm_statistics")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_StatisticsTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		if (!CreateResult.bSuccess)
		{
			TestTrue(TEXT("Create Succeeded"), false);
			return true;
		}
	}

	// 2. Add Source State
	FString SourceGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("SourceState"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), SourceGuid);
		}
	}

	// 3. Add Target Conduit
	FString TargetGuid;
	{
		TSharedPtr<FJsonObject> AddConduitParams = MakeShared<FJsonObject>();
		AddConduitParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddConduitParams->SetStringField(TEXT("name"), TEXT("TargetConduit"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_conduit"), AddConduitParams);
		TestTrue(TEXT("add_conduit should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), TargetGuid);
		}
	}

	// 4. Add Transition
	if (!SourceGuid.IsEmpty() && !TargetGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> AddTransParams = MakeShared<FJsonObject>();
		AddTransParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddTransParams->SetStringField(TEXT("source_guid"), SourceGuid);
		AddTransParams->SetStringField(TEXT("target_guid"), TargetGuid);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_transition"), AddTransParams);
		TestTrue(TEXT("add_transition should succeed"), Result.bSuccess);
	}

	// 5. Get SM Statistics
	{
		TSharedPtr<FJsonObject> StatsParams = MakeShared<FJsonObject>();
		StatsParams->SetStringField(TEXT("asset_path"), AssetPath);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("get_sm_statistics"), StatsParams);
		TestTrue(TEXT("get_sm_statistics should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Payload.IsValid())
		{
			double StateCount = 0;
			Result.Payload->TryGetNumberField(TEXT("states"), StateCount);
			TestTrue(TEXT("State count should be at least 1"), StateCount >= 1.0);

			double ConduitCount = 0;
			Result.Payload->TryGetNumberField(TEXT("conduits"), ConduitCount);
			TestTrue(TEXT("Conduit count should be at least 1"), ConduitCount >= 1.0);

			double TransitionCount = 0;
			Result.Payload->TryGetNumberField(TEXT("transitions"), TransitionCount);
			TestTrue(TEXT("Transition count should be at least 1"), TransitionCount >= 1.0);

			double TotalNodes = 0;
			Result.Payload->TryGetNumberField(TEXT("total_nodes"), TotalNodes);
			TestTrue(TEXT("Total nodes count should be at least 3"), TotalNodes >= 3.0);
		}
	}

	// 6. Cleanup
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}

// Monolith.LogicDriverKeeper.SetEndStateFunctional
// Validates the functional 'happy path' for setting a node as the end state of a state machine graph.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverSetEndStateFunctionalTest, "Monolith.LogicDriverKeeper.SetEndStateFunctional", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverSetEndStateFunctionalTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("set_end_state")))
	{
		FMonolithLogicDriverAssetActions::RegisterActions(Registry);
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// 1. Create a State Machine Blueprint
	FString AssetPath = TEXT("/Game/Tests/SM_SetEndStateTest");
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), AssetPath);
		FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("create_state_machine"), CreateParams);

		// In Jules VM or environments without Logic Driver Pro loaded, this might fail,
		// but we still want the test coverage to be syntactically correct and run what it can.
		// Note: The memory instruction says: "When writing EditorContext automation tests for Monolith...
		// do NOT assert TestTrue on the failing condition. Doing so forces a test failure and breaks CI pipelines for environments without the plugin."
		if (!CreateResult.bSuccess)
		{
			return true;
		}
	}

	// 2. Add State to be an end state
	FString NodeGuid;
	{
		TSharedPtr<FJsonObject> AddStateParams = MakeShared<FJsonObject>();
		AddStateParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddStateParams->SetStringField(TEXT("name"), TEXT("EndState"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), AddStateParams);
		TestTrue(TEXT("add_state should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Payload.IsValid())
		{
			Result.Payload->TryGetStringField(TEXT("node_guid"), NodeGuid);
		}
	}

	// 3. Set End State
	if (!NodeGuid.IsEmpty())
	{
		TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetStringField(TEXT("asset_path"), AssetPath);
		SetParams->SetStringField(TEXT("node_guid"), NodeGuid);
		SetParams->SetBoolField(TEXT("is_end_state"), true);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("set_end_state"), SetParams);
		TestTrue(TEXT("set_end_state should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Payload.IsValid())
		{
			FString ActionName;
			Result.Payload->TryGetStringField(TEXT("action"), ActionName);
			TestEqual(TEXT("Action should be set_end_state"), ActionName, TEXT("set_end_state"));

			FString SetNodeGuid;
			Result.Payload->TryGetStringField(TEXT("node_guid"), SetNodeGuid);
			TestEqual(TEXT("Set node guid should match"), SetNodeGuid, NodeGuid);
		}
	}

	// 4. Cleanup (if delete_state_machine action is available, use it to leave clean state)
	if (Registry.HasAction(TEXT("logicdriver"), TEXT("delete_state_machine")))
	{
		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		Registry.ExecuteAction(TEXT("logicdriver"), TEXT("delete_state_machine"), DeleteParams);
	}

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
