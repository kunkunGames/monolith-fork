#include "MonolithBlueprintGraphExportActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "K2Node_Variable.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "EdGraphNode_Comment.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintGraphExportActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("export_graph"),
		TEXT("Export a Blueprint graph to JSON with full node data and a separate connections array. "
			"Output is compatible with build_blueprint_from_spec input format."),
		FMonolithActionHandler::CreateStatic(&HandleExportGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (defaults to first event graph)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("copy_nodes"),
		TEXT("Copy nodes from one graph to another using UE native T3D export/import. "
			"Internal connections are preserved; external connections are silently dropped. Node IDs change on copy."),
		FMonolithActionHandler::CreateStatic(&HandleCopyNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_asset"), TEXT("Source Blueprint asset path"))
			.Optional(TEXT("source_graph"), TEXT("string"), TEXT("Source graph name (defaults to first event graph)"))
			.Required(TEXT("node_ids"), TEXT("array"), TEXT("Array of node ID strings to copy"))
			.RequiredAssetPath(TEXT("target_asset"), TEXT("Target Blueprint asset path"))
			.Optional(TEXT("target_graph"), TEXT("string"), TEXT("Target graph name (defaults to first event graph)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_graph"),
		TEXT("Duplicate a function or macro graph within the same Blueprint. "
			"Only works for function and macro graphs (not event graphs)."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("graph_name"), TEXT("string"), TEXT("Name of the graph to duplicate"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("Name for the duplicated graph"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("clone_graphs_with_reference_remap"),
		TEXT("Clone function or macro graphs from one Blueprint into another while remapping hard and soft object/class references. "
			"Dry-run is the default; mutating calls require confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleCloneGraphsWithReferenceRemap),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_asset_path"), TEXT("Source Blueprint asset path"))
			.RequiredAssetPath(TEXT("destination_asset_path"), TEXT("Destination Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Single source graph name"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Destination graph name for graph_name"))
			.Optional(TEXT("graphs"), TEXT("array"), TEXT("Batch graph specs: strings or objects with source_graph and destination_graph/new_name"))
			.Optional(TEXT("class_remaps"), TEXT("object"), TEXT("Map source class path/name to destination class path/name"))
			.Optional(TEXT("object_remaps"), TEXT("object"), TEXT("Exact object path remaps used for hard/soft references"))
			.Optional(TEXT("root_remaps"), TEXT("object"), TEXT("Map source package roots to destination roots, e.g. {\"/Game/Old\":\"/Game/New\"}"))
			.Optional(TEXT("source_root"), TEXT("string"), TEXT("Single source root shorthand; must be supplied with dest_root"))
			.Optional(TEXT("dest_root"), TEXT("string"), TEXT("Single destination root shorthand; must be supplied with source_root"))
			.Optional(TEXT("allow_empty_remap"), TEXT("boolean"), TEXT("Allow cloning with no explicit class/object/root remap contract"), TEXT("false"))
			.Optional(TEXT("existing_policy"), TEXT("string"), TEXT("How to handle destination name collisions: fail, replace, or skip"), TEXT("fail")).Enum(TEXT("existing_policy"), { TEXT("fail"), TEXT("replace"), TEXT("skip") })
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the destination Blueprint after applying"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the destination package after applying"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan without mutating the destination Blueprint"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false"), TEXT("false"))
			.Build(),
		TEXT("PostCopyRepair"));

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("blueprint"), TEXT("clone_graphs_with_reference_remap"),
		{ TEXT("Blueprint graph clone"), TEXT("copy function graph"), TEXT("copy macro graph"), TEXT("reference remap"), TEXT("post-copy Blueprint repair") },
		{ TEXT("clone_graphs"), TEXT("copy_graphs_with_remap"), TEXT("remap_blueprint_graphs") },
		{ TEXT("clone HostSession helper functions from a source Blueprint into a copied destination Blueprint"), TEXT("dry-run function graph copy with root_remaps before applying") });
}

// ============================================================
//  Helper: Extended node serialization for export
// ============================================================

namespace
{
	TArray<TSharedPtr<FJsonValue>> GraphExportStringsToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	TArray<FString> DuplicateGraphAcceptedParameters()
	{
		TArray<FString> Values;
		Values.Add(TEXT("asset_path"));
		Values.Add(TEXT("graph_name"));
		Values.Add(TEXT("new_name"));
		return Values;
	}

	TArray<FString> DuplicateGraphSupportedGraphKinds()
	{
		TArray<FString> Values;
		Values.Add(TEXT("function"));
		Values.Add(TEXT("macro"));
		return Values;
	}

	template <typename GraphArrayType>
	TArray<FString> GraphNames(const GraphArrayType& Graphs)
	{
		TArray<FString> Names;
		Names.Reserve(Graphs.Num());
		for (const auto& GraphRef : Graphs)
		{
			const UEdGraph* Graph = GraphRef;
			if (Graph)
			{
				Names.Add(Graph->GetName());
			}
		}
		Names.Sort();
		return Names;
	}

	TArray<FString> FunctionGraphNames(const UBlueprint* BP)
	{
		return BP ? GraphNames(BP->FunctionGraphs) : TArray<FString>();
	}

	TArray<FString> MacroGraphNames(const UBlueprint* BP)
	{
		return BP ? GraphNames(BP->MacroGraphs) : TArray<FString>();
	}

	FString BlueprintGraphKind(const UBlueprint* BP, const UEdGraph* Graph, FString* OutInterfaceName = nullptr)
	{
		if (OutInterfaceName)
		{
			OutInterfaceName->Reset();
		}
		if (!BP || !Graph)
		{
			return TEXT("unknown");
		}

		UEdGraph* MutableGraph = const_cast<UEdGraph*>(Graph);
		if (BP->UbergraphPages.Contains(MutableGraph)) return TEXT("event_graph");
		if (BP->FunctionGraphs.Contains(MutableGraph)) return TEXT("function");
		if (BP->MacroGraphs.Contains(MutableGraph)) return TEXT("macro");
		if (BP->DelegateSignatureGraphs.Contains(MutableGraph)) return TEXT("delegate_signature");
		for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
		{
			if (Iface.Graphs.Contains(MutableGraph))
			{
				if (OutInterfaceName && Iface.Interface)
				{
					*OutInterfaceName = Iface.Interface->GetName();
				}
				return TEXT("interface");
			}
		}
		return TEXT("unknown");
	}

	void AddGraphCatalogEntry(const UBlueprint* BP, const UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutGraphs)
	{
		if (!BP || !Graph)
		{
			return;
		}

		FString InterfaceName;
		const FString Kind = BlueprintGraphKind(BP, Graph, &InterfaceName);
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph->GetName());
		GraphObj->SetStringField(TEXT("graph_kind"), Kind);
		GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		if (!InterfaceName.IsEmpty())
		{
			GraphObj->SetStringField(TEXT("interface"), InterfaceName);
		}
		OutGraphs.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	TArray<TSharedPtr<FJsonValue>> BlueprintGraphCatalogJsonValues(const UBlueprint* BP)
	{
		TArray<TSharedPtr<FJsonValue>> Graphs;
		if (!BP)
		{
			return Graphs;
		}

		TArray<UEdGraph*> AllGraphs;
		const_cast<UBlueprint*>(BP)->GetAllGraphs(AllGraphs);
		Graphs.Reserve(AllGraphs.Num());
		for (const UEdGraph* Graph : AllGraphs)
		{
			AddGraphCatalogEntry(BP, Graph, Graphs);
		}
		return Graphs;
	}

	TArray<TSharedPtr<FJsonValue>> DuplicableGraphCatalogJsonValues(const UBlueprint* BP)
	{
		TArray<TSharedPtr<FJsonValue>> Graphs;
		if (!BP)
		{
			return Graphs;
		}

		Graphs.Reserve(BP->FunctionGraphs.Num() + BP->MacroGraphs.Num());
		for (const auto& GraphRef : BP->FunctionGraphs)
		{
			const UEdGraph* Graph = GraphRef;
			AddGraphCatalogEntry(BP, Graph, Graphs);
		}
		for (const auto& GraphRef : BP->MacroGraphs)
		{
			const UEdGraph* Graph = GraphRef;
			AddGraphCatalogEntry(BP, Graph, Graphs);
		}
		return Graphs;
	}

	TSharedPtr<FJsonObject> DuplicateGraphReadArgs(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (!AssetPath.IsEmpty())
		{
			Args->SetStringField(TEXT("asset_path"), AssetPath);
		}
		return Args;
	}

	TSharedPtr<FJsonObject> MakeDuplicateGraphErrorData(
		const UBlueprint* BP,
		const FString& FailureCause,
		const FString& AssetPath,
		const FString& GraphName,
		const FString& NewName)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), FailureCause);
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		if (!GraphName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("offending_graph"), GraphName);
			ErrorData->SetStringField(TEXT("requested_graph_name"), GraphName);
		}
		if (!NewName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("requested_new_name"), NewName);
		}
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphExportStringsToJsonValues(DuplicateGraphAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("supported_graph_kinds"), GraphExportStringsToJsonValues(DuplicateGraphSupportedGraphKinds()));
		ErrorData->SetArrayField(TEXT("candidate_graphs"), DuplicableGraphCatalogJsonValues(BP));
		ErrorData->SetArrayField(TEXT("available_graphs"), BlueprintGraphCatalogJsonValues(BP));
		ErrorData->SetArrayField(TEXT("available_functions"), GraphExportStringsToJsonValues(FunctionGraphNames(BP)));
		ErrorData->SetArrayField(TEXT("available_macros"), GraphExportStringsToJsonValues(MacroGraphNames(BP)));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.list_graphs"));
		ErrorData->SetObjectField(TEXT("read_args"), DuplicateGraphReadArgs(AssetPath));
		return ErrorData;
	}

	void SetRecoveryHints(TSharedPtr<FJsonObject> ErrorData, const TArray<FString>& Hints)
	{
		if (ErrorData.IsValid())
		{
			ErrorData->SetArrayField(TEXT("recovery_hints"), GraphExportStringsToJsonValues(Hints));
		}
	}

	FString DuplicateGraphKindFailureCause(const FString& GraphKind)
	{
		if (GraphKind == TEXT("event_graph"))
		{
			return TEXT("event_graph_not_duplicable");
		}
		if (GraphKind == TEXT("delegate_signature"))
		{
			return TEXT("delegate_signature_not_duplicable");
		}
		if (GraphKind == TEXT("interface"))
		{
			return TEXT("interface_graph_not_duplicable");
		}
		return TEXT("graph_kind_not_duplicable");
	}

	/**
	 * Extended version of SerializeNode that adds extra type-specific fields:
	 * variable_name for Get/Set nodes, cast_class for DynamicCast, etc.
	 */
	TSharedPtr<FJsonObject> SerializeNodeExtended(UEdGraphNode* Node)
	{
		// Start with the standard serialization
		TSharedPtr<FJsonObject> NObj = MonolithBlueprintInternal::SerializeNode(Node);

		// Add variable_name for variable Get/Set nodes
		if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
		{
			FName VarName = VarNode->GetVarName();
			if (VarName != NAME_None)
			{
				NObj->SetStringField(TEXT("variable_name"), VarName.ToString());
			}
			if (VarNode->VariableReference.IsSelfContext())
			{
				NObj->SetBoolField(TEXT("is_self_context"), true);
			}
			else if (UClass* OwnerClass = VarNode->VariableReference.GetMemberParentClass())
			{
				NObj->SetStringField(TEXT("variable_class"), OwnerClass->GetName());
			}
		}

		// Add cast_class for DynamicCast nodes
		if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			if (CastNode->TargetType)
			{
				NObj->SetStringField(TEXT("cast_class"), CastNode->TargetType->GetName());
			}
		}

		// Add actor_class for SpawnActorFromClass nodes
		if (UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node))
		{
			UClass* SpawnClass = SpawnNode->GetClassToSpawn();
			if (SpawnClass)
			{
				NObj->SetStringField(TEXT("actor_class"), SpawnClass->GetName());
			}
		}

		// Add comment dimensions for comment nodes
		if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
		{
			NObj->SetNumberField(TEXT("comment_size_x"), CommentNode->NodeWidth);
			NObj->SetNumberField(TEXT("comment_size_y"), CommentNode->NodeHeight);
			// CommentColor is FLinearColor (float RGBA 0-1)
			NObj->SetStringField(TEXT("comment_color"),
				FString::Printf(TEXT("(%.3f,%.3f,%.3f,%.3f)"),
					CommentNode->CommentColor.R,
					CommentNode->CommentColor.G,
					CommentNode->CommentColor.B,
					CommentNode->CommentColor.A));
		}

		return NObj;
	}
}

// ============================================================
//  export_graph
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphExportActions::HandleExportGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("format_version"), 1);
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());

	// Determine graph type
	FString GraphType = TEXT("unknown");
	if (BP->UbergraphPages.Contains(Graph)) GraphType = TEXT("event_graph");
	else if (BP->FunctionGraphs.Contains(Graph)) GraphType = TEXT("function");
	else if (BP->MacroGraphs.Contains(Graph)) GraphType = TEXT("macro");
	else if (BP->DelegateSignatureGraphs.Contains(Graph)) GraphType = TEXT("delegate_signature");
	Root->SetStringField(TEXT("graph_type"), GraphType);

	// Serialize all nodes with extended info
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	NodesArr.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		NodesArr.Add(MakeShared<FJsonValueObject>(SerializeNodeExtended(Node)));
	}
	Root->SetArrayField(TEXT("nodes"), NodesArr);

	// Build separate connections array (material pattern)
	// Format: {from_node, from_pin, to_node, to_pin}
	// Only output each connection once (from the output side)
	TArray<TSharedPtr<FJsonValue>> ConnectionsArr;
	ConnectionsArr.Reserve(Graph->Nodes.Num() * 2);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
				TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
				Conn->SetStringField(TEXT("from_node"), Node->GetName());
				Conn->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
				Conn->SetStringField(TEXT("to_node"), LinkedPin->GetOwningNode()->GetName());
				Conn->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
				ConnectionsArr.Add(MakeShared<FJsonValueObject>(Conn));
			}
		}
	}
	Root->SetArrayField(TEXT("connections"), ConnectionsArr);

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  copy_nodes
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphExportActions::HandleCopyNodes(const TSharedPtr<FJsonObject>& Params)
{
	// Load source Blueprint
	FString SourceAssetPath;
	Params->TryGetStringField(TEXT("source_asset"), SourceAssetPath);
	if (SourceAssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: source_asset"));
	}
	UBlueprint* SourceBP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(SourceAssetPath);
	if (!SourceBP)
	{
		// Try level blueprint fallback
		SourceBP = MonolithBlueprintInternal::TryLoadLevelBlueprint(SourceAssetPath);
	}
	if (!SourceBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source Blueprint not found: %s"), *SourceAssetPath));
	}

	// Load target Blueprint
	FString TargetAssetPath;
	Params->TryGetStringField(TEXT("target_asset"), TargetAssetPath);
	if (TargetAssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: target_asset"));
	}
	UBlueprint* TargetBP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(TargetAssetPath);
	if (!TargetBP)
	{
		TargetBP = MonolithBlueprintInternal::TryLoadLevelBlueprint(TargetAssetPath);
	}
	if (!TargetBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target Blueprint not found: %s"), *TargetAssetPath));
	}

	// Find source graph
	FString SourceGraphName;
	Params->TryGetStringField(TEXT("source_graph"), SourceGraphName);
	UEdGraph* SourceGraph = MonolithBlueprintInternal::FindGraphByName(SourceBP, SourceGraphName);
	if (!SourceGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source graph not found: %s"), *SourceGraphName));
	}

	// Find target graph
	FString TargetGraphName;
	Params->TryGetStringField(TEXT("target_graph"), TargetGraphName);
	UEdGraph* TargetGraph = MonolithBlueprintInternal::FindGraphByName(TargetBP, TargetGraphName);
	if (!TargetGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target graph not found: %s"), *TargetGraphName));
	}

	// Parse node IDs
	const TArray<TSharedPtr<FJsonValue>>* NodeIdValues = nullptr;
	if (!Params->TryGetArrayField(TEXT("node_ids"), NodeIdValues) || !NodeIdValues || NodeIdValues->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: node_ids"));
	}

	// Collect source nodes
	TSet<UObject*> NodesToExport;
	TArray<FString> NotFound;
	for (const TSharedPtr<FJsonValue>& IdVal : *NodeIdValues)
	{
		FString NodeId = IdVal->AsString();
		UEdGraphNode* Node = nullptr;
		for (UEdGraphNode* N : SourceGraph->Nodes)
		{
			if (N && N->GetName() == NodeId)
			{
				Node = N;
				break;
			}
		}
		if (Node)
		{
			NodesToExport.Add(Node);
		}
		else
		{
			NotFound.Add(NodeId);
		}
	}

	if (NodesToExport.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("None of the specified nodes were found in graph '%s'. Not found: %s"),
			*SourceGraph->GetName(), *FString::Join(NotFound, TEXT(", "))));
	}

	// Export via T3D text
	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

	// Import into target graph
	TSet<UEdGraphNode*> ImportedNodes;
	FEdGraphUtilities::ImportNodesFromText(TargetGraph, ExportedText, ImportedNodes);

	// Mark target Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(TargetBP);

	// Build result
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source_asset"), SourceAssetPath);
	Root->SetStringField(TEXT("source_graph"), SourceGraph->GetName());
	Root->SetStringField(TEXT("target_asset"), TargetAssetPath);
	Root->SetStringField(TEXT("target_graph"), TargetGraph->GetName());
	Root->SetNumberField(TEXT("nodes_copied"), ImportedNodes.Num());

	TArray<TSharedPtr<FJsonValue>> NewNodeIds;
	for (UEdGraphNode* ImportedNode : ImportedNodes)
	{
		if (ImportedNode)
		{
			NewNodeIds.Add(MakeShared<FJsonValueString>(ImportedNode->GetName()));
		}
	}
	Root->SetArrayField(TEXT("new_node_ids"), NewNodeIds);

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& Id : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(Id));
		}
		Root->SetArrayField(TEXT("not_found"), NotFoundArr);
		Root->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("%d node(s) not found in source graph"), NotFound.Num()));
	}

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  duplicate_graph
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphExportActions::HandleDuplicateGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	if (GraphName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("missing_graph_name"), AssetPath, GraphName, FString());
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Call blueprint.list_graphs and retry with graph_name set to a function or macro graph."));
		RecoveryHints.Add(TEXT("duplicate_graph does not default to the event graph; graph_name is required."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(TEXT("Missing required parameter: graph_name"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Call blueprint.list_graphs, then retry with graph_name set to one of error_data.candidate_graphs[].name."))
			.WithRelatedAction(TEXT("blueprint.list_graphs"));
	}

	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);
	if (NewName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("missing_new_name"), AssetPath, GraphName, NewName);
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Provide new_name with a graph name that is not present in error_data.available_graphs."));
		RecoveryHints.Add(TEXT("Keep graph_name unchanged; new_name names the duplicated graph."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Provide new_name with a non-conflicting graph name."))
			.WithRelatedAction(TEXT("blueprint.list_graphs"));
	}

	UEdGraph* SourceGraph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!SourceGraph)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("graph_not_found"), AssetPath, GraphName, NewName);
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Retry with graph_name set to one of error_data.candidate_graphs[].name."));
		RecoveryHints.Add(TEXT("Use error_data.available_graphs[].graph_kind to distinguish function, macro, event, delegate, and interface graphs."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Retry with graph_name set to one of error_data.candidate_graphs[].name; event graphs are listed for diagnosis but cannot be duplicated."))
			.WithRelatedAction(TEXT("blueprint.list_graphs"));
	}

	// Only allow duplication of function and macro graphs
	bool bIsFunction = BP->FunctionGraphs.Contains(SourceGraph);
	bool bIsMacro = BP->MacroGraphs.Contains(SourceGraph);

	if (!bIsFunction && !bIsMacro)
	{
		FString InterfaceName;
		const FString GraphKind = BlueprintGraphKind(BP, SourceGraph, &InterfaceName);
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, DuplicateGraphKindFailureCause(GraphKind), AssetPath, GraphName, NewName);
		ErrorData->SetStringField(TEXT("graph_kind"), GraphKind);
		ErrorData->SetStringField(TEXT("source_graph"), SourceGraph->GetName());
		if (!InterfaceName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("interface"), InterfaceName);
		}
		ErrorData->SetStringField(TEXT("recovery"), TEXT("Use duplicate_graph only for function and macro graphs; choose one of error_data.candidate_graphs."));
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Function and macro graphs are duplicable; event graphs, delegate signatures, and interface graphs are not."));
		RecoveryHints.Add(TEXT("Use blueprint.list_graphs to inspect graph_kind before retrying."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(
			TEXT("Only function and macro graphs can be duplicated. Event graphs and delegate signature graphs are not supported."))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Use error_data.graph_kind to choose the correct recovery; duplicate_graph accepts only function and macro graphs."))
			.WithRelatedAction(TEXT("blueprint.list_graphs"));
	}

	// Check if a graph with the new name already exists
	if (UEdGraph* ExistingGraph = MonolithBlueprintInternal::FindGraphByName(BP, NewName))
	{
		const FString SourceGraphKind = bIsFunction ? TEXT("function") : TEXT("macro");
		FString ConflictingInterface;
		const FString ConflictingGraphKind = BlueprintGraphKind(BP, ExistingGraph, &ConflictingInterface);
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("name_conflict"), AssetPath, GraphName, NewName);
		ErrorData->SetStringField(TEXT("source_graph"), SourceGraph->GetName());
		ErrorData->SetStringField(TEXT("source_graph_kind"), SourceGraphKind);
		ErrorData->SetStringField(TEXT("candidate_name"), NewName);
		ErrorData->SetStringField(TEXT("conflicting_graph"), ExistingGraph->GetName());
		ErrorData->SetStringField(TEXT("conflicting_graph_kind"), ConflictingGraphKind);
		if (!ConflictingInterface.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("conflicting_interface"), ConflictingInterface);
		}
		ErrorData->SetStringField(TEXT("recovery"), TEXT("Choose a new_name that does not already appear in error_data.available_graphs[].name."));
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Pick a unique new_name; error_data.available_graphs includes every current graph name and kind."));
		RecoveryHints.Add(TEXT("If you intended to edit the existing graph, skip duplicate_graph and use that graph name directly."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(FString::Printf(TEXT("A graph named '%s' already exists in this Blueprint"), *NewName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Choose a non-conflicting new_name; error_data.available_graphs lists current names and graph kinds."))
			.WithRelatedAction(TEXT("blueprint.list_graphs"));
	}

	// Check the schema supports duplication
	const UEdGraphSchema* Schema = SourceGraph->GetSchema();
	if (!Schema || !Schema->CanDuplicateGraph(SourceGraph))
	{
		const FString SourceGraphKind = bIsFunction ? TEXT("function") : TEXT("macro");
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("copy_limitation"), AssetPath, GraphName, NewName);
		ErrorData->SetStringField(TEXT("copy_limitation_cause"), Schema ? TEXT("schema_refused_duplication") : TEXT("missing_graph_schema"));
		ErrorData->SetStringField(TEXT("source_graph"), SourceGraph->GetName());
		ErrorData->SetStringField(TEXT("source_graph_kind"), SourceGraphKind);
		if (Schema)
		{
			ErrorData->SetStringField(TEXT("schema_class"), Schema->GetClass()->GetName());
		}
		ErrorData->SetStringField(TEXT("recovery"), TEXT("The graph name and kind are valid, but the graph schema refused native duplication."));
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Inspect the source graph with blueprint.export_graph or blueprint.get_graph_summary."));
		RecoveryHints.Add(TEXT("Retry with another function or macro graph if schema_class cannot duplicate this graph."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema does not allow duplication of graph '%s'"), *GraphName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("The graph schema refused native duplication; error_data.copy_limitation_cause and schema_class explain why this is not a name or lookup failure."))
			.WithRelatedActions({ TEXT("blueprint.list_graphs"), TEXT("blueprint.export_graph"), TEXT("blueprint.get_graph_summary") });
	}

	BP->Modify();

	// Duplicate via the graph schema (correct UE 5.7 pattern)
	UEdGraph* DuplicatedGraph = Schema->DuplicateGraph(SourceGraph);
	if (!DuplicatedGraph)
	{
		const FString SourceGraphKind = bIsFunction ? TEXT("function") : TEXT("macro");
		TSharedPtr<FJsonObject> ErrorData = MakeDuplicateGraphErrorData(
			BP, TEXT("copy_limitation"), AssetPath, GraphName, NewName);
		ErrorData->SetStringField(TEXT("copy_limitation_cause"), TEXT("engine_duplicate_graph_returned_null"));
		ErrorData->SetStringField(TEXT("source_graph"), SourceGraph->GetName());
		ErrorData->SetStringField(TEXT("source_graph_kind"), SourceGraphKind);
		ErrorData->SetStringField(TEXT("schema_class"), Schema->GetClass()->GetName());
		ErrorData->SetStringField(TEXT("recovery"), TEXT("The graph schema accepted duplication, but the engine DuplicateGraph call returned no graph."));
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Inspect the source graph with blueprint.export_graph or blueprint.get_graph_summary before retrying."));
		RecoveryHints.Add(TEXT("Treat this as an engine/schema copy limitation, not a graph lookup or name-conflict error."));
		SetRecoveryHints(ErrorData, RecoveryHints);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate graph '%s'"), *GraphName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Native graph duplication returned null; error_data.copy_limitation_cause distinguishes this from graph_not_found or name_conflict."))
			.WithRelatedActions({ TEXT("blueprint.export_graph"), TEXT("blueprint.get_graph_summary") });
	}

	DuplicatedGraph->Modify();

	// Generate new GUIDs and component templates for all nodes
	for (UEdGraphNode* EdGraphNode : DuplicatedGraph->Nodes)
	{
		if (EdGraphNode)
		{
			EdGraphNode->CreateNewGuid();
		}
	}

	// Add the duplicated graph to the appropriate array
	if (bIsFunction)
	{
		BP->FunctionGraphs.Add(DuplicatedGraph);
	}
	else // bIsMacro
	{
		BP->MacroGraphs.Add(DuplicatedGraph);
	}

	// Rename to the desired name (must happen after adding to graph array)
	FBlueprintEditorUtils::RenameGraph(DuplicatedGraph, NewName);

	// Mark modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Build result
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_graph"), GraphName);
	Root->SetStringField(TEXT("new_graph"), DuplicatedGraph->GetName());
	Root->SetStringField(TEXT("graph_type"), bIsFunction ? TEXT("function") : TEXT("macro"));
	Root->SetNumberField(TEXT("node_count"), DuplicatedGraph->Nodes.Num());

	return FMonolithActionResult::Success(Root);
}
