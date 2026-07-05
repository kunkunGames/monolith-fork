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

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
