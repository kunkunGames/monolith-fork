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
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

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
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph to layout: 'AnimGraph' (default), state machine name, or 'all' for every graph"), TEXT("AnimGraph"))
			.Optional(TEXT("formatter"), TEXT("string"),
				TEXT("Formatter: 'auto' uses Blueprint Assist because no built-in Monolith formatter exists; 'blueprint_assist' forces Blueprint Assist; 'monolith' is not supported for animation"),
				TEXT("auto"))
			.Build());

	Registry.RegisterAction(TEXT("metahuman"), TEXT("get_status"),
		TEXT("Report read-only MetaHuman capability status without hard MetaHuman plugin dependencies or service calls."),
		FMonolithActionHandler::CreateStatic(&HandleGetMetaHumanStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("metahuman"), TEXT("list_character_assets"),
		TEXT("List MetaHuman-like assets under /Game using AssetRegistry metadata only. Does not load characters, build, rig, conform, or call services."),
		FMonolithActionHandler::CreateStatic(&HandleListMetaHumanAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content package path under /Game"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return, clamped to 1..500"), TEXT("100"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
int32 ClampMetaHumanLimit(double LimitValue)
{
	return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
}

bool IsMetaHumanLikeAssetClass(const FAssetData& AssetData)
{
	const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
	const FString ClassPath = AssetData.AssetClassPath.ToString();
	return ClassPath.Contains(TEXT("MetaHuman"))
		|| ClassPath.Contains(TEXT("MetaHumans"))
		|| ClassName.Contains(TEXT("MetaHuman"))
		|| ClassName.Contains(TEXT("MetaHumans"));
}

TSharedPtr<FJsonObject> MakeMetaHumanModuleStatus(const TCHAR* ModuleName)
{
	FModuleManager& ModuleManager = FModuleManager::Get();
	auto Status = MakeShared<FJsonObject>();
	Status->SetStringField(TEXT("name"), ModuleName);
	Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
	Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
	return Status;
}

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
	constexpr bool bHasBuiltInFormatter = false;
	if (!IMonolithGraphFormatter::IsExternalMutationFormattingEnabled(bHasBuiltInFormatter))
	{
		OutError = IMonolithGraphFormatter::GetExternalMutationFormattingDisabledMessage();
		return nullptr;
	}

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
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
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
			TEXT("No built-in Monolith formatter exists for animation graphs. Use formatter='auto' or formatter='blueprint_assist' with Blueprint Assist installed."));
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

// ---------------------------------------------------------------------------
// metahuman.get_status / metahuman.list_character_assets
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimLayoutActions::HandleGetMetaHumanStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("metahuman"));
	Result->SetStringField(TEXT("domain"), TEXT("metahuman_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);
	Result->SetBoolField(TEXT("service_calls"), false);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanCharacter"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanCharacterEditor"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanCharacterPalette"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanCoreTechLib"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanIdentity"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("MetaHumanIdentityEditor"))));
	Modules.Add(MakeShared<FJsonValueObject>(MakeMetaHumanModuleStatus(TEXT("RigLogic"))));
	Result->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.list_character_assets")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.get_metahuman_info")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.initialize_metahuman_from_preset")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.list_metahuman_wardrobe")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.request_metahuman_textures")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.auto_rig_metahuman")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("metahuman.build_metahuman")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This first milestone uses module reflection plus AssetRegistry metadata only; it does not load MetaHuman assets or call services.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Build, rig, conform, texture, wardrobe, and spawn workflows remain future work gated by compatible plugin APIs and confirm=true.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAnimLayoutActions::HandleListMetaHumanAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampMetaHumanLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Limit);
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (const FAssetData& AssetData : Assets)
	{
		if (!IsMetaHumanLikeAssetClass(AssetData))
		{
			continue;
		}

		MatchedCount++;
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassCounts.FindOrAdd(ClassName)++;

		if (Rows.Num() >= Limit)
		{
			continue;
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), ClassName);
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("metahuman"));
	Result->SetStringField(TEXT("domain"), TEXT("metahuman_discovery"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}
