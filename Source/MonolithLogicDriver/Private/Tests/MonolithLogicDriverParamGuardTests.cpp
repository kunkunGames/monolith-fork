#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithLogicDriverComponentActions.h"
#include "MonolithLogicDriverScaffoldActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteScaffoldAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("logicdriver"), Action))
		{
			FMonolithLogicDriverScaffoldActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("logicdriver"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetSMComponentConfigRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.GetSMComponentConfigRejectsMalformedParams", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGetSMComponentConfigRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	// Test 1: Missing blueprint_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_name"), TEXT("MyComp"));

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Missing blueprint_path should return error"), !Result.bSuccess);
		TestTrue(TEXT("Missing blueprint_path error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 2: Wrong type for blueprint_path (number instead of string)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("blueprint_path"), 12345);

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Wrong type blueprint_path should return error"), !Result.bSuccess);
		TestTrue(TEXT("Wrong type blueprint_path error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 3: Wrong type for component_name (number instead of string)
	{
		// If blueprint_path is valid but doesn't exist, it returns a load error after parsing component_name safely.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/NonExistent/BP_Dummy"));
		Params->SetNumberField(TEXT("component_name"), 123);

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(Params);
		TestTrue(TEXT("Valid blueprint_path but wrong type component_name should safely return load error (not crash)"), !Result.bSuccess);
		TestTrue(TEXT("Should fail because blueprint path doesn't exist"), Result.ErrorMessage.Contains(TEXT("Failed to load")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverScaffoldRejectsMalformedArraysTest, "Monolith.ParamGuard.LogicDriver.ScaffoldRejectsMalformedArrays", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverScaffoldRejectsMalformedArraysTest::RunTest(const FString& Parameters)
{
	// Test 1: dialogue_nodes malformed
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetStringField(TEXT("dialogue_nodes"), TEXT("not_an_array"));

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_dialogue_sm"), Params);
		TestTrue(TEXT("Malformed dialogue_nodes should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention dialogue_nodes array"), Result.ErrorMessage.Contains(TEXT("dialogue_nodes")) && Result.ErrorMessage.Contains(TEXT("array")));
	}

	// Test 2: choices malformed
	{
		TSharedPtr<FJsonObject> DialogueNode = MakeShared<FJsonObject>();
		DialogueNode->SetStringField(TEXT("text"), TEXT("hello"));
		DialogueNode->SetStringField(TEXT("choices"), TEXT("not_an_array"));

		TArray<TSharedPtr<FJsonValue>> DialogueNodes;
		DialogueNodes.Add(MakeShared<FJsonValueObject>(DialogueNode));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetArrayField(TEXT("dialogue_nodes"), DialogueNodes);

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_dialogue_sm"), Params);
		TestTrue(TEXT("Malformed choices should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention choices array"), Result.ErrorMessage.Contains(TEXT("choices")) && Result.ErrorMessage.Contains(TEXT("array")));
	}

	// Test 3: choices with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> Choices;
		Choices.Add(MakeShared<FJsonValueNumber>(123));

		TSharedPtr<FJsonObject> DialogueNode = MakeShared<FJsonObject>();
		DialogueNode->SetStringField(TEXT("text"), TEXT("hello"));
		DialogueNode->SetArrayField(TEXT("choices"), Choices);

		TArray<TSharedPtr<FJsonValue>> DialogueNodes;
		DialogueNodes.Add(MakeShared<FJsonValueObject>(DialogueNode));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetArrayField(TEXT("dialogue_nodes"), DialogueNodes);

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_dialogue_sm"), Params);
		TestTrue(TEXT("Choices array with wrong item type should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention choices string type"), Result.ErrorMessage.Contains(TEXT("choices")) && Result.ErrorMessage.Contains(TEXT("string")));
	}

	// Test 4: objectives malformed
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetStringField(TEXT("objectives"), TEXT("not_an_array"));

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_quest_sm"), Params);
		TestTrue(TEXT("Malformed objectives should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention objectives array"), Result.ErrorMessage.Contains(TEXT("objectives")) && Result.ErrorMessage.Contains(TEXT("array")));
	}

	// Test 5: objectives with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> Objectives;
		Objectives.Add(MakeShared<FJsonValueNumber>(123));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetArrayField(TEXT("objectives"), Objectives);

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_quest_sm"), Params);
		TestTrue(TEXT("Objectives array with wrong item type should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention objectives string type"), Result.ErrorMessage.Contains(TEXT("objectives")) && Result.ErrorMessage.Contains(TEXT("string")));
	}

	// Test 6: states malformed
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetStringField(TEXT("states"), TEXT("not_an_array"));

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_interactable_sm"), Params);
		TestTrue(TEXT("Malformed states should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention states array"), Result.ErrorMessage.Contains(TEXT("states")) && Result.ErrorMessage.Contains(TEXT("array")));
	}

	// Test 7: states with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> States;
		States.Add(MakeShared<FJsonValueNumber>(123));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetArrayField(TEXT("states"), States);

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_interactable_sm"), Params);
		TestTrue(TEXT("States array with wrong item type should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention states string type"), Result.ErrorMessage.Contains(TEXT("states")) && Result.ErrorMessage.Contains(TEXT("string")));
	}

	// Test 8: dialogue_nodes with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> DialogueNodes;
		DialogueNodes.Add(MakeShared<FJsonValueNumber>(123));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestSM"));
		Params->SetArrayField(TEXT("dialogue_nodes"), DialogueNodes);

		FMonolithActionResult Result = ExecuteScaffoldAction(TEXT("scaffold_dialogue_sm"), Params);
		TestTrue(TEXT("Dialogue nodes array with wrong item type should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention dialogue_nodes object type"), Result.ErrorMessage.Contains(TEXT("dialogue_nodes")) && Result.ErrorMessage.Contains(TEXT("object")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
