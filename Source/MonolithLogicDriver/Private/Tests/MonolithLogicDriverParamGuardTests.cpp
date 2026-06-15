#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithJsonUtils.h"
#include "MonolithLogicDriverComponentActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "MonolithLogicDriverScaffoldActions.h"
#include "MonolithLogicDriverSpecActions.h"
#include "MonolithLogicDriverGraphActions.h"

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverComponentActionsRejectsMissingBlueprintPathTest, "Monolith.ParamGuard.LogicDriver.ComponentActionsRejectsMissingBlueprintPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverComponentActionsRejectsMissingBlueprintPathTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();

	// Test 1: get_sm_component_config
	{
		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleGetSMComponentConfig(EmptyParams);
		TestTrue(TEXT("HandleGetSMComponentConfig rejects missing blueprint_path"), !Result.bSuccess);
		TestEqual(TEXT("HandleGetSMComponentConfig returns ErrInvalidParams (-32602)"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 2: add_sm_component
	{
		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleAddSMComponent(EmptyParams);
		TestTrue(TEXT("HandleAddSMComponent rejects missing blueprint_path"), !Result.bSuccess);
		TestEqual(TEXT("HandleAddSMComponent returns ErrInvalidParams (-32602)"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
	}

	// Test 3: configure_sm_component
	{
		FMonolithActionResult Result = FMonolithLogicDriverComponentActions::HandleConfigureSMComponent(EmptyParams);
		TestTrue(TEXT("HandleConfigureSMComponent rejects missing blueprint_path"), !Result.bSuccess);
		TestEqual(TEXT("HandleConfigureSMComponent returns ErrInvalidParams (-32602)"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error message should mention blueprint_path"), Result.ErrorMessage.Contains(TEXT("blueprint_path")));
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



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverAddAnyStateNodeRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.AddAnyStateNodeRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverAddAnyStateNodeRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_any_state_node")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// position_x as string instead of number
	TSharedPtr<FJsonObject> BadXParams = MakeShared<FJsonObject>();
	BadXParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadXParams->SetStringField(TEXT("position_x"), TEXT("100"));

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_any_state_node"), BadXParams);
	TestTrue(TEXT("add_any_state_node rejects string position_x"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions position_x must be a number"), Result1.ErrorMessage.Contains(TEXT("position_x")) && Result1.ErrorMessage.Contains(TEXT("must be a number")));

	// position_y as bool instead of number
	TSharedPtr<FJsonObject> BadYParams = MakeShared<FJsonObject>();
	BadYParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadYParams->SetNumberField(TEXT("position_x"), 100);
	BadYParams->SetBoolField(TEXT("position_y"), true);

	FMonolithActionResult Result2 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_any_state_node"), BadYParams);
	TestTrue(TEXT("add_any_state_node rejects bool position_y"), !Result2.bSuccess);
	TestTrue(TEXT("error mentions position_y must be a number"), Result2.ErrorMessage.Contains(TEXT("position_y")) && Result2.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverSetExposedPropertyRejectsMissingValueTest, "Monolith.ParamGuard.LogicDriver.SetExposedPropertyRejectsMissingValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverSetExposedPropertyRejectsMissingValueTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("set_exposed_property")))
	{
		FMonolithLogicDriverNodeActions::RegisterActions(Registry);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	Params->SetStringField(TEXT("node_guid"), TEXT("test-guid"));
	Params->SetStringField(TEXT("property_name"), TEXT("MyProperty"));

	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("set_exposed_property"), Params);
	TestTrue(TEXT("set_exposed_property rejects missing value"), !Result.bSuccess);
	TestTrue(TEXT("error mentions missing value param"), Result.ErrorMessage.Contains(TEXT("value")));

	Params->SetNumberField(TEXT("value"), 123.0);
	Result = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("set_exposed_property"), Params);
	TestTrue(TEXT("set_exposed_property rejects non-string value"), !Result.bSuccess);
	TestEqual(TEXT("non-string value returns ErrInvalidParams (-32602)"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("error mentions string value param"), Result.ErrorMessage.Contains(TEXT("value")) && Result.ErrorMessage.Contains(TEXT("string")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverAddTransitionRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.AddTransitionRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverAddTransitionRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_transition")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// priority as string instead of number
	TSharedPtr<FJsonObject> BadPriorityParams = MakeShared<FJsonObject>();
	BadPriorityParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadPriorityParams->SetStringField(TEXT("source_guid"), TEXT("test-source-guid"));
	BadPriorityParams->SetStringField(TEXT("target_guid"), TEXT("test-target-guid"));
	BadPriorityParams->SetStringField(TEXT("priority"), TEXT("1"));

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_transition"), BadPriorityParams);
	TestTrue(TEXT("add_transition rejects string priority"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions priority must be a number"), Result1.ErrorMessage.Contains(TEXT("priority")) && Result1.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverAddStateRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.AddStateRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverAddStateRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_state")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// name as bool instead of string
	TSharedPtr<FJsonObject> BadNameParams = MakeShared<FJsonObject>();
	BadNameParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadNameParams->SetBoolField(TEXT("name"), true);

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), BadNameParams);
	TestTrue(TEXT("add_state rejects bool name"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions name must be a string"), Result1.ErrorMessage.Contains(TEXT("name")) && Result1.ErrorMessage.Contains(TEXT("must be a string")));

	// position_x as string instead of number
	TSharedPtr<FJsonObject> BadXParams = MakeShared<FJsonObject>();
	BadXParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadXParams->SetStringField(TEXT("position_x"), TEXT("100"));

	FMonolithActionResult Result2 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), BadXParams);
	TestTrue(TEXT("add_state rejects string position_x"), !Result2.bSuccess);
	TestTrue(TEXT("error mentions position_x must be a number"), Result2.ErrorMessage.Contains(TEXT("position_x")) && Result2.ErrorMessage.Contains(TEXT("must be a number")));

	// position_y as string instead of number
	TSharedPtr<FJsonObject> BadYParams = MakeShared<FJsonObject>();
	BadYParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadYParams->SetStringField(TEXT("position_y"), TEXT("100"));

	FMonolithActionResult Result3 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state"), BadYParams);
	TestTrue(TEXT("add_state rejects string position_y"), !Result3.bSuccess);
	TestTrue(TEXT("error mentions position_y must be a number"), Result3.ErrorMessage.Contains(TEXT("position_y")) && Result3.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverAddConduitRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.AddConduitRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverAddConduitRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_conduit")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// name as bool instead of string
	TSharedPtr<FJsonObject> BadNameParams = MakeShared<FJsonObject>();
	BadNameParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadNameParams->SetBoolField(TEXT("name"), true);

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_conduit"), BadNameParams);
	TestTrue(TEXT("add_conduit rejects bool name"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions name must be a string"), Result1.ErrorMessage.Contains(TEXT("name")) && Result1.ErrorMessage.Contains(TEXT("must be a string")));

	// position_x as string instead of number
	TSharedPtr<FJsonObject> BadXParams = MakeShared<FJsonObject>();
	BadXParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadXParams->SetStringField(TEXT("position_x"), TEXT("100"));

	FMonolithActionResult Result2 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_conduit"), BadXParams);
	TestTrue(TEXT("add_conduit rejects string position_x"), !Result2.bSuccess);
	TestTrue(TEXT("error mentions position_x must be a number"), Result2.ErrorMessage.Contains(TEXT("position_x")) && Result2.ErrorMessage.Contains(TEXT("must be a number")));

	// position_y as string instead of number
	TSharedPtr<FJsonObject> BadYParams = MakeShared<FJsonObject>();
	BadYParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadYParams->SetStringField(TEXT("position_y"), TEXT("100"));

	FMonolithActionResult Result3 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_conduit"), BadYParams);
	TestTrue(TEXT("add_conduit rejects string position_y"), !Result3.bSuccess);
	TestTrue(TEXT("error mentions position_y must be a number"), Result3.ErrorMessage.Contains(TEXT("position_y")) && Result3.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverSetEndStateRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.SetEndStateRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverSetEndStateRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("set_end_state")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// is_end_state as string instead of bool
	TSharedPtr<FJsonObject> BadIsEndStateParams = MakeShared<FJsonObject>();
	BadIsEndStateParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadIsEndStateParams->SetStringField(TEXT("node_guid"), TEXT("test-node-guid"));
	BadIsEndStateParams->SetStringField(TEXT("is_end_state"), TEXT("true"));

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("set_end_state"), BadIsEndStateParams);
	TestTrue(TEXT("set_end_state rejects string is_end_state"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions is_end_state must be a boolean"), Result1.ErrorMessage.Contains(TEXT("is_end_state")) && Result1.ErrorMessage.Contains(TEXT("must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverAddStateMachineNodeRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.AddStateMachineNodeRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverAddStateMachineNodeRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("add_state_machine_node")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// name as bool instead of string
	TSharedPtr<FJsonObject> BadNameParams = MakeShared<FJsonObject>();
	BadNameParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadNameParams->SetBoolField(TEXT("name"), true);

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state_machine_node"), BadNameParams);
	TestTrue(TEXT("add_state_machine_node rejects bool name"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions name must be a string"), Result1.ErrorMessage.Contains(TEXT("name")) && Result1.ErrorMessage.Contains(TEXT("must be a string")));

	// reference_path as bool instead of string
	TSharedPtr<FJsonObject> BadRefParams = MakeShared<FJsonObject>();
	BadRefParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadRefParams->SetBoolField(TEXT("reference_path"), true);

	FMonolithActionResult Result2 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state_machine_node"), BadRefParams);
	TestTrue(TEXT("add_state_machine_node rejects bool reference_path"), !Result2.bSuccess);
	TestTrue(TEXT("error mentions reference_path must be a string"), Result2.ErrorMessage.Contains(TEXT("reference_path")) && Result2.ErrorMessage.Contains(TEXT("must be a string")));

	// position_x as string instead of number
	TSharedPtr<FJsonObject> BadXParams = MakeShared<FJsonObject>();
	BadXParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadXParams->SetStringField(TEXT("position_x"), TEXT("100"));

	FMonolithActionResult Result3 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state_machine_node"), BadXParams);
	TestTrue(TEXT("add_state_machine_node rejects string position_x"), !Result3.bSuccess);
	TestTrue(TEXT("error mentions position_x must be a number"), Result3.ErrorMessage.Contains(TEXT("position_x")) && Result3.ErrorMessage.Contains(TEXT("must be a number")));

	// position_y as string instead of number
	TSharedPtr<FJsonObject> BadYParams = MakeShared<FJsonObject>();
	BadYParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadYParams->SetStringField(TEXT("position_y"), TEXT("100"));

	FMonolithActionResult Result4 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("add_state_machine_node"), BadYParams);
	TestTrue(TEXT("add_state_machine_node rejects string position_y"), !Result4.bSuccess);
	TestTrue(TEXT("error mentions position_y must be a number"), Result4.ErrorMessage.Contains(TEXT("position_y")) && Result4.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLogicDriverMoveNodeRejectsMalformedParamsTest, "Monolith.ParamGuard.LogicDriver.MoveNodeRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardLogicDriverMoveNodeRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("logicdriver"), TEXT("move_node")))
	{
		FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	}

	// position_x as string instead of number
	TSharedPtr<FJsonObject> BadPosXParams = MakeShared<FJsonObject>();
	BadPosXParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadPosXParams->SetStringField(TEXT("node_guid"), TEXT("test-node-guid"));
	BadPosXParams->SetStringField(TEXT("position_x"), TEXT("100"));
	BadPosXParams->SetNumberField(TEXT("position_y"), 100);

	FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("move_node"), BadPosXParams);
	TestTrue(TEXT("move_node rejects string position_x"), !Result1.bSuccess);
	TestTrue(TEXT("error mentions position_x must be a number"), Result1.ErrorMessage.Contains(TEXT("position_x")) && Result1.ErrorMessage.Contains(TEXT("must be a number")));

	// position_y as string instead of number
	TSharedPtr<FJsonObject> BadPosYParams = MakeShared<FJsonObject>();
	BadPosYParams->SetStringField(TEXT("asset_path"), TEXT("/Game/SM_Test.SM_Test"));
	BadPosYParams->SetStringField(TEXT("node_guid"), TEXT("test-node-guid"));
	BadPosYParams->SetNumberField(TEXT("position_x"), 100);
	BadPosYParams->SetStringField(TEXT("position_y"), TEXT("100"));

	FMonolithActionResult Result2 = Registry.ExecuteAction(TEXT("logicdriver"), TEXT("move_node"), BadPosYParams);
	TestTrue(TEXT("move_node rejects string position_y"), !Result2.bSuccess);
	TestTrue(TEXT("error mentions position_y must be a number"), Result2.ErrorMessage.Contains(TEXT("position_y")) && Result2.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
