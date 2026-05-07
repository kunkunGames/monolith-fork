#include "MonolithAnimLayoutActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "IMonolithGraphFormatter.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimationStateGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAnimLayoutActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("auto_layout"),
		TEXT("Auto-layout nodes in an Animation Blueprint graph using Blueprint Assist. "
			 "Asset must be open in the editor. No built-in Monolith formatter exists for animation graphs."),
		FMonolithActionHandler::CreateStatic(&HandleAutoLayout),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph to layout: 'AnimGraph' (default), state machine name, or 'all' for every graph"), TEXT("AnimGraph"))
			.Optional(TEXT("formatter"), TEXT("string"),
				TEXT("Formatter: 'auto' (default, uses BA if available), 'blueprint_assist' (BA or error), 'monolith' (not supported for animation)"),
				TEXT("auto"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/** Collect all formattable graphs from an ABP: the main AnimGraph, all state machine graphs, and state inner graphs. */
void CollectAllGraphs(UAnimBlueprint* ABP, TArray<TPair<FString, UEdGraph*>>& OutGraphs)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;

		// Add the top-level graph (e.g. AnimGraph)
		OutGraphs.Add(TPair<FString, UEdGraph*>(Graph->GetName(), Graph));

		// Dig into state machine nodes
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			// The SM graph itself
			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			OutGraphs.Add(TPair<FString, UEdGraph*>(SMTitle, SMGraph));

			// Each state's inner graph
			for (UEdGraphNode* SMChild : SMGraph->Nodes)
			{
				UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChild);
				if (!StateNode || !StateNode->BoundGraph) continue;

				FString StateLabel = FString::Printf(TEXT("%s.%s"), *SMTitle, *StateNode->GetStateName());
				OutGraphs.Add(TPair<FString, UEdGraph*>(StateLabel, StateNode->BoundGraph));
			}
		}
	}
}

/** Find the main AnimGraph (first UAnimationGraph in FunctionGraphs). */
UEdGraph* FindAnimGraph(UAnimBlueprint* ABP)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (UAnimationGraph* AG = Cast<UAnimationGraph>(Graph))
		{
			return AG;
		}
	}
	return nullptr;
}

/** Find a state machine graph by display title. */
UEdGraph* FindSMGraphByTitle(UAnimBlueprint* ABP, const FString& MachineName)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (SMTitle == MachineName)
			{
				return Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			}
		}
	}
	return nullptr;
}

/** Format a single graph via IMonolithGraphFormatter. Returns a JSON object with results. */
TSharedPtr<FJsonObject> FormatSingleGraph(const FString& GraphLabel, UEdGraph* Graph, bool bExplicitBA, FString& OutError)
{
	bool bBAAvailable = IMonolithGraphFormatter::IsAvailable()
		&& IMonolithGraphFormatter::Get().SupportsGraph(Graph);

	if (!bBAAvailable)
	{
		if (bExplicitBA)
		{
			OutError = FString::Printf(
				TEXT("Blueprint Assist formatter not available or does not support graph '%s'. "
					 "Ensure Blueprint Assist plugin is installed and the asset is open in the editor."),
				*GraphLabel);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("No formatter available for graph '%s'. Install Blueprint Assist plugin and ensure the asset is open in the editor."),
				*GraphLabel);
		}
		return nullptr;
	}

	int32 NodesFormatted = 0;
	FString FormatError;
	bool bSuccess = IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, FormatError);

	if (!bSuccess)
	{
		OutError = FString::Printf(TEXT("Formatter failed on graph '%s': %s"), *GraphLabel, *FormatError);
		return nullptr;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("graph"), GraphLabel);
	ResultObj->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);
	ResultObj->SetStringField(TEXT("formatter_used"), TEXT("blueprint_assist"));
	return ResultObj;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Action: auto_layout
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimLayoutActions::HandleAutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString GraphName = TEXT("AnimGraph");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString Formatter = TEXT("auto");
	Params->TryGetStringField(TEXT("formatter"), Formatter);

	// Validate formatter param
	if (Formatter != TEXT("auto") && Formatter != TEXT("blueprint_assist") && Formatter != TEXT("monolith"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown formatter '%s'. Supported: 'auto', 'blueprint_assist', 'monolith'"), *Formatter));
	}

	// Monolith has no built-in animation graph formatter
	if (Formatter == TEXT("monolith"))
	{
		return FMonolithActionResult::Error(
			TEXT("No built-in Monolith formatter exists for animation graphs. Use formatter='auto' or install Blueprint Assist."));
	}

	// Load the AnimBlueprint
	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));
	}

	bool bExplicitBA = (Formatter == TEXT("blueprint_assist"));

	// --- "all" mode: format every graph ---
	if (GraphName.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		TArray<TPair<FString, UEdGraph*>> AllGraphs;
		CollectAllGraphs(ABP, AllGraphs);

		if (AllGraphs.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("No graphs found in this Animation Blueprint"));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("mode"), TEXT("all"));

		TArray<TSharedPtr<FJsonValue>> ResultsArr;
		TArray<TSharedPtr<FJsonValue>> ErrorsArr;
		int32 TotalFormatted = 0;

		for (const auto& Pair : AllGraphs)
		{
			FString Error;
			TSharedPtr<FJsonObject> GraphResult = FormatSingleGraph(Pair.Key, Pair.Value, bExplicitBA, Error);
			if (GraphResult)
			{
				TotalFormatted++;
				ResultsArr.Add(MakeShared<FJsonValueObject>(GraphResult));
			}
			else
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("graph"), Pair.Key);
				ErrObj->SetStringField(TEXT("error"), Error);
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
		}

		Root->SetArrayField(TEXT("formatted"), ResultsArr);
		Root->SetNumberField(TEXT("graphs_formatted"), TotalFormatted);
		Root->SetNumberField(TEXT("graphs_total"), AllGraphs.Num());

		if (ErrorsArr.Num() > 0)
		{
			Root->SetArrayField(TEXT("errors"), ErrorsArr);
		}

		if (TotalFormatted == 0)
		{
			// All graphs failed — return error with details
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to format any of %d graphs. Install Blueprint Assist and ensure the asset is open in the editor."),
				AllGraphs.Num()));
		}

		return FMonolithActionResult::Success(Root);
	}

	// --- Single graph mode ---
	UEdGraph* TargetGraph = nullptr;
	FString GraphLabel;

	if (GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase) || GraphName.IsEmpty())
	{
		TargetGraph = FindAnimGraph(ABP);
		GraphLabel = TEXT("AnimGraph");
		if (!TargetGraph)
		{
			return FMonolithActionResult::Error(TEXT("No AnimGraph found in this Animation Blueprint"));
		}
	}
	else
	{
		// Treat as state machine name
		TargetGraph = FindSMGraphByTitle(ABP, GraphName);
		GraphLabel = GraphName;
		if (!TargetGraph)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Graph '%s' not found. Use 'AnimGraph' for the main graph, a state machine name, or 'all'."),
				*GraphName));
		}
	}

	FString Error;
	TSharedPtr<FJsonObject> GraphResult = FormatSingleGraph(GraphLabel, TargetGraph, bExplicitBA, Error);
	if (!GraphResult)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Wrap in a top-level result
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph"), GraphLabel);
	Root->SetNumberField(TEXT("nodes_formatted"), GraphResult->GetNumberField(TEXT("nodes_formatted")));
	Root->SetStringField(TEXT("formatter_used"), GraphResult->GetStringField(TEXT("formatter_used")));

	return FMonolithActionResult::Success(Root);
}
