#include "MonolithControlRigWriteActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "ControlRigBlueprintLegacy.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMEditorAsset.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithControlRigWriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- get_control_rig_graph ---
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_graph"),
		TEXT("Read the full RigVM node graph from a Control Rig Blueprint: nodes, pins, connections"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Build());

	// --- add_control_rig_node ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_control_rig_node"),
		TEXT("Add a rig unit node to a Control Rig graph from a struct path (e.g. RigUnit_SetTransform)"),
		FMonolithActionHandler::CreateStatic(&HandleAddControlRigNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("struct_path"), TEXT("string"), TEXT("Script struct path, e.g. /Script/ControlRig.RigUnit_SetTransform"))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Node X position (default 0)"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Node Y position (default 0)"))
			.Optional(TEXT("node_name"), TEXT("string"), TEXT("Desired node name (auto-uniquified)"))
			.Optional(TEXT("method_name"), TEXT("string"), TEXT("Execute method name (default: Execute)"))
			.Optional(TEXT("pin_defaults"), TEXT("object"), TEXT("Pin default values as {pin_name: value} pairs"))
			.Build());

	// --- connect_control_rig_pins ---
	Registry.RegisterAction(TEXT("animation"), TEXT("connect_control_rig_pins"),
		TEXT("Connect two pins in a Control Rig graph using dot-notation paths (e.g. NodeName.PinName)"),
		FMonolithActionHandler::CreateStatic(&HandleConnectControlRigPins),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Output pin path, dot-notation: NodeName.PinName"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Input pin path, dot-notation: NodeName.PinName"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("edit_control_rig_array_pin"),
		TEXT("Edit a Control Rig array pin by direct RigVM pin path. operation: add | insert | remove | clear | duplicate | set_size."),
		FMonolithActionHandler::CreateStatic(&HandleEditControlRigArrayPin),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("pin_path"), TEXT("string"), TEXT("RigVM pin path, e.g. Node.Items or Node.Items.0"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("add | insert | remove | clear | duplicate | set_size"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Insert index for operation=insert"))
			.Optional(TEXT("size"), TEXT("integer"), TEXT("Target array size for operation=set_size"))
			.Optional(TEXT("default_value"), TEXT("string"), TEXT("Default value for new array elements"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("bind_control_rig_pin_variable"),
		TEXT("Bind, unbind, or promote a Control Rig pin variable by direct RigVM pin path. operation: bind | unbind | promote."),
		FMonolithActionHandler::CreateStatic(&HandleBindControlRigPinVariable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("pin_path"), TEXT("string"), TEXT("RigVM pin path, e.g. Node.Value"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("bind | unbind | promote"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("variable_path"), TEXT("string"), TEXT("Blueprint variable path/name for operation=bind"))
			.Optional(TEXT("create_variable_node"), TEXT("boolean"), TEXT("Create a variable node while promoting."))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Variable node X when create_variable_node=true"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Variable node Y when create_variable_node=true"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_control_rig_pin_metadata"),
		TEXT("Set Control Rig pin UI metadata by direct RigVM pin path: expansion and category."),
		FMonolithActionHandler::CreateStatic(&HandleSetControlRigPinMetadata),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("pin_path"), TEXT("string"), TEXT("RigVM pin path, e.g. Node.Transform"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("expanded"), TEXT("boolean"), TEXT("Optional expansion state"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Optional category string"))
			.Optional(TEXT("clear_category"), TEXT("boolean"), TEXT("Clear existing category"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("manage_control_rig_exposed_pin"),
		TEXT("Manage exposed pins on a Control Rig function/collapse graph. operation: add | remove | rename | change_type | reorder."),
		FMonolithActionHandler::CreateStatic(&HandleManageControlRigExposedPin),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("add | remove | rename | change_type | reorder"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Exposed pin name"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Function/collapse graph name"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("input | output | io | visible | hidden"))
			.Optional(TEXT("cpp_type"), TEXT("string"), TEXT("CPP type, e.g. float or FVector"))
			.Optional(TEXT("cpp_type_object_path"), TEXT("string"), TEXT("Type object path for object/struct types"))
			.Optional(TEXT("default_value"), TEXT("string"), TEXT("Default value for operation=add"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("New name for operation=rename"))
			.Optional(TEXT("new_index"), TEXT("integer"), TEXT("New index for operation=reorder"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("manage_control_rig_local_variable"),
		TEXT("Manage local variables in a Control Rig function/collapse graph. operation: add | remove | rename | set_type | set_default."),
		FMonolithActionHandler::CreateStatic(&HandleManageControlRigLocalVariable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("add | remove | rename | set_type | set_default"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Local variable name"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Function/collapse graph name"))
			.Optional(TEXT("cpp_type"), TEXT("string"), TEXT("CPP type for add/set_type"))
			.Optional(TEXT("cpp_type_object_path"), TEXT("string"), TEXT("Type object path for object/struct types"))
			.Optional(TEXT("default_value"), TEXT("string"), TEXT("Default value for add/set_default"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("New name for operation=rename"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_control_rig_node_metadata"),
		TEXT("Set category, keywords, and description on a Control Rig collapse/function node."),
		FMonolithActionHandler::CreateStatic(&HandleSetControlRigNodeMetadata),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("RigVM node name"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Node category"))
			.Optional(TEXT("keywords"), TEXT("string"), TEXT("Search keywords"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Node description"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("collapse_control_rig_nodes"),
		TEXT("Collapse named Control Rig nodes into a collapse node."),
		FMonolithActionHandler::CreateStatic(&HandleCollapseControlRigNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("node_names"), TEXT("array"), TEXT("Array of RigVM node names to collapse"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("collapse_node_name"), TEXT("string"), TEXT("Desired collapse node name"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("promote_control_rig_node"),
		TEXT("Promote a Control Rig collapse node to a function reference or demote a function reference to a collapse node."),
		FMonolithActionHandler::CreateStatic(&HandlePromoteControlRigNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("RigVM collapse/function-reference node name"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("collapse_to_function | function_to_collapse"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("existing_function_path"), TEXT("string"), TEXT("Optional existing function definition path for collapse_to_function"))
			.Optional(TEXT("remove_function_definition"), TEXT("boolean"), TEXT("Remove function definition during function_to_collapse"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("manage_control_rig_trait"),
		TEXT("Add or remove a RigVM trait on a Control Rig node. operation: add | remove."),
		FMonolithActionHandler::CreateStatic(&HandleManageControlRigTrait),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("RigVM node name"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("add | remove"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("trait_type_object_path"), TEXT("string"), TEXT("Trait script struct object path for operation=add"))
			.Optional(TEXT("trait_name"), TEXT("string"), TEXT("Trait name for add/remove"))
			.Optional(TEXT("default_value"), TEXT("string"), TEXT("Trait default value for add"))
			.Optional(TEXT("pin_index"), TEXT("integer"), TEXT("Pin index for add, default -1"))
			.Build());


	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("animation"), TEXT("add_control_rig_node"),
		{ TEXT("rig unit"), TEXT("RigVM node"), TEXT("control rig graph node"), TEXT("set transform node"), TEXT("CR node") },
		{ TEXT("add_rig_node"), TEXT("create_control_rig_node"), TEXT("add_rigvm_node") },
		{ TEXT("add a SetTransform rig unit to a control rig graph"), TEXT("place a RigVM node in the control rig") });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UControlRigBlueprint* LoadCRBlueprint(const FString& AssetPath, FString& OutError)
{
	UControlRigBlueprint* CRB = FMonolithAssetUtils::LoadAssetByPath<UControlRigBlueprint>(AssetPath);
	if (!CRB)
	{
		OutError = FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *AssetPath);
	}
	return CRB;
}

static URigVMGraph* GetGraphFromBlueprint(UControlRigBlueprint* CRB, const FString& GraphName, FString& OutError)
{
	if (GraphName.IsEmpty())
	{
		// Get root/default model
		URigVMGraph* Graph = CRB->GetDefaultModel();
		if (!Graph)
		{
			OutError = TEXT("Control Rig has no default graph");
		}
		return Graph;
	}

	// Search by name across all models
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
	if (!Client)
	{
		OutError = TEXT("Failed to get RigVMClient");
		return nullptr;
	}

	TArray<URigVMGraph*> AllGraphs = Client->GetAllModels(/*bIncludeFunctionLibrary=*/true, /*bRecursive=*/true);
	for (URigVMGraph* G : AllGraphs)
	{
		if (G && G->GetName() == GraphName)
		{
			return G;
		}
	}

	OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName);
	return nullptr;
}

static URigVMController* GetControllerForGraph(UControlRigBlueprint* CRB, URigVMGraph* Graph, FString& OutError)
{
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
	if (!Client)
	{
		OutError = TEXT("Failed to get RigVMClient");
		return nullptr;
	}

	URigVMController* Controller = Client->GetOrCreateController(Graph);
	if (!Controller)
	{
		OutError = TEXT("Failed to get or create RigVM controller");
	}
	return Controller;
}

static bool LoadRigContext(const TSharedPtr<FJsonObject>& Params, UControlRigBlueprint*& OutCRB, URigVMGraph*& OutGraph, URigVMController*& OutController, FString& OutError)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("asset_path is required");
		return false;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	OutCRB = LoadCRBlueprint(AssetPath, OutError);
	if (!OutCRB)
	{
		return false;
	}

	OutGraph = GetGraphFromBlueprint(OutCRB, GraphName, OutError);
	if (!OutGraph)
	{
		return false;
	}

	OutController = GetControllerForGraph(OutCRB, OutGraph, OutError);
	return OutController != nullptr;
}

static FMonolithActionResult InvalidParam(const FString& Message)
{
	return FMonolithActionResult::Error(Message, -32602);
}

static ERigVMPinDirection ParseRigVMPinDirection(const FString& Direction)
{
	if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Output;
	if (Direction.Equals(TEXT("io"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::IO;
	if (Direction.Equals(TEXT("visible"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Visible;
	if (Direction.Equals(TEXT("hidden"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Hidden;
	return ERigVMPinDirection::Input;
}

static void FinalizeControlRigMutation(UControlRigBlueprint* CRB, const TSharedPtr<FJsonObject>& Result)
{
	if (!CRB)
	{
		return;
	}

	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();
	Result->SetBoolField(TEXT("package_dirty"), CRB->GetOutermost() ? CRB->GetOutermost()->IsDirty() : true);
}

static TSharedPtr<FJsonObject> BasicRigMutationResult(UControlRigBlueprint* CRB, URigVMGraph* Graph, const FString& Operation)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("operation"), Operation);
	Result->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
	Result->SetStringField(TEXT("asset_path"), CRB ? CRB->GetPathName() : FString());
	return Result;
}

static bool ParseNodeNameArray(const TSharedPtr<FJsonObject>& Params, TArray<FName>& OutNodeNames, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("node_names"), Values) || !Values)
	{
		OutError = TEXT("node_names array is required");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString NodeName;
		if (!Value.IsValid() || !Value->TryGetString(NodeName) || NodeName.IsEmpty())
		{
			OutError = TEXT("node_names must contain only non-empty strings");
			return false;
		}
		OutNodeNames.Add(FName(*NodeName));
	}

	if (OutNodeNames.Num() == 0)
	{
		OutError = TEXT("node_names cannot be empty");
		return false;
	}

	return true;
}

static FString PinDirectionToString(ERigVMPinDirection Dir)
{
	switch (Dir)
	{
	case ERigVMPinDirection::Input:   return TEXT("Input");
	case ERigVMPinDirection::Output:  return TEXT("Output");
	case ERigVMPinDirection::IO:      return TEXT("IO");
	case ERigVMPinDirection::Visible: return TEXT("Visible");
	case ERigVMPinDirection::Hidden:  return TEXT("Hidden");
	default:                          return TEXT("Unknown");
	}
}

static TSharedPtr<FJsonObject> SerializePin(URigVMPin* Pin)
{
	TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
	PinObj->SetStringField(TEXT("name"), Pin->GetName());
	PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
	PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
	PinObj->SetStringField(TEXT("default_value"), Pin->GetDefaultValue());
	PinObj->SetStringField(TEXT("pin_path"), Pin->GetPinPath());

	// Connected pins
	TArray<TSharedPtr<FJsonValue>> ConnArr;
	ConnArr.Reserve(Pin->GetLinks().Num());
	for (URigVMLink* Link : Pin->GetLinks())
	{
		URigVMPin* OtherPin = (Link->GetSourcePin() == Pin) ? Link->GetTargetPin() : Link->GetSourcePin();
		if (OtherPin)
		{
			ConnArr.Add(MakeShared<FJsonValueString>(OtherPin->GetPinPath()));
		}
	}
	if (ConnArr.Num() > 0)
	{
		PinObj->SetArrayField(TEXT("connected_to"), ConnArr);
	}

	// Sub-pins (for struct types)
	const TArray<URigVMPin*>& SubPins = Pin->GetSubPins();
	if (SubPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SubArr;
		SubArr.Reserve(SubPins.Num());
		for (URigVMPin* SubPin : SubPins)
		{
			SubArr.Add(MakeShared<FJsonValueObject>(SerializePin(SubPin)));
		}
		PinObj->SetArrayField(TEXT("sub_pins"), SubArr);
	}

	return PinObj;
}

// ---------------------------------------------------------------------------
// get_control_rig_graph
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleGetControlRigGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = GetGraphFromBlueprint(CRB, GraphName, Error);
	if (!Graph) return FMonolithActionResult::Error(Error);

	// Serialize nodes
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	NodesArr.Reserve(Graph->GetNodes().Num());
	for (URigVMNode* Node : Graph->GetNodes())
	{
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("name"), Node->GetName());
		NodeObj->SetStringField(TEXT("node_path"), Node->GetNodePath());
		NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());

		// Position
		FVector2D Pos = Node->GetPosition();
		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Pos.X));
		PosArr.Add(MakeShared<FJsonValueNumber>(Pos.Y));
		NodeObj->SetArrayField(TEXT("position"), PosArr);

		// Struct path for unit nodes
		if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
		{
			UScriptStruct* SS = UnitNode->GetScriptStruct();
			if (SS)
			{
				NodeObj->SetStringField(TEXT("struct_path"), SS->GetPathName());
				NodeObj->SetStringField(TEXT("struct_name"), SS->GetName());
			}
		}

		// Pins (top-level only — sub-pins are nested inside)
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		PinsArr.Reserve(Node->GetPins().Num());
		for (URigVMPin* Pin : Node->GetPins())
		{
			if (!Pin) continue;
			PinsArr.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
		NodeObj->SetArrayField(TEXT("pins"), PinsArr);

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	// Serialize links
	TArray<TSharedPtr<FJsonValue>> LinksArr;
	LinksArr.Reserve(Graph->GetLinks().Num());
	for (URigVMLink* Link : Graph->GetLinks())
	{
		if (!Link) continue;

		TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
		LinkObj->SetStringField(TEXT("source"), Link->GetSourcePinPath());
		LinkObj->SetStringField(TEXT("target"), Link->GetTargetPinPath());
		LinksArr.Add(MakeShared<FJsonValueObject>(LinkObj));
	}

	// List available sub-graphs
	TArray<URigVMGraph*> ContainedGraphs = Graph->GetContainedGraphs();
	TArray<TSharedPtr<FJsonValue>> SubGraphArr;
	SubGraphArr.Reserve(ContainedGraphs.Num());
	for (URigVMGraph* SubG : ContainedGraphs)
	{
		if (SubG)
		{
			SubGraphArr.Add(MakeShared<FJsonValueString>(SubG->GetName()));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());
	Root->SetNumberField(TEXT("node_count"), NodesArr.Num());
	Root->SetNumberField(TEXT("link_count"), LinksArr.Num());
	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetArrayField(TEXT("links"), LinksArr);
	if (SubGraphArr.Num() > 0)
	{
		Root->SetArrayField(TEXT("sub_graphs"), SubGraphArr);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_control_rig_node
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleAddControlRigNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString StructPath;
	if (!Params->TryGetStringField(TEXT("struct_path"), StructPath) || StructPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("struct_path is required"));
	}

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("position_x"), PosX);
	Params->TryGetNumberField(TEXT("position_y"), PosY);

	FString NodeName;
	Params->TryGetStringField(TEXT("node_name"), NodeName);

	FString MethodName = TEXT("Execute");
	Params->TryGetStringField(TEXT("method_name"), MethodName);

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = CRB->GetDefaultModel();
	if (!Graph) return FMonolithActionResult::Error(TEXT("Control Rig has no default graph"));

	URigVMController* Controller = GetControllerForGraph(CRB, Graph, Error);
	if (!Controller) return FMonolithActionResult::Error(Error);

	// Begin transaction
	GEditor->BeginTransaction(FText::FromString(TEXT("Add Control Rig Node")));
	static_cast<UBlueprint*>(CRB)->Modify();

	// Add the unit node
	URigVMUnitNode* NewNode = Controller->AddUnitNodeFromStructPath(
		StructPath,
		FName(*MethodName),
		FVector2D(PosX, PosY),
		NodeName,
		/*bSetupUndoRedo=*/true,
		/*bPrintPythonCommand=*/false);

	if (!NewNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add unit node from struct path: %s"), *StructPath));
	}

	// Apply pin defaults if provided
	const TSharedPtr<FJsonObject>* PinDefaultsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsObj) && PinDefaultsObj && (*PinDefaultsObj)->Values.Num() > 0)
	{
		for (const auto& KV : FMonolithJsonUtils::GetFields(*PinDefaultsObj))
		{
			FString PinPath = FString::Printf(TEXT("%s.%s"), *NewNode->GetName(), *KV.Key);
			FString Value;

			// Handle different JSON value types
			if (KV.Value->Type == EJson::String)
			{
				Value = KV.Value->AsString();
			}
			else if (KV.Value->Type == EJson::Number)
			{
				Value = FString::SanitizeFloat(KV.Value->AsNumber());
			}
			else if (KV.Value->Type == EJson::Boolean)
			{
				Value = KV.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				// For objects/arrays, serialize to string
				FString JsonStr;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
				FJsonSerializer::Serialize(KV.Value, MonolithKeyToString(KV.Key), Writer);
				Value = JsonStr;
			}

			bool bSetOk = Controller->SetPinDefaultValue(PinPath, Value, /*bResizeArrays=*/true, /*bSetupUndoRedo=*/true);
			if (!bSetOk)
			{
				UE_LOG(LogTemp, Warning, TEXT("Monolith: Failed to set pin default %s = %s"), *PinPath, *Value);
			}
		}
	}

	// Reinit the VM
	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	GEditor->EndTransaction();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_name"), NewNode->GetName());
	Root->SetStringField(TEXT("node_path"), NewNode->GetNodePath());

	UScriptStruct* SS = NewNode->GetScriptStruct();
	if (SS)
	{
		Root->SetStringField(TEXT("struct_path"), SS->GetPathName());
		Root->SetStringField(TEXT("struct_name"), SS->GetName());
	}

	FVector2D Pos = NewNode->GetPosition();
	Root->SetNumberField(TEXT("position_x"), Pos.X);
	Root->SetNumberField(TEXT("position_y"), Pos.Y);

	// Return pin names for reference
	TArray<TSharedPtr<FJsonValue>> PinNames;
	PinNames.Reserve(NewNode->GetPins().Num());
	for (URigVMPin* Pin : NewNode->GetPins())
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->GetName());
		PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
		PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
		PinNames.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Root->SetArrayField(TEXT("pins"), PinNames);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// connect_control_rig_pins
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleConnectControlRigPins(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString SourcePin;
	if (!Params->TryGetStringField(TEXT("source_pin"), SourcePin) || SourcePin.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_pin is required"));
	}

	FString TargetPin;
	if (!Params->TryGetStringField(TEXT("target_pin"), TargetPin) || TargetPin.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("target_pin is required"));
	}

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = CRB->GetDefaultModel();
	if (!Graph) return FMonolithActionResult::Error(TEXT("Control Rig has no default graph"));

	URigVMController* Controller = GetControllerForGraph(CRB, Graph, Error);
	if (!Controller) return FMonolithActionResult::Error(Error);

	// Begin transaction
	GEditor->BeginTransaction(FText::FromString(TEXT("Connect Control Rig Pins")));
	static_cast<UBlueprint*>(CRB)->Modify();

	bool bSuccess = Controller->AddLink(
		SourcePin,
		TargetPin,
		/*bSetupUndoRedo=*/true,
		/*bPrintPythonCommand=*/false);

	if (!bSuccess)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to connect pins: %s -> %s (check pin paths and type compatibility)"),
			*SourcePin, *TargetPin));
	}

	// Reinit the VM
	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_pin"), SourcePin);
	Root->SetStringField(TEXT("target_pin"), TargetPin);
	Root->SetBoolField(TEXT("connected"), true);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleEditControlRigArrayPin(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString PinPath;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("pin_path"), PinPath) || PinPath.IsEmpty())
	{
		return InvalidParam(TEXT("pin_path is required"));
	}

	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("pin_path"), PinPath);

	bool bChanged = false;
	FString NewPinPath;
	if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase))
	{
		NewPinPath = Controller->AddArrayPin(PinPath, DefaultValue, true, false);
		bChanged = !NewPinPath.IsEmpty();
	}
	else if (Operation.Equals(TEXT("insert"), ESearchCase::IgnoreCase))
	{
		int32 Index = INDEX_NONE;
		if (!Params->TryGetNumberField(TEXT("index"), Index))
		{
			return InvalidParam(TEXT("index is required for operation=insert"));
		}
		NewPinPath = Controller->InsertArrayPin(PinPath, Index, DefaultValue, true, false);
		bChanged = !NewPinPath.IsEmpty();
		Result->SetNumberField(TEXT("index"), Index);
	}
	else if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
	{
		bChanged = Controller->RemoveArrayPin(PinPath, true, false);
	}
	else if (Operation.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
	{
		bChanged = Controller->ClearArrayPin(PinPath, true, false);
	}
	else if (Operation.Equals(TEXT("duplicate"), ESearchCase::IgnoreCase))
	{
		NewPinPath = Controller->DuplicateArrayPin(PinPath, true, false);
		bChanged = !NewPinPath.IsEmpty();
	}
	else if (Operation.Equals(TEXT("set_size"), ESearchCase::IgnoreCase))
	{
		int32 Size = INDEX_NONE;
		if (!Params->TryGetNumberField(TEXT("size"), Size) || Size < 0)
		{
			return InvalidParam(TEXT("size >= 0 is required for operation=set_size"));
		}
		bChanged = Controller->SetArrayPinSize(PinPath, Size, DefaultValue, true, false);
		Result->SetNumberField(TEXT("size"), Size);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: add, insert, remove, clear, duplicate, set_size"));
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig array pin operation failed: %s on %s"), *Operation, *PinPath));
	}

	if (!NewPinPath.IsEmpty())
	{
		Result->SetStringField(TEXT("new_pin_path"), NewPinPath);
	}
	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleBindControlRigPinVariable(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString PinPath;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("pin_path"), PinPath) || PinPath.IsEmpty())
	{
		return InvalidParam(TEXT("pin_path is required"));
	}

	bool bChanged = false;
	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("pin_path"), PinPath);

	if (Operation.Equals(TEXT("bind"), ESearchCase::IgnoreCase))
	{
		FString VariablePath;
		if (!Params->TryGetStringField(TEXT("variable_path"), VariablePath) || VariablePath.IsEmpty())
		{
			return InvalidParam(TEXT("variable_path is required for operation=bind"));
		}
		bChanged = Controller->BindPinToVariable(PinPath, VariablePath, true, false);
		Result->SetStringField(TEXT("variable_path"), VariablePath);
	}
	else if (Operation.Equals(TEXT("unbind"), ESearchCase::IgnoreCase))
	{
		bChanged = Controller->UnbindPinFromVariable(PinPath, true, false);
	}
	else if (Operation.Equals(TEXT("promote"), ESearchCase::IgnoreCase))
	{
		bool bCreateVariableNode = false;
		Params->TryGetBoolField(TEXT("create_variable_node"), bCreateVariableNode);
		double X = 0.0;
		double Y = 0.0;
		Params->TryGetNumberField(TEXT("position_x"), X);
		Params->TryGetNumberField(TEXT("position_y"), Y);
		bChanged = Controller->PromotePinToVariable(PinPath, bCreateVariableNode, FVector2D(X, Y), true, false);
		Result->SetBoolField(TEXT("create_variable_node"), bCreateVariableNode);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: bind, unbind, promote"));
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig pin variable operation failed: %s on %s"), *Operation, *PinPath));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleSetControlRigPinMetadata(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString PinPath;
	if (!Params->TryGetStringField(TEXT("pin_path"), PinPath) || PinPath.IsEmpty())
	{
		return InvalidParam(TEXT("pin_path is required"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, TEXT("set_pin_metadata"));
	Result->SetStringField(TEXT("pin_path"), PinPath);

	bool bAppliedAny = false;
	bool bAllSucceeded = true;
	bool bExpanded = false;
	if (Params->TryGetBoolField(TEXT("expanded"), bExpanded))
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->SetPinExpansion(PinPath, bExpanded, true, false);
		Result->SetBoolField(TEXT("expanded"), bExpanded);
	}

	bool bClearCategory = false;
	Params->TryGetBoolField(TEXT("clear_category"), bClearCategory);
	FString Category;
	if (bClearCategory)
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->ClearPinCategory(PinPath, true, false);
		Result->SetBoolField(TEXT("clear_category"), true);
	}
	else if (Params->TryGetStringField(TEXT("category"), Category))
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->SetPinCategory(PinPath, Category, true, false);
		Result->SetStringField(TEXT("category"), Category);
	}

	if (!bAppliedAny)
	{
		return InvalidParam(TEXT("At least one of expanded, category, or clear_category is required"));
	}
	if (!bAllSucceeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set Control Rig pin metadata for %s"), *PinPath));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleManageControlRigExposedPin(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString Name;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return InvalidParam(TEXT("name is required"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("name"), Name);
	bool bChanged = false;

	if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase))
	{
		FString Direction;
		FString CppType;
		FString TypeObjectPath;
		FString DefaultValue;
		Params->TryGetStringField(TEXT("direction"), Direction);
		if (!Params->TryGetStringField(TEXT("cpp_type"), CppType) || CppType.IsEmpty())
		{
			return InvalidParam(TEXT("cpp_type is required for operation=add"));
		}
		Params->TryGetStringField(TEXT("cpp_type_object_path"), TypeObjectPath);
		Params->TryGetStringField(TEXT("default_value"), DefaultValue);
		const FName NewName = Controller->AddExposedPin(FName(*Name), ParseRigVMPinDirection(Direction), CppType, TypeObjectPath.IsEmpty() ? NAME_None : FName(*TypeObjectPath), DefaultValue, true, false);
		bChanged = !NewName.IsNone();
		Result->SetStringField(TEXT("new_name"), NewName.ToString());
	}
	else if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
	{
		bChanged = Controller->RemoveExposedPin(FName(*Name), true, false);
	}
	else if (Operation.Equals(TEXT("rename"), ESearchCase::IgnoreCase))
	{
		FString NewName;
		if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		{
			return InvalidParam(TEXT("new_name is required for operation=rename"));
		}
		bChanged = Controller->RenameExposedPin(FName(*Name), FName(*NewName), true, false);
		Result->SetStringField(TEXT("new_name"), NewName);
	}
	else if (Operation.Equals(TEXT("change_type"), ESearchCase::IgnoreCase))
	{
		FString CppType;
		FString TypeObjectPath;
		if (!Params->TryGetStringField(TEXT("cpp_type"), CppType) || CppType.IsEmpty())
		{
			return InvalidParam(TEXT("cpp_type is required for operation=change_type"));
		}
		Params->TryGetStringField(TEXT("cpp_type_object_path"), TypeObjectPath);
		bool bSetupUndoRedo = true;
		bChanged = Controller->ChangeExposedPinType(FName(*Name), CppType, TypeObjectPath.IsEmpty() ? NAME_None : FName(*TypeObjectPath), bSetupUndoRedo, true, false);
		Result->SetStringField(TEXT("cpp_type"), CppType);
	}
	else if (Operation.Equals(TEXT("reorder"), ESearchCase::IgnoreCase))
	{
		int32 NewIndex = INDEX_NONE;
		if (!Params->TryGetNumberField(TEXT("new_index"), NewIndex))
		{
			return InvalidParam(TEXT("new_index is required for operation=reorder"));
		}
		bChanged = Controller->SetExposedPinIndex(FName(*Name), NewIndex, true, false);
		Result->SetNumberField(TEXT("new_index"), NewIndex);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: add, remove, rename, change_type, reorder"));
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig exposed pin operation failed: %s on %s"), *Operation, *Name));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleManageControlRigLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString Name;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return InvalidParam(TEXT("name is required"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("name"), Name);
	bool bChanged = false;

	if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase))
	{
		FString CppType;
		FString TypeObjectPath;
		FString DefaultValue;
		if (!Params->TryGetStringField(TEXT("cpp_type"), CppType) || CppType.IsEmpty())
		{
			return InvalidParam(TEXT("cpp_type is required for operation=add"));
		}
		Params->TryGetStringField(TEXT("cpp_type_object_path"), TypeObjectPath);
		Params->TryGetStringField(TEXT("default_value"), DefaultValue);
		FRigVMGraphVariableDescription Desc = TypeObjectPath.IsEmpty()
			? Controller->AddLocalVariable(FName(*Name), CppType, nullptr, DefaultValue, true, false)
			: Controller->AddLocalVariableFromObjectPath(FName(*Name), CppType, TypeObjectPath, DefaultValue, true);
		bChanged = !Desc.Name.IsNone();
		Result->SetStringField(TEXT("cpp_type"), CppType);
	}
	else if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
	{
		bChanged = Controller->RemoveLocalVariable(FName(*Name), true, false);
	}
	else if (Operation.Equals(TEXT("rename"), ESearchCase::IgnoreCase))
	{
		FString NewName;
		if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		{
			return InvalidParam(TEXT("new_name is required for operation=rename"));
		}
		bChanged = Controller->RenameLocalVariable(FName(*Name), FName(*NewName), true, false);
		Result->SetStringField(TEXT("new_name"), NewName);
	}
	else if (Operation.Equals(TEXT("set_type"), ESearchCase::IgnoreCase))
	{
		FString CppType;
		FString TypeObjectPath;
		if (!Params->TryGetStringField(TEXT("cpp_type"), CppType) || CppType.IsEmpty())
		{
			return InvalidParam(TEXT("cpp_type is required for operation=set_type"));
		}
		Params->TryGetStringField(TEXT("cpp_type_object_path"), TypeObjectPath);
		bChanged = TypeObjectPath.IsEmpty()
			? Controller->SetLocalVariableType(FName(*Name), CppType, nullptr, true, false)
			: Controller->SetLocalVariableTypeFromObjectPath(FName(*Name), CppType, TypeObjectPath, true, false);
		Result->SetStringField(TEXT("cpp_type"), CppType);
	}
	else if (Operation.Equals(TEXT("set_default"), ESearchCase::IgnoreCase))
	{
		FString DefaultValue;
		if (!Params->TryGetStringField(TEXT("default_value"), DefaultValue))
		{
			return InvalidParam(TEXT("default_value is required for operation=set_default"));
		}
		bChanged = Controller->SetLocalVariableDefaultValue(FName(*Name), DefaultValue, true, false);
		Result->SetStringField(TEXT("default_value"), DefaultValue);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: add, remove, rename, set_type, set_default"));
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig local variable operation failed: %s on %s"), *Operation, *Name));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleSetControlRigNodeMetadata(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeName;
	if (!Params->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
	{
		return InvalidParam(TEXT("node_name is required"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, TEXT("set_node_metadata"));
	Result->SetStringField(TEXT("node_name"), NodeName);

	bool bAppliedAny = false;
	bool bAllSucceeded = true;
	FString Value;
	if (Params->TryGetStringField(TEXT("category"), Value))
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->SetNodeCategoryByName(FName(*NodeName), Value, true, false);
		Result->SetStringField(TEXT("category"), Value);
	}
	if (Params->TryGetStringField(TEXT("keywords"), Value))
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->SetNodeKeywordsByName(FName(*NodeName), Value, true, false);
		Result->SetStringField(TEXT("keywords"), Value);
	}
	if (Params->TryGetStringField(TEXT("description"), Value))
	{
		bAppliedAny = true;
		bAllSucceeded &= Controller->SetNodeDescriptionByName(FName(*NodeName), Value, true, false);
		Result->SetStringField(TEXT("description"), Value);
	}

	if (!bAppliedAny)
	{
		return InvalidParam(TEXT("At least one of category, keywords, or description is required"));
	}
	if (!bAllSucceeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set Control Rig node metadata for %s"), *NodeName));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleCollapseControlRigNodes(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FName> NodeNames;
	if (!ParseNodeNameArray(Params, NodeNames, Error))
	{
		return InvalidParam(Error);
	}

	FString CollapseNodeName;
	Params->TryGetStringField(TEXT("collapse_node_name"), CollapseNodeName);
	URigVMCollapseNode* CollapseNode = Controller->CollapseNodes(NodeNames, CollapseNodeName, true, false);
	if (!CollapseNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to collapse Control Rig nodes"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, TEXT("collapse_nodes"));
	Result->SetStringField(TEXT("collapse_node_name"), CollapseNode->GetName());
	Result->SetStringField(TEXT("collapse_node_path"), CollapseNode->GetNodePath());
	Result->SetNumberField(TEXT("collapsed_count"), NodeNames.Num());
	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandlePromoteControlRigNode(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString NodeName;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
	{
		return InvalidParam(TEXT("node_name is required"));
	}

	FName NewNodeName = NAME_None;
	if (Operation.Equals(TEXT("collapse_to_function"), ESearchCase::IgnoreCase))
	{
		FString ExistingFunctionPath;
		Params->TryGetStringField(TEXT("existing_function_path"), ExistingFunctionPath);
		NewNodeName = Controller->PromoteCollapseNodeToFunctionReferenceNode(FName(*NodeName), true, false, ExistingFunctionPath);
	}
	else if (Operation.Equals(TEXT("function_to_collapse"), ESearchCase::IgnoreCase))
	{
		bool bRemoveDefinition = false;
		Params->TryGetBoolField(TEXT("remove_function_definition"), bRemoveDefinition);
		NewNodeName = Controller->PromoteFunctionReferenceNodeToCollapseNode(FName(*NodeName), true, false, bRemoveDefinition);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: collapse_to_function, function_to_collapse"));
	}

	if (NewNodeName.IsNone())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig node promotion failed: %s on %s"), *Operation, *NodeName));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("node_name"), NodeName);
	Result->SetStringField(TEXT("new_node_name"), NewNodeName.ToString());
	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithControlRigWriteActions::HandleManageControlRigTrait(const TSharedPtr<FJsonObject>& Params)
{
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	FString Error;
	if (!LoadRigContext(Params, CRB, Graph, Controller, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Operation;
	FString NodeName;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return InvalidParam(TEXT("operation is required"));
	}
	if (!Params->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
	{
		return InvalidParam(TEXT("node_name is required"));
	}

	TSharedPtr<FJsonObject> Result = BasicRigMutationResult(CRB, Graph, Operation);
	Result->SetStringField(TEXT("node_name"), NodeName);
	bool bChanged = false;

	if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase))
	{
		FString TraitTypeObjectPath;
		FString TraitName;
		FString DefaultValue;
		int32 PinIndex = -1;
		if (!Params->TryGetStringField(TEXT("trait_type_object_path"), TraitTypeObjectPath) || TraitTypeObjectPath.IsEmpty())
		{
			return InvalidParam(TEXT("trait_type_object_path is required for operation=add"));
		}
		Params->TryGetStringField(TEXT("trait_name"), TraitName);
		Params->TryGetStringField(TEXT("default_value"), DefaultValue);
		Params->TryGetNumberField(TEXT("pin_index"), PinIndex);
		const FName AddedName = Controller->AddTrait(FName(*NodeName), FName(*TraitTypeObjectPath), TraitName.IsEmpty() ? NAME_None : FName(*TraitName), DefaultValue, PinIndex, true, false);
		bChanged = !AddedName.IsNone();
		Result->SetStringField(TEXT("trait_name"), AddedName.ToString());
	}
	else if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
	{
		FString TraitName;
		if (!Params->TryGetStringField(TEXT("trait_name"), TraitName) || TraitName.IsEmpty())
		{
			return InvalidParam(TEXT("trait_name is required for operation=remove"));
		}
		bChanged = Controller->RemoveTrait(FName(*NodeName), FName(*TraitName), true, false);
		Result->SetStringField(TEXT("trait_name"), TraitName);
	}
	else
	{
		return InvalidParam(TEXT("operation must be one of: add, remove"));
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Control Rig trait operation failed: %s on %s"), *Operation, *NodeName));
	}

	FinalizeControlRigMutation(CRB, Result);
	return FMonolithActionResult::Success(Result);
}
