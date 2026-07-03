#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "MonolithBlueprintActions.h"
#include "MonolithBlueprintBuildActions.h"
#include "MonolithBlueprintCDOActions.h"
#include "MonolithBlueprintCompileActions.h"
#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintDiffActions.h"
#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintGraphExportActions.h"
#include "MonolithBlueprintLayoutActions.h"
#include "MonolithBlueprintNodeActions.h"
#include "MonolithBlueprintSpawnActions.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintTemplateActions.h"
#include "MonolithBlueprintVariableActions.h"
#include "MonolithTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardVariableActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.VariableActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardVariableActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintVariableActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("add_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("name"), TEXT("Health"));
					Params->SetStringField(TEXT("type"), TEXT("float"));
					Params->SetStringField(TEXT("instance_editable"), TEXT("true"));
				},
				TEXT("instance_editable"),
				TEXT("add_variable should reject malformed instance_editable")
			},
			{
				TEXT("remove_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("name"), 12.0);
				},
				TEXT("name"),
				TEXT("remove_variable should reject malformed name")
			},
			{
				TEXT("rename_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("old_name"), TEXT("OldName"));
					Params->SetNumberField(TEXT("new_name"), 12.0);
				},
				TEXT("new_name"),
				TEXT("rename_variable should reject malformed new_name")
			},
			{
				TEXT("set_variable_type"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("name"), TEXT("Health"));
					Params->SetNumberField(TEXT("type"), 12.0);
				},
				TEXT("type"),
				TEXT("set_variable_type should reject malformed type")
			},
			{
				TEXT("set_variable_defaults"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("name"), TEXT("Health"));
					Params->SetStringField(TEXT("save_game"), TEXT("true"));
				},
				TEXT("save_game"),
				TEXT("set_variable_defaults should reject malformed save_game")
			},
			{
				TEXT("add_local_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("function_name"), 12.0);
					Params->SetStringField(TEXT("name"), TEXT("LocalHealth"));
					Params->SetStringField(TEXT("type"), TEXT("float"));
				},
				TEXT("function_name"),
				TEXT("add_local_variable should reject malformed function_name")
			},
			{
				TEXT("remove_local_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("function_name"), TEXT("Calculate"));
					Params->SetNumberField(TEXT("name"), 12.0);
				},
				TEXT("name"),
				TEXT("remove_local_variable should reject malformed name")
			},
			{
				TEXT("add_replicated_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("variable_name"), TEXT("ReplicatedHealth"));
					Params->SetStringField(TEXT("type"), TEXT("float"));
					Params->SetStringField(TEXT("create_on_rep"), TEXT("true"));
				},
				TEXT("create_on_rep"),
				TEXT("add_replicated_variable should reject malformed create_on_rep")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardComponentActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.ComponentActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardComponentActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintComponentActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("add_component"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("component_class"), 12.0);
				},
				TEXT("component_class"),
				TEXT("add_component should reject malformed component_class")
			},
			{
				TEXT("remove_component"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("component_name"), TEXT("Mesh"));
					Params->SetStringField(TEXT("promote_children"), TEXT("true"));
				},
				TEXT("promote_children"),
				TEXT("remove_component should reject malformed promote_children")
			},
			{
				TEXT("rename_component"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("component_name"), TEXT("Mesh"));
					Params->SetNumberField(TEXT("new_name"), 12.0);
				},
				TEXT("new_name"),
				TEXT("rename_component should reject malformed new_name")
			},
			{
				TEXT("reparent_component"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("component_name"), TEXT("Mesh"));
					Params->SetStringField(TEXT("new_parent"), TEXT("Root"));
					Params->SetNumberField(TEXT("attach_socket"), 12.0);
				},
				TEXT("attach_socket"),
				TEXT("reparent_component should reject malformed attach_socket")
			},
			{
				TEXT("set_component_property"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("component_name"), TEXT("Mesh"));
					Params->SetStringField(TEXT("property_name"), TEXT("Mobility"));
					Params->SetNumberField(TEXT("value"), 12.0);
				},
				TEXT("value"),
				TEXT("set_component_property should reject malformed value")
			},
			{
				TEXT("duplicate_component"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("component_name"), TEXT("Mesh"));
					Params->SetNumberField(TEXT("new_name"), 12.0);
				},
				TEXT("new_name"),
				TEXT("duplicate_component should reject malformed new_name")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardReadActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.ReadActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardReadActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintActions::RegisterActions();
		},
		{
			{
				TEXT("list_graphs"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("list_graphs should reject malformed asset_path")
			},
			{
				TEXT("get_graph_data"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("graph_name"), 12.0);
				},
				TEXT("graph_name"),
				TEXT("get_graph_data should reject malformed graph_name")
			},
			{
				TEXT("get_graph_summary"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("graph_name"), 12.0);
				},
				TEXT("graph_name"),
				TEXT("get_graph_summary should reject malformed graph_name")
			},
			{
				TEXT("get_variables"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_variables should reject malformed asset_path")
			},
			{
				TEXT("get_execution_flow"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("entry_point"), 12.0);
				},
				TEXT("entry_point"),
				TEXT("get_execution_flow should reject malformed entry_point")
			},
			{
				TEXT("search_nodes"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("query"), 12.0);
				},
				TEXT("query"),
				TEXT("search_nodes should reject malformed query")
			},
			{
				TEXT("get_components"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_components should reject malformed asset_path")
			},
			{
				TEXT("get_component_details"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("component_name"), 12.0);
				},
				TEXT("component_name"),
				TEXT("get_component_details should reject malformed component_name")
			},
			{
				TEXT("get_functions"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_functions should reject malformed asset_path")
			},
			{
				TEXT("get_event_dispatchers"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_event_dispatchers should reject malformed asset_path")
			},
			{
				TEXT("get_parent_class"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_parent_class should reject malformed asset_path")
			},
			{
				TEXT("get_interfaces"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_interfaces should reject malformed asset_path")
			},
			{
				TEXT("get_construction_script"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_construction_script should reject malformed asset_path")
			},
			{
				TEXT("search_functions"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("query"), TEXT("Move"));
					Params->SetStringField(TEXT("limit"), TEXT("10"));
				},
				TEXT("limit"),
				TEXT("search_functions should reject malformed limit")
			},
			{
				TEXT("get_node_details"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("node_id"), 12.0);
				},
				TEXT("node_id"),
				TEXT("get_node_details should reject malformed node_id")
			},
			{
				TEXT("get_interface_functions"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("interface_class"), 12.0);
				},
				TEXT("interface_class"),
				TEXT("get_interface_functions should reject malformed interface_class")
			},
			{
				TEXT("get_function_signature"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("function_name"), TEXT("Execute"));
					Params->SetStringField(TEXT("include_inherited"), TEXT("true"));
				},
				TEXT("include_inherited"),
				TEXT("get_function_signature should reject malformed include_inherited")
			},
			{
				TEXT("get_event_dispatcher_details"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("dispatcher_name"), 12.0);
				},
				TEXT("dispatcher_name"),
				TEXT("get_event_dispatcher_details should reject malformed dispatcher_name")
			},
			{
				TEXT("get_blueprint_info"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_blueprint_info should reject malformed asset_path")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardCompileAndCDOActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.CompileAndCDOActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardCompileAndCDOActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintCompileActions::RegisterActions(Registry);
			FMonolithBlueprintCDOActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("compile_blueprint"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("compile_blueprint should reject malformed asset_path")
			},
			{
				TEXT("validate_blueprint"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("validate_blueprint should reject malformed asset_path")
			},
			{
				TEXT("create_blueprint"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuardCreated"));
					Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
					Params->SetStringField(TEXT("skip_save"), TEXT("false"));
				},
				TEXT("skip_save"),
				TEXT("create_blueprint should reject malformed skip_save")
			},
			{
				TEXT("duplicate_blueprint"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("new_path"), 12.0);
				},
				TEXT("new_path"),
				TEXT("duplicate_blueprint should reject malformed new_path")
			},
			{
				TEXT("get_dependencies"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("direction"), 12.0);
				},
				TEXT("direction"),
				TEXT("get_dependencies should reject malformed direction")
			},
			{
				TEXT("save_asset"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("save_asset should reject malformed asset_path")
			},
			{
				TEXT("get_cdo_properties"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("include_parent_defaults"), TEXT("true"));
				},
				TEXT("include_parent_defaults"),
				TEXT("get_cdo_properties should reject malformed include_parent_defaults")
			},
			{
				TEXT("set_cdo_property"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("property_name"), TEXT("Health"));
					Params->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
				},
				TEXT("value"),
				TEXT("set_cdo_property should reject malformed value")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardGraphActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.GraphActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardGraphActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintGraphActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("add_function"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("name"), TEXT("Execute"));
					Params->SetStringField(TEXT("reliable"), TEXT("false"));
				},
				TEXT("reliable"),
				TEXT("add_function should reject malformed reliable")
			},
			{
				TEXT("remove_function"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("name"), 12.0);
				},
				TEXT("name"),
				TEXT("remove_function should reject malformed name")
			},
			{
				TEXT("rename_function"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("old_name"), TEXT("OldExecute"));
					Params->SetNumberField(TEXT("new_name"), 12.0);
				},
				TEXT("new_name"),
				TEXT("rename_function should reject malformed new_name")
			},
			{
				TEXT("add_macro"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("name"), 12.0);
				},
				TEXT("name"),
				TEXT("add_macro should reject malformed name")
			},
			{
				TEXT("remove_macro"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("macro_name"), 12.0);
				},
				TEXT("macro_name"),
				TEXT("remove_macro should reject malformed macro_name")
			},
			{
				TEXT("rename_macro"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("old_name"), 12.0);
					Params->SetStringField(TEXT("new_name"), TEXT("NewMacro"));
				},
				TEXT("old_name"),
				TEXT("rename_macro should reject malformed old_name")
			},
			{
				TEXT("add_event_dispatcher"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("name"), 12.0);
				},
				TEXT("name"),
				TEXT("add_event_dispatcher should reject malformed name")
			},
			{
				TEXT("set_function_params"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("function_name"), TEXT("Execute"));
					Params->SetObjectField(TEXT("inputs"), MakeShared<FJsonObject>());
				},
				TEXT("inputs"),
				TEXT("set_function_params should reject malformed inputs")
			},
			{
				TEXT("implement_interface"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("interface_class"), 12.0);
				},
				TEXT("interface_class"),
				TEXT("implement_interface should reject malformed interface_class")
			},
			{
				TEXT("remove_interface"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("interface_class"), TEXT("BPI_ParamGuard"));
					Params->SetStringField(TEXT("preserve_functions"), TEXT("true"));
				},
				TEXT("preserve_functions"),
				TEXT("remove_interface should reject malformed preserve_functions")
			},
			{
				TEXT("reparent_blueprint"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("new_parent_class"), 12.0);
				},
				TEXT("new_parent_class"),
				TEXT("reparent_blueprint should reject malformed new_parent_class")
			},
			{
				TEXT("remove_event_dispatcher"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("dispatcher_name"), 12.0);
				},
				TEXT("dispatcher_name"),
				TEXT("remove_event_dispatcher should reject malformed dispatcher_name")
			},
			{
				TEXT("set_event_dispatcher_params"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("dispatcher_name"), TEXT("OnTriggered"));
					Params->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
				},
				TEXT("params"),
				TEXT("set_event_dispatcher_params should reject malformed params")
			},
			{
				TEXT("scaffold_interface_implementation"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("interface_class"), 12.0);
				},
				TEXT("interface_class"),
				TEXT("scaffold_interface_implementation should reject malformed interface_class")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardNodeActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.NodeActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardNodeActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintNodeActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("add_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_type"), TEXT("Branch"));
					Params->SetObjectField(TEXT("position"), MakeShared<FJsonObject>());
				},
				TEXT("position"),
				TEXT("add_node should reject malformed position")
			},
			{
				TEXT("remove_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("node_id"), 12.0);
				},
				TEXT("node_id"),
				TEXT("remove_node should reject malformed node_id")
			},
			{
				TEXT("connect_pins"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("source_node"), 12.0);
					Params->SetStringField(TEXT("source_pin"), TEXT("Then"));
					Params->SetStringField(TEXT("target_node"), TEXT("TargetNode"));
					Params->SetStringField(TEXT("target_pin"), TEXT("Execute"));
				},
				TEXT("source_node"),
				TEXT("connect_pins should reject malformed source_node")
			},
			{
				TEXT("disconnect_pins"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_id"), TEXT("SourceNode"));
					Params->SetStringField(TEXT("pin_name"), TEXT("Then"));
					Params->SetStringField(TEXT("target_node"), TEXT("TargetNode"));
					Params->SetNumberField(TEXT("target_pin"), 12.0);
				},
				TEXT("target_pin"),
				TEXT("disconnect_pins should reject malformed target_pin")
			},
			{
				TEXT("set_pin_default"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_id"), TEXT("Node"));
					Params->SetStringField(TEXT("pin_name"), TEXT("Value"));
					Params->SetNumberField(TEXT("value"), 12.0);
				},
				TEXT("value"),
				TEXT("set_pin_default should reject malformed value")
			},
			{
				TEXT("set_node_position"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_id"), TEXT("Node"));
					Params->SetObjectField(TEXT("position"), MakeShared<FJsonObject>());
				},
				TEXT("position"),
				TEXT("set_node_position should reject malformed position")
			},
			{
				TEXT("resolve_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("node_type"), TEXT("CustomEvent"));
					Params->SetStringField(TEXT("reliable"), TEXT("maybe"));
				},
				TEXT("reliable"),
				TEXT("resolve_node should reject malformed reliable")
			},
			{
				TEXT("search_spawnable_nodes"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("query"), TEXT("Print"));
					Params->SetStringField(TEXT("limit"), TEXT("10"));
				},
				TEXT("limit"),
				TEXT("search_spawnable_nodes should reject malformed limit")
			},
			{
				TEXT("spawn_cached_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("cache_id"), TEXT("cache-1"));
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("position"), MakeShared<FJsonObject>());
				},
				TEXT("position"),
				TEXT("spawn_cached_node should reject malformed position")
			},
			{
				TEXT("validate_node_cache"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("validate_node_cache should reject malformed asset_path")
			},
			{
				TEXT("register_node_alias"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_id"), TEXT("Node"));
					Params->SetStringField(TEXT("alias"), TEXT("Start"));
					Params->SetStringField(TEXT("overwrite"), TEXT("true"));
				},
				TEXT("overwrite"),
				TEXT("register_node_alias should reject malformed overwrite")
			},
			{
				TEXT("resolve_node_alias"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("alias"), 12.0);
				},
				TEXT("alias"),
				TEXT("resolve_node_alias should reject malformed alias")
			},
			{
				TEXT("clear_node_aliases"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("clear_node_aliases should reject malformed asset_path")
			},
			{
				TEXT("batch_execute"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("operations"), MakeShared<FJsonObject>());
				},
				TEXT("operations"),
				TEXT("batch_execute should reject malformed operations")
			},
			{
				TEXT("add_nodes_bulk"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("nodes"), MakeShared<FJsonObject>());
				},
				TEXT("nodes"),
				TEXT("add_nodes_bulk should reject malformed nodes")
			},
			{
				TEXT("connect_pins_bulk"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("connections"), MakeShared<FJsonObject>());
				},
				TEXT("connections"),
				TEXT("connect_pins_bulk should reject malformed connections")
			},
			{
				TEXT("set_pin_defaults_bulk"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("defaults"), MakeShared<FJsonObject>());
				},
				TEXT("defaults"),
				TEXT("set_pin_defaults_bulk should reject malformed defaults")
			},
			{
				TEXT("add_timeline"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("auto_play"), TEXT("true"));
				},
				TEXT("auto_play"),
				TEXT("add_timeline should reject malformed auto_play")
			},
			{
				TEXT("add_event_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("event_name"), TEXT("BeginPlay"));
					Params->SetObjectField(TEXT("position"), MakeShared<FJsonObject>());
				},
				TEXT("position"),
				TEXT("add_event_node should reject malformed position")
			},
			{
				TEXT("add_comment_node"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("text"), TEXT("Comment"));
					Params->SetStringField(TEXT("color"), TEXT("yellow"));
				},
				TEXT("color"),
				TEXT("add_comment_node should reject malformed color")
			},
			{
				TEXT("get_timeline_data"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("timeline_name"), 12.0);
				},
				TEXT("timeline_name"),
				TEXT("get_timeline_data should reject malformed timeline_name")
			},
			{
				TEXT("add_timeline_track"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("timeline_name"), TEXT("Intro"));
					Params->SetStringField(TEXT("track_name"), TEXT("Alpha"));
					Params->SetNumberField(TEXT("track_type"), 12.0);
				},
				TEXT("track_type"),
				TEXT("add_timeline_track should reject malformed track_type")
			},
			{
				TEXT("set_timeline_keys"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("timeline_name"), TEXT("Intro"));
					Params->SetStringField(TEXT("track_name"), TEXT("Alpha"));
					Params->SetObjectField(TEXT("keys"), MakeShared<FJsonObject>());
				},
				TEXT("keys"),
				TEXT("set_timeline_keys should reject malformed keys")
			},
			{
				TEXT("promote_pin_to_variable"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("node_id"), TEXT("Node"));
					Params->SetStringField(TEXT("pin_name"), TEXT("Value"));
					Params->SetNumberField(TEXT("variable_name"), 12.0);
				},
				TEXT("variable_name"),
				TEXT("promote_pin_to_variable should reject malformed variable_name")
			},
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintResolveNodeAcceptsReliableStringLiteral, "Monolith.Blueprint.ResolveNode.AcceptsReliableStringLiteral", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintResolveNodeAcceptsReliableStringLiteral::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithBlueprintNodeActions::RegisterActions(Registry);

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_type"), TEXT("CustomEvent"));
	Params->SetStringField(TEXT("replication"), TEXT("server"));
	Params->SetStringField(TEXT("reliable"), TEXT("true"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("blueprint"), TEXT("resolve_node"), Params);
	bool bOk = true;
	bOk &= TestTrue(TEXT("resolve_node accepts reliable string literal"), Result.bSuccess);
	bOk &= TestTrue(TEXT("resolve_node returns a result object"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		bool bReliable = false;
		bOk &= TestTrue(TEXT("resolve_node emits reliable field"), Result.Result->TryGetBoolField(TEXT("reliable"), bReliable));
		bOk &= TestTrue(TEXT("resolve_node reliable string literal resolves true"), bReliable);
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintParamGuardUtilityAndDataActionsRejectMalformedTopLevelParams, "Monolith.ParamGuard.Blueprint.UtilityAndDataActionsRejectMalformedTopLevelParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintParamGuardUtilityAndDataActionsRejectMalformedTopLevelParams::RunTest(const FString& Parameters)
{
	return FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("blueprint"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithBlueprintBuildActions::RegisterActions(Registry);
			FMonolithBlueprintDiffActions::RegisterActions(Registry);
			FMonolithBlueprintGraphExportActions::RegisterActions(Registry);
			FMonolithBlueprintLayoutActions::RegisterActions(Registry);
			FMonolithBlueprintSpawnActions::RegisterActions(Registry);
			FMonolithBlueprintStructActions::RegisterActions(Registry);
			FMonolithBlueprintTemplateActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("build_blueprint_from_spec"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("auto_compile"), TEXT("true"));
				},
				TEXT("auto_compile"),
				TEXT("build_blueprint_from_spec should reject malformed auto_compile")
			},
			{
				TEXT("compare_blueprints"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path_a"), 12.0);
					Params->SetStringField(TEXT("asset_path_b"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuardB"));
				},
				TEXT("asset_path_a"),
				TEXT("compare_blueprints should reject malformed asset_path_a")
			},
			{
				TEXT("export_graph"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("graph_name"), 12.0);
				},
				TEXT("graph_name"),
				TEXT("export_graph should reject malformed graph_name")
			},
			{
				TEXT("copy_nodes"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("source_asset"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetObjectField(TEXT("node_ids"), MakeShared<FJsonObject>());
					Params->SetStringField(TEXT("target_asset"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuardCopy"));
				},
				TEXT("node_ids"),
				TEXT("copy_nodes should reject malformed node_ids")
			},
			{
				TEXT("duplicate_graph"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetNumberField(TEXT("graph_name"), 12.0);
					Params->SetStringField(TEXT("new_name"), TEXT("DuplicatedGraph"));
				},
				TEXT("graph_name"),
				TEXT("duplicate_graph should reject malformed graph_name")
			},
			{
				TEXT("auto_layout"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("horizontal_spacing"), TEXT("350"));
				},
				TEXT("horizontal_spacing"),
				TEXT("auto_layout should reject malformed horizontal_spacing")
			},
			{
				TEXT("spawn_blueprint_actor"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("blueprint"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("select"), TEXT("true"));
				},
				TEXT("select"),
				TEXT("spawn_blueprint_actor should reject malformed select")
			},
			{
				TEXT("batch_spawn_blueprint_actors"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("blueprint"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("count"), TEXT("10"));
				},
				TEXT("count"),
				TEXT("batch_spawn_blueprint_actors should reject malformed count")
			},
			{
				TEXT("apply_template"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("template_name"), TEXT("health_system"));
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_ParamGuard"));
					Params->SetStringField(TEXT("params"), TEXT("{}"));
				},
				TEXT("params"),
				TEXT("apply_template should reject malformed params")
			},
			{
				TEXT("create_user_defined_struct"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/Data/S_ParamGuard"));
					Params->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
				},
				TEXT("fields"),
				TEXT("create_user_defined_struct should reject malformed fields")
			},
			{
				TEXT("create_user_defined_enum"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/Data/E_ParamGuard"));
					Params->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());
				},
				TEXT("values"),
				TEXT("create_user_defined_enum should reject malformed values")
			},
			{
				TEXT("create_data_table"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetNumberField(TEXT("row_struct"), 12.0);
				},
				TEXT("row_struct"),
				TEXT("create_data_table should reject malformed row_struct")
			},
			{
				TEXT("add_data_table_row"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
					Params->SetStringField(TEXT("values"), TEXT("{}"));
				},
				TEXT("values"),
				TEXT("add_data_table_row should reject malformed values")
			},
			{
				TEXT("get_data_table_rows"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetNumberField(TEXT("row_name"), 12.0);
				},
				TEXT("row_name"),
				TEXT("get_data_table_rows should reject malformed row_name")
			},
			{
				TEXT("get_data_table_schema"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 12.0);
				},
				TEXT("asset_path"),
				TEXT("get_data_table_schema should reject malformed asset_path")
			},
			{
				TEXT("update_data_table_row"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
					Params->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());
					Params->SetStringField(TEXT("confirm"), TEXT("true"));
				},
				TEXT("confirm"),
				TEXT("update_data_table_row should reject malformed confirm")
			},
			{
				TEXT("remove_data_table_row"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
					Params->SetStringField(TEXT("dry_run"), TEXT("true"));
				},
				TEXT("dry_run"),
				TEXT("remove_data_table_row should reject malformed dry_run")
			},
			{
				TEXT("export_data_table_csv"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Data/DT_ParamGuard"));
					Params->SetStringField(TEXT("file_path"), TEXT("Saved/ParamGuard.csv"));
					Params->SetStringField(TEXT("confirm"), TEXT("true"));
				},
				TEXT("confirm"),
				TEXT("export_data_table_csv should reject malformed confirm")
			},
			{
				TEXT("create_data_asset"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/Data/DA_ParamGuard"));
					Params->SetStringField(TEXT("class_name"), TEXT("DataAsset"));
					Params->SetStringField(TEXT("skip_save"), TEXT("true"));
				},
				TEXT("skip_save"),
				TEXT("create_data_asset should reject malformed skip_save")
			},
		});
}

#endif // WITH_DEV_AUTOMATION_TESTS
