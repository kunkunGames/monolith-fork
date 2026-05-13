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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (defaults to first event graph)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("copy_nodes"),
		TEXT("Copy nodes from one graph to another using UE native T3D export/import. "
			"Internal connections are preserved; external connections are silently dropped. Node IDs change on copy."),
		FMonolithActionHandler::CreateStatic(&HandleCopyNodes),
		FParamSchemaBuilder()
			.Required(TEXT("source_asset"), TEXT("string"), TEXT("Source Blueprint asset path"))
			.Optional(TEXT("source_graph"), TEXT("string"), TEXT("Source graph name (defaults to first event graph)"))
			.Required(TEXT("node_ids"), TEXT("array"), TEXT("Array of node ID strings to copy"))
			.Required(TEXT("target_asset"), TEXT("string"), TEXT("Target Blueprint asset path"))
			.Optional(TEXT("target_graph"), TEXT("string"), TEXT("Target graph name (defaults to first event graph)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_graph"),
		TEXT("Duplicate a function or macro graph within the same Blueprint. "
			"Only works for function and macro graphs (not event graphs)."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("graph_name"), TEXT("string"), TEXT("Name of the graph to duplicate"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("Name for the duplicated graph"))
			.Build());
}

// ============================================================
//  Helper: Extended node serialization for export
// ============================================================

namespace
{
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
		return FMonolithActionResult::Error(TEXT("Missing required parameter: graph_name"));
	}

	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);
	if (NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"));
	}

	UEdGraph* SourceGraph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!SourceGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// Only allow duplication of function and macro graphs
	bool bIsFunction = BP->FunctionGraphs.Contains(SourceGraph);
	bool bIsMacro = BP->MacroGraphs.Contains(SourceGraph);

	if (!bIsFunction && !bIsMacro)
	{
		return FMonolithActionResult::Error(
			TEXT("Only function and macro graphs can be duplicated. Event graphs and delegate signature graphs are not supported."));
	}

	// Check if a graph with the new name already exists
	if (MonolithBlueprintInternal::FindGraphByName(BP, NewName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A graph named '%s' already exists in this Blueprint"), *NewName));
	}

	// Check the schema supports duplication
	const UEdGraphSchema* Schema = SourceGraph->GetSchema();
	if (!Schema || !Schema->CanDuplicateGraph(SourceGraph))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema does not allow duplication of graph '%s'"), *GraphName));
	}

	BP->Modify();

	// Duplicate via the graph schema (correct UE 5.7 pattern)
	UEdGraph* DuplicatedGraph = Schema->DuplicateGraph(SourceGraph);
	if (!DuplicatedGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate graph '%s'"), *GraphName));
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
