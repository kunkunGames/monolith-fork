#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithLogicDriverComponentActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "MonolithLogicDriverScaffoldActions.h"
#include "MonolithLogicDriverSpecActions.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER

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

	FMonolithActionResult ExecuteNodeAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("logicdriver"), Action))
		{
			FMonolithLogicDriverNodeActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("logicdriver"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetSMComponentConfigRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.GetSMComponentConfigRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverSetStateTagsRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.SetStateTagsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverSetStateTagsRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	// Test 1: missing gameplay_tags
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/DummyPath"));
		Params->SetStringField(TEXT("node_guid"), TEXT("DummyGuid"));

		FMonolithActionResult Result = ExecuteNodeAction(TEXT("set_state_tags"), Params);
		TestTrue(TEXT("Missing tags should return error"), !Result.bSuccess);
		TestTrue(TEXT("Missing tags error message should mention gameplay_tags"), Result.ErrorMessage.Contains(TEXT("gameplay_tags")));
	}

	// Test 2: malformed gameplay_tags type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/DummyPath"));
		Params->SetStringField(TEXT("node_guid"), TEXT("DummyGuid"));
		Params->SetStringField(TEXT("gameplay_tags"), TEXT("NotAnArray"));

		FMonolithActionResult Result = ExecuteNodeAction(TEXT("set_state_tags"), Params);
		TestTrue(TEXT("Malformed tags should return error"), !Result.bSuccess);
		TestTrue(TEXT("Malformed tags error message should mention gameplay_tags"), Result.ErrorMessage.Contains(TEXT("gameplay_tags")));
	}

	// Test 3: valid array but malformed item type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/DummyPath"));
		Params->SetStringField(TEXT("node_guid"), TEXT("DummyGuid"));

		TArray<TSharedPtr<FJsonValue>> ArrayVal;
		ArrayVal.Add(MakeShared<FJsonValueNumber>(123));
		Params->SetArrayField(TEXT("gameplay_tags"), ArrayVal);

		FMonolithActionResult Result = ExecuteNodeAction(TEXT("set_state_tags"), Params);
		TestTrue(TEXT("Malformed tag items should return error"), !Result.bSuccess);
		TestTrue(TEXT("Malformed tags error message should mention gameplay_tags"), Result.ErrorMessage.Contains(TEXT("gameplay_tags")) && Result.ErrorMessage.Contains(TEXT("string")));
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverBuildSMFromSpecRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.BuildSMFromSpecRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverBuildSMFromSpecRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	auto ExecuteAction = [](const TSharedPtr<FJsonObject>& Params) -> FMonolithActionResult
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("logicdriver"), TEXT("build_sm_from_spec")))
		{
			FMonolithLogicDriverSpecActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("logicdriver"), TEXT("build_sm_from_spec"), Params);
	};

	// Helper to create a valid base spec
	auto CreateBaseSpec = []() -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		return Spec;
	};

	auto CreateBaseParams = [&]() -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test/SM_SpecTest"));
		return Params;
	};

		// Test 1: states is malformed
	{
		TSharedPtr<FJsonObject> Params = CreateBaseParams();
		TSharedPtr<FJsonObject> Spec = CreateBaseSpec();
		Spec->SetStringField(TEXT("states"), TEXT("NotAnArray"));
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAction(Params);
		TestTrue(TEXT("Malformed states array should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention states"), Result.ErrorMessage.Contains(TEXT("states")));
	}

	// Test 2: conduits is malformed
	{
		TSharedPtr<FJsonObject> Params = CreateBaseParams();
		TSharedPtr<FJsonObject> Spec = CreateBaseSpec();
		Spec->SetNumberField(TEXT("conduits"), 123);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAction(Params);
		TestTrue(TEXT("Malformed conduits array should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention conduits"), Result.ErrorMessage.Contains(TEXT("conduits")));
	}

	// Test 3: transitions is malformed
	{
		TSharedPtr<FJsonObject> Params = CreateBaseParams();
		TSharedPtr<FJsonObject> Spec = CreateBaseSpec();
		Spec->SetStringField(TEXT("transitions"), TEXT("NotAnArray"));
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAction(Params);
		TestTrue(TEXT("Malformed transitions array should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention transitions"), Result.ErrorMessage.Contains(TEXT("transitions")));
	}

	// Test 4: states with malformed is_initial/is_end
	{
		TSharedPtr<FJsonObject> Params = CreateBaseParams();
		TSharedPtr<FJsonObject> Spec = CreateBaseSpec();

		TArray<TSharedPtr<FJsonValue>> States;
		TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("name"), TEXT("StateA"));
		State->SetStringField(TEXT("is_initial"), TEXT("true")); // should be bool
		State->SetNumberField(TEXT("is_end"), 1); // should be bool
		States.Add(MakeShared<FJsonValueObject>(State));

		Spec->SetArrayField(TEXT("states"), States);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAction(Params);
		TestTrue(TEXT("Malformed is_initial/is_end bools should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention is_initial or is_end"), Result.ErrorMessage.Contains(TEXT("is_initial")) || Result.ErrorMessage.Contains(TEXT("is_end")));
	}

	// Test 5: nested_sms is malformed
	{
		TSharedPtr<FJsonObject> Params = CreateBaseParams();
		TSharedPtr<FJsonObject> Spec = CreateBaseSpec();
		Spec->SetNumberField(TEXT("nested_sms"), 456);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAction(Params);
		TestTrue(TEXT("Malformed nested_sms array should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention nested_sms"), Result.ErrorMessage.Contains(TEXT("nested_sms")));
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverConfigureSMComponentRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.ConfigureSMComponentRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverConfigureSMComponentRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	// Test 1: auto_start malformed
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Dummy"));
		Params->SetStringField(TEXT("auto_start"), TEXT("not_a_bool"));

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleConfigureSMComponent(Params);
		TestTrue(TEXT("Malformed auto_start should return error before Blueprint load"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention auto_start"), Result.ErrorMessage.Contains(TEXT("auto_start")));
	}

	// Test 2: tick_interval malformed
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Dummy"));
		Params->SetStringField(TEXT("tick_interval"), TEXT("not_a_number"));

		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleConfigureSMComponent(Params);
		TestTrue(TEXT("Malformed tick_interval should return error before Blueprint load"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention tick_interval"), Result.ErrorMessage.Contains(TEXT("tick_interval")));
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverNodeRejectsMalformedFieldsTest, "Monolith.ParamGuard.LogicDriver.NodeRejectsMalformedFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverNodeRejectsMalformedFieldsTest::RunTest(const FString& Parameters)
{
	// Test 1: configure_state with malformed always_update
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/TestSM"));
		Params->SetStringField(TEXT("node_guid"), TEXT("some-guid"));
		Params->SetStringField(TEXT("always_update"), TEXT("not_a_boolean"));

		FMonolithActionResult Result = ExecuteNodeAction(TEXT("configure_state"), Params);
		TestTrue(TEXT("Malformed always_update should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention always_update boolean"), Result.ErrorMessage.Contains(TEXT("always_update")) && Result.ErrorMessage.Contains(TEXT("boolean")));
	}

	// Test 2: configure_transition with malformed priority
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/TestSM"));
		Params->SetStringField(TEXT("node_guid"), TEXT("some-guid"));
		Params->SetStringField(TEXT("priority"), TEXT("not_a_number"));

		FMonolithActionResult Result = ExecuteNodeAction(TEXT("configure_transition"), Params);
		TestTrue(TEXT("Malformed priority should return error"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention priority number"), Result.ErrorMessage.Contains(TEXT("priority")) && Result.ErrorMessage.Contains(TEXT("number")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
