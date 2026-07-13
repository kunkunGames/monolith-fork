#include "MonolithBlueprintCompileActions.h"
#include "MonolithAssetLifecycleActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "K2Node_Variable.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "MonolithBlueprintEditCradle.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> CreateBlueprintStringsToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	TArray<FString> CreateBlueprintAcceptedParameters()
	{
		TArray<FString> Values;
		Values.Add(TEXT("save_path"));
		Values.Add(TEXT("parent_class"));
		Values.Add(TEXT("blueprint_type"));
		Values.Add(TEXT("skip_save"));
		return Values;
	}

	TArray<FString> CreateBlueprintAcceptedTypes()
	{
		TArray<FString> Values;
		Values.Add(TEXT("Normal"));
		Values.Add(TEXT("Const"));
		Values.Add(TEXT("MacroLibrary"));
		Values.Add(TEXT("Interface"));
		Values.Add(TEXT("FunctionLibrary"));
		return Values;
	}

	TArray<FString> CreateBlueprintCandidateActions()
	{
		TArray<FString> Values;
		Values.Add(TEXT("blueprint.create_blueprint"));
		Values.Add(TEXT("blueprint.duplicate_blueprint"));
		Values.Add(TEXT("blueprint.save_asset"));
		Values.Add(TEXT("project.search"));
		return Values;
	}

	void AddUniqueCreateBlueprintLookupName(TArray<FString>& Names, const FString& Candidate)
	{
		const FString Trimmed = Candidate.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			Names.AddUnique(Trimmed);
		}
	}

	TArray<FString> CreateBlueprintClassLookupNames(const FString& ClassName)
	{
		TArray<FString> Names;
		const FString Trimmed = ClassName.TrimStartAndEnd();
		AddUniqueCreateBlueprintLookupName(Names, Trimmed);
		if (Trimmed.EndsWith(TEXT("_C")))
		{
			AddUniqueCreateBlueprintLookupName(Names, Trimmed.LeftChop(2));
		}
		if (!Trimmed.StartsWith(TEXT("A")))
		{
			AddUniqueCreateBlueprintLookupName(Names, TEXT("A") + Trimmed);
		}
		if (!Trimmed.StartsWith(TEXT("U")))
		{
			AddUniqueCreateBlueprintLookupName(Names, TEXT("U") + Trimmed);
		}
		if (Trimmed.Len() > 1 && (Trimmed.StartsWith(TEXT("A")) || Trimmed.StartsWith(TEXT("U"))))
		{
			AddUniqueCreateBlueprintLookupName(Names, Trimmed.Mid(1));
		}
		return Names;
	}

	UClass* ResolveCreateBlueprintParentClass(const FString& ClassName)
	{
		for (const FString& CandidateName : CreateBlueprintClassLookupNames(ClassName))
		{
			if (UClass* Candidate = FindFirstObject<UClass>(*CandidateName, EFindFirstObjectOptions::NativeFirst))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	struct FCreateBlueprintClassCandidate
	{
		UClass* Class = nullptr;
		int32 Score = 0;
	};

	int32 CreateBlueprintClassCandidateScore(const UClass* Class, const FString& Query)
	{
		if (!Class)
		{
			return 1000;
		}
		const FString CleanQuery = Query.TrimStartAndEnd();
		if (CleanQuery.IsEmpty())
		{
			return 500;
		}

		const FString Name = Class->GetName();
		const FString CppName = FString(Class->GetPrefixCPP()) + Name;
		if (Name.Equals(CleanQuery, ESearchCase::IgnoreCase) || CppName.Equals(CleanQuery, ESearchCase::IgnoreCase))
		{
			return 0;
		}
		if (Name.StartsWith(CleanQuery, ESearchCase::IgnoreCase) || CppName.StartsWith(CleanQuery, ESearchCase::IgnoreCase))
		{
			return 10;
		}
		if (Name.Contains(CleanQuery, ESearchCase::IgnoreCase) || CppName.Contains(CleanQuery, ESearchCase::IgnoreCase))
		{
			return 20;
		}
		return 1000;
	}

	TSharedPtr<FJsonObject> CreateBlueprintClassCandidateToJson(const UClass* Class)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Class)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Class->GetName());
		Obj->SetStringField(TEXT("cpp_name"), FString(Class->GetPrefixCPP()) + Class->GetName());
		Obj->SetStringField(TEXT("path"), Class->GetPathName());
		Obj->SetBoolField(TEXT("native"), Class->HasAnyClassFlags(CLASS_Native));
		Obj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> CreateBlueprintParentClassCandidatesToJsonValues(const FString& Query, int32 Limit = 12)
	{
		TArray<FCreateBlueprintClassCandidate> Candidates;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated))
			{
				continue;
			}
			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Class))
			{
				continue;
			}
			const int32 Score = CreateBlueprintClassCandidateScore(Class, Query);
			if (Score >= 1000)
			{
				continue;
			}
			FCreateBlueprintClassCandidate Candidate;
			Candidate.Class = Class;
			Candidate.Score = Score;
			Candidates.Add(Candidate);
		}

		if (Candidates.Num() == 0)
		{
			const TArray<FString> FallbackNames = {
				TEXT("Actor"),
				TEXT("Pawn"),
				TEXT("Character"),
				TEXT("ActorComponent"),
				TEXT("SceneComponent"),
				TEXT("Object")
			};
			for (const FString& FallbackName : FallbackNames)
			{
				if (UClass* Class = ResolveCreateBlueprintParentClass(FallbackName))
				{
					if (FKismetEditorUtilities::CanCreateBlueprintOfClass(Class))
					{
						FCreateBlueprintClassCandidate Candidate;
						Candidate.Class = Class;
						Candidate.Score = 900;
						Candidates.Add(Candidate);
					}
				}
			}
		}

		Candidates.Sort([](const FCreateBlueprintClassCandidate& A, const FCreateBlueprintClassCandidate& B)
		{
			if (A.Score != B.Score)
			{
				return A.Score < B.Score;
			}
			const FString AName = A.Class ? A.Class->GetName() : FString();
			const FString BName = B.Class ? B.Class->GetName() : FString();
			return AName < BName;
		});

		TArray<TSharedPtr<FJsonValue>> Out;
		const int32 Count = FMath::Min(Candidates.Num(), Limit);
		Out.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Out.Add(MakeShared<FJsonValueObject>(CreateBlueprintClassCandidateToJson(Candidates[Index].Class)));
		}
		return Out;
	}

	TArray<FString> CreateBlueprintRecoveryHints(const FString& FailureCause)
	{
		TArray<FString> Hints;
		if (FailureCause == TEXT("missing_save_path") || FailureCause == TEXT("invalid_save_path"))
		{
			Hints.Add(TEXT("Retry with save_path set to a package asset path such as /Game/Folder/BP_MyActor."));
			Hints.Add(TEXT("Use monolith.discover for the blueprint.create_blueprint parameter contract."));
		}
		else if (FailureCause == TEXT("missing_parent_class") || FailureCause == TEXT("parent_class_not_found"))
		{
			Hints.Add(TEXT("Retry with parent_class set to one of error_data.candidate_parent_classes[].name."));
			Hints.Add(TEXT("Common parent_class values include Actor, Pawn, Character, ActorComponent, and Object."));
		}
		else if (FailureCause == TEXT("target_path_exists"))
		{
			Hints.Add(TEXT("Choose a new save_path, or delete/rename the existing asset before retrying create_blueprint."));
			Hints.Add(TEXT("Use blueprint.duplicate_blueprint when the intent is to clone an existing Blueprint to a new_path."));
		}
		else
		{
			Hints.Add(TEXT("Use monolith.discover for the blueprint.create_blueprint parameter contract and retry with corrected inputs."));
		}
		return Hints;
	}

	TSharedPtr<FJsonObject> CreateBlueprintSchemaReadArgs()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("namespace"), TEXT("blueprint"));
		Args->SetStringField(TEXT("action"), TEXT("create_blueprint"));
		return Args;
	}

	TSharedPtr<FJsonObject> CreateBlueprintProjectSearchReadArgs(const FString& SavePath, const FString& AssetName)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("query"), AssetName.IsEmpty() ? SavePath : AssetName);
		Args->SetBoolField(TEXT("include_content"), false);
		Args->SetNumberField(TEXT("limit"), 5);
		return Args;
	}

	TSharedPtr<FJsonObject> MakeCreateBlueprintErrorData(
		const FString& FailureCause,
		const FString& SavePath,
		const FString& ParentClassName,
		const FString& BlueprintType,
		const FString& AssetName = FString(),
		bool bIncludeParentCandidates = false)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), FailureCause);
		if (!SavePath.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("save_path"), SavePath);
		}
		if (!AssetName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("asset_name"), AssetName);
		}
		if (!ParentClassName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("parent_class"), ParentClassName);
		}
		if (!BlueprintType.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("blueprint_type"), BlueprintType);
		}
		ErrorData->SetArrayField(TEXT("accepted_parameters"), CreateBlueprintStringsToJsonValues(CreateBlueprintAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("accepted_blueprint_types"), CreateBlueprintStringsToJsonValues(CreateBlueprintAcceptedTypes()));
		ErrorData->SetArrayField(TEXT("candidate_actions"), CreateBlueprintStringsToJsonValues(CreateBlueprintCandidateActions()));
		ErrorData->SetStringField(TEXT("schema_read_action"), TEXT("monolith.discover"));
		ErrorData->SetObjectField(TEXT("schema_read_args"), CreateBlueprintSchemaReadArgs());
		ErrorData->SetArrayField(TEXT("recovery_hints"), CreateBlueprintStringsToJsonValues(CreateBlueprintRecoveryHints(FailureCause)));

		if (bIncludeParentCandidates)
		{
			ErrorData->SetArrayField(TEXT("candidate_parent_classes"), CreateBlueprintParentClassCandidatesToJsonValues(ParentClassName));
		}

		if (FailureCause == TEXT("target_path_exists"))
		{
			ErrorData->SetStringField(TEXT("read_action"), TEXT("project.search"));
			ErrorData->SetObjectField(TEXT("read_args"), CreateBlueprintProjectSearchReadArgs(SavePath, AssetName));
			ErrorData->SetStringField(TEXT("offending_save_path"), SavePath);
		}
		else
		{
			ErrorData->SetStringField(TEXT("read_action"), TEXT("monolith.discover"));
			ErrorData->SetObjectField(TEXT("read_args"), CreateBlueprintSchemaReadArgs());
		}

		return ErrorData;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintCompileActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("compile_blueprint"),
		TEXT("Compile a Blueprint asset and return errors, warnings, and compile status."),
		FMonolithActionHandler::CreateStatic(&HandleCompileBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("validate_blueprint"),
		TEXT("Validate a Blueprint without compiling — returns unused variables, disconnected nodes, and compiler messages already stored on nodes."),
		FMonolithActionHandler::CreateStatic(&HandleValidateBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_blueprint"),
		TEXT("Create a new Blueprint asset at the given save path with the specified parent class and blueprint type."),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"),      TEXT("Asset save path, e.g. /Game/Test/BP_MyActor"))
			.Required(TEXT("parent_class"),   TEXT("string"),  TEXT("Parent class name, e.g. Actor, Pawn, Character"))
			.Optional(TEXT("blueprint_type"), TEXT("string"),  TEXT("Blueprint type: Normal, Const, MacroLibrary, Interface, FunctionLibrary (default: Normal)"), TEXT("Normal"))
			.Optional(TEXT("skip_save"),      TEXT("boolean"), TEXT("Skip the synchronous package save — Blueprint exists in-memory and can be saved later (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_blueprint"),
		TEXT("Duplicate an existing Blueprint asset to a new path."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Source Blueprint asset path"))
			.RequiredAssetPath(TEXT("new_path"),   TEXT("Destination asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_dependencies"),
		TEXT("Get asset dependencies for a Blueprint using the Asset Registry. Reports what the Blueprint depends on and/or what references it."),
		FMonolithActionHandler::CreateStatic(&HandleGetDependencies),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Optional(TEXT("direction"),  TEXT("string"), TEXT("depends_on, referenced_by, or both (default: both)"), TEXT("both"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("save_asset"),
		TEXT("Save a loaded asset to disk. Works on any asset type, not just Blueprints."),
		FMonolithActionHandler::CreateStatic(&HandleSaveAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to save"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("save_dirty_assets"),
		TEXT("Save ALL currently-dirty Blueprint and Widget Blueprint packages in one sweep (closes the data-loss window after a batch of edit actions that dirty but do not persist packages). Filter with path_prefix (default /Game). Returns saved[], failed[], count."),
		FMonolithActionHandler::CreateStatic(&HandleSaveDirtyAssets),
		FParamSchemaBuilder()
			.OptionalAssetPathWithDefault(TEXT("path_prefix"), TEXT("Only save assets whose package path starts with this prefix (default: /Game). Pass empty string to save all dirty Blueprint/Widget packages regardless of path."), TEXT("/Game"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("blueprint"), TEXT("create_blueprint"),
		{ TEXT("new blueprint asset"), TEXT("make a BP"), TEXT("subclass an actor in blueprint"), TEXT("blueprint from parent class"), TEXT("author new actor BP") },
		{ TEXT("new_blueprint"), TEXT("make_blueprint"), TEXT("add_blueprint"), TEXT("create_bp") },
		{ TEXT("create a new Actor blueprint at /Game/BP_MyActor"), TEXT("make a Character subclass blueprint"), TEXT("author a blueprint interface asset") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("blueprint"), TEXT("compile_blueprint"),
		{ TEXT("build the blueprint"), TEXT("check blueprint for errors"), TEXT("recompile BP"), TEXT("why does my blueprint not compile"), TEXT("blueprint compile errors") },
		{ TEXT("build_blueprint"), TEXT("recompile_blueprint"), TEXT("compile_bp") },
		{ TEXT("compile BP_Player and show errors"), TEXT("recompile the blueprint after editing nodes"), TEXT("check if BP_Enemy has compile errors") });
}

// ============================================================
//  compile_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

	// Build a map of compiler messages on nodes (node_id -> {graph_name, error_msg, severity})
	// This is the proven fallback approach using bHasCompilerMessage
	TMap<FString, TPair<FString, FString>> NodeErrorMap; // ErrorMsg -> {NodeId, GraphName}
	{
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !Node->bHasCompilerMessage) continue;
				if (!Node->ErrorMsg.IsEmpty())
				{
					NodeErrorMap.Add(Node->ErrorMsg, TPair<FString, FString>(Node->GetName(), Graph->GetName()));
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ErrorArr, WarnArr;
	ErrorArr.Reserve(Results.Messages.Num());
	WarnArr.Reserve(Results.Messages.Num());
	for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		FString MsgText = Msg->ToText().ToString();
		MsgObj->SetStringField(TEXT("message"), MsgText);

		// Try to match this message to a node with compiler errors
		for (const auto& Pair : NodeErrorMap)
		{
			if (MsgText.Contains(Pair.Key) || Pair.Key.Contains(MsgText))
			{
				MsgObj->SetStringField(TEXT("node_id"), Pair.Value.Key);
				MsgObj->SetStringField(TEXT("graph_name"), Pair.Value.Value);
				break;
			}
		}

		if (Msg->GetSeverity() == EMessageSeverity::Error)
		{
			ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
		else if (Msg->GetSeverity() == EMessageSeverity::Warning)
		{
			WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
	}

	FString StatusStr;
	switch (BP->Status)
	{
	case BS_Unknown:             StatusStr = TEXT("Unknown"); break;
	case BS_Dirty:               StatusStr = TEXT("Dirty"); break;
	case BS_Error:               StatusStr = TEXT("Error"); break;
	case BS_UpToDate:            StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	case BS_BeingCreated:        StatusStr = TEXT("BeingCreated"); break;
	default:                     StatusStr = TEXT("Unknown"); break;
	}

	bool bSuccess = (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);

	// Per-node compiler message walk — nodes carry bHasCompilerMessage independently
	// of Results.Messages. This field makes error locations actionable without text-matching.
	TArray<TSharedPtr<FJsonValue>> NodeMsgsArr;
	{
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !Node->bHasCompilerMessage) continue;
				TSharedPtr<FJsonObject> NMsg = MakeShared<FJsonObject>();
				NMsg->SetStringField(TEXT("node_id"),  Node->GetName());
				NMsg->SetStringField(TEXT("title"),    Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				NMsg->SetStringField(TEXT("message"),  Node->ErrorMsg);
				NMsg->SetStringField(TEXT("severity"), Node->ErrorType == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
				NMsg->SetStringField(TEXT("graph"),    Graph->GetName());
				NodeMsgsArr.Add(MakeShared<FJsonValueObject>(NMsg));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("success"), bSuccess);
	Root->SetStringField(TEXT("status"), StatusStr);
	Root->SetArrayField(TEXT("errors"), ErrorArr);
	Root->SetArrayField(TEXT("warnings"), WarnArr);
	Root->SetNumberField(TEXT("error_count"), ErrorArr.Num());
	Root->SetNumberField(TEXT("warning_count"), WarnArr.Num());
	Root->SetArrayField(TEXT("node_compiler_messages"), NodeMsgsArr);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  validate_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleValidateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	// --- Unused variables ---
	TArray<TSharedPtr<FJsonValue>> UnusedVars;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		bool bReferenced = false;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			TArray<UK2Node_Variable*> VarNodes;
			Graph->GetNodesOfClass<UK2Node_Variable>(VarNodes);
			for (UK2Node_Variable* VarNode : VarNodes)
			{
				if (VarNode->GetVarName() == Var.VarName)
				{
					bReferenced = true;
					break;
				}
			}
			if (bReferenced) break;
		}
		if (!bReferenced)
		{
			UnusedVars.Add(MakeShared<FJsonValueString>(Var.VarName.ToString()));
		}
	}

	// --- Disconnected nodes ---
	TArray<TSharedPtr<FJsonValue>> DisconnectedNodes;
	int32 TotalNodes = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TotalNodes += Graph->Nodes.Num();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>()) continue;
			if (Cast<UEdGraphNode_Comment>(Node)) continue;

			bool bHasExecInput = false;
			bool bExecInputConnected = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
				{
					bHasExecInput = true;
					if (Pin->LinkedTo.Num() > 0)
					{
						bExecInputConnected = true;
					}
				}
			}
			if (bHasExecInput && !bExecInputConnected)
			{
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("node_id"), Node->GetName());
				NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				UEdGraph* DNodeGraph = Node->GetGraph();
				NodeObj->SetStringField(TEXT("graph"), DNodeGraph ? DNodeGraph->GetName() : TEXT("(orphaned)"));
				DisconnectedNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}

	// --- Node errors ---
	TArray<TSharedPtr<FJsonValue>> NodeErrors;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Warning)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("node_id"), Node->GetName());
				ErrObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				ErrObj->SetStringField(TEXT("message"), Node->ErrorMsg);
				ErrObj->SetStringField(TEXT("severity"), Node->ErrorType == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
				UEdGraph* ErrNodeGraph = Node->GetGraph();
				ErrObj->SetStringField(TEXT("graph"), ErrNodeGraph ? ErrNodeGraph->GetName() : TEXT("(orphaned)"));
				NodeErrors.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
		}
	}

	// --- Unimplemented interface functions ---
	TArray<TSharedPtr<FJsonValue>> UnimplementedFuncs;

	// Collect all overridden event names across all graphs
	TSet<FName> OverriddenFuncNames;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TArray<UK2Node_Event*> EventNodes;
		Graph->GetNodesOfClass<UK2Node_Event>(EventNodes);
		for (UK2Node_Event* EN : EventNodes)
		{
			if (EN && EN->bOverrideFunction)
			{
				OverriddenFuncNames.Add(EN->EventReference.GetMemberName());
			}
		}
	}

	for (const FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
	{
		UClass* InterfaceClass = InterfaceDesc.Interface;
		if (!InterfaceClass) continue;

		// Collect names of graphs implemented for this interface
		TSet<FName> ImplementedGraphNames;
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph) ImplementedGraphNames.Add(Graph->GetFName());
		}

		for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)) continue;

			FName FuncName = Func->GetFName();
			if (!ImplementedGraphNames.Contains(FuncName) && !OverriddenFuncNames.Contains(FuncName))
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("interface"), InterfaceClass->GetName());
				Entry->SetStringField(TEXT("function"), FuncName.ToString());
				UnimplementedFuncs.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	// --- Duplicate custom events ---
	TArray<TSharedPtr<FJsonValue>> DuplicateEvents;
	TMap<FName, TArray<FString>> EventNameToGraphs;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TArray<UK2Node_CustomEvent*> CustomEventNodes;
		Graph->GetNodesOfClass<UK2Node_CustomEvent>(CustomEventNodes);
		for (UK2Node_CustomEvent* CE : CustomEventNodes)
		{
			if (!CE || CE->CustomFunctionName.IsNone()) continue;
			EventNameToGraphs.FindOrAdd(CE->CustomFunctionName).Add(Graph->GetName());
		}
	}
	for (auto& Pair : EventNameToGraphs)
	{
		if (Pair.Value.Num() > 1)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("event_name"), Pair.Key.ToString());
			Entry->SetNumberField(TEXT("count"), Pair.Value.Num());
			TArray<TSharedPtr<FJsonValue>> GraphArr;
			for (const FString& GN : Pair.Value)
			{
				GraphArr.Add(MakeShared<FJsonValueString>(GN));
			}
			Entry->SetArrayField(TEXT("graphs"), GraphArr);
			DuplicateEvents.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("unused_variables"), UnusedVars);
	Root->SetArrayField(TEXT("disconnected_nodes"), DisconnectedNodes);
	Root->SetArrayField(TEXT("node_errors"), NodeErrors);
	Root->SetArrayField(TEXT("unimplemented_interface_functions"), UnimplementedFuncs);
	Root->SetArrayField(TEXT("duplicate_custom_events"), DuplicateEvents);
	Root->SetNumberField(TEXT("total_graphs"), AllGraphs.Num());
	Root->SetNumberField(TEXT("total_nodes"), TotalNodes);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("missing_save_path"),
				SavePath,
				FString(),
				FString()));
	}

	FString ParentClassName;
	Params->TryGetStringField(TEXT("parent_class"), ParentClassName);
	if (ParentClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: parent_class"))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("missing_parent_class"),
				SavePath,
				ParentClassName,
				FString(),
				FString(),
				true));
	}

	FString BlueprintTypeStr;
	Params->TryGetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
	if (BlueprintTypeStr.IsEmpty())
	{
		BlueprintTypeStr = TEXT("Normal");
	}

	// Extract asset name from the save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("invalid_save_path"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("invalid_save_path"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr));
	}

	// Resolve parent class — try exact, common C++ prefixes, and stripped prefixes.
	UClass* ParentClass = ResolveCreateBlueprintParentClass(ParentClassName);
	if (!ParentClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("parent_class_not_found"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName,
				true))
			.WithHint(TEXT("Retry with parent_class set to one of error_data.candidate_parent_classes[].name."));
	}

	// Parse blueprint type
	EBlueprintType BPType = BPTYPE_Normal;
	if (BlueprintTypeStr == TEXT("Const"))               BPType = BPTYPE_Const;
	else if (BlueprintTypeStr == TEXT("MacroLibrary"))   BPType = BPTYPE_MacroLibrary;
	else if (BlueprintTypeStr == TEXT("Interface"))      BPType = BPTYPE_Interface;
	else if (BlueprintTypeStr == TEXT("FunctionLibrary")) BPType = BPTYPE_FunctionLibrary;

	// Guard: check if a Blueprint already exists at this path.
	// Asset Registry check covers on-disk assets not yet loaded (cold path).
	// FindObject covers in-memory assets from the current session.
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint already exists at '%s'. Use duplicate_blueprint or delete it first."), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("target_path_exists"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName))
			.WithHint(TEXT("Choose a different save_path, delete or rename the existing asset, or use blueprint.duplicate_blueprint to clone to a new_path."));
	}
	UBlueprint* ExistingBP = FindObject<UBlueprint>(nullptr, *(SavePath + TEXT(".") + AssetName));
	if (!ExistingBP)
	{
		UPackage* ExistingPkg = FindPackage(nullptr, *SavePath);
		if (ExistingPkg)
		{
			ExistingBP = FindObject<UBlueprint>(ExistingPkg, *AssetName);
		}
	}
	if (ExistingBP)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint already exists at '%s'. Use duplicate_blueprint or delete it first."), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("target_path_exists"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName))
			.WithHint(TEXT("Choose a different save_path, delete or rename the existing asset, or use blueprint.duplicate_blueprint to clone to a new_path."));
	}

	// Create the package — use the full save path as package name.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError)
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("invalid_save_path"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName));
	}
	// CreatePackage returns either the existing in-memory UPackage at this
	// path or a fresh RF_Public one (UObjectGlobals.cpp:1040-1050); it does
	// not touch disk. Canonical asset-create at AssetTools.cpp:1755-1772
	// uses CreatePackage's return value directly with no FullyLoad — calling
	// FullyLoad on the in-memory hit path forces a serialization read that
	// can pull stale RF_Transient flags from a leftover .uasset into the
	// in-memory package, then the subsequent SaveLoadedAsset writes the
	// transient state back to disk as partial bytes.
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("package_create_failed"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName));
	}

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Blueprint at: %s"), *SavePath))
			.WithErrorData(MakeCreateBlueprintErrorData(
				TEXT("blueprint_create_failed"),
				SavePath,
				ParentClassName,
				BlueprintTypeStr,
				AssetName,
				true));
	}

	// Read skip_save param (default false)
	bool bSkipSave = false;
	Params->TryGetBoolField(TEXT("skip_save"), bSkipSave);

	// CreateBlueprint already calls FBlueprintCompilationManager::CompileSynchronously
	// before returning (Kismet2.cpp:514-516), so the GeneratedClass and CDO are
	// fully initialized at this point. A second compile here triggers a
	// redundant reinstance pass which can carry RF_Transient onto the BPGC
	// when stale package state is involved (the HOFF 6 leak path observed
	// in the SquirrelTamagotchi 2026-04-30 session — four BPs created with
	// stale .uasset paths + multi-step set_cdo_property + overlapping prior
	// deletes returned saved:false; subsequent loads crashed at
	// LinkerLoad.cpp:5032 on serial-size-mismatch).

	// Fire edit cradle on CDO properties after compile (#29).
	// Uses FireFullCradle so root property is included in notification chain.
	UObject* CDO = NewBP->GeneratedClass ? NewBP->GeneratedClass->GetDefaultObject() : nullptr;
	if (CDO)
	{
		CDO->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintCompileActions",
			"CreateBlueprint", "Monolith Create Blueprint"));
		CDO->Modify();
		for (TFieldIterator<FProperty> It(CDO->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				continue;
			MonolithEditCradle::FireFullCradle(CDO, Prop);
		}
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	bool bSaved = false;
	if (!bSkipSave)
	{
		// SaveLoadedAsset is the safest path for newly-created assets —
		// it works on the loaded UObject directly and handles the package
		// state correctly.
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewBP, false);
	}

	FString GeneratedClassName;
	if (NewBP->GeneratedClass)
	{
		GeneratedClassName = NewBP->GeneratedClass->GetName();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Root->SetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
	Root->SetStringField(TEXT("generated_class"), GeneratedClassName);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  duplicate_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleDuplicateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString NewPath;
	Params->TryGetStringField(TEXT("new_path"), NewPath);
	if (NewPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_path"));
	}

	UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(AssetPath, NewPath);
	bool bSuccess = (Duplicated != nullptr);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("original_path"), AssetPath);
	Root->SetStringField(TEXT("new_asset_path"), NewPath);
	Root->SetBoolField(TEXT("success"), bSuccess);
	if (!bSuccess)
	{
		Root->SetStringField(TEXT("error"), FString::Printf(TEXT("DuplicateAsset failed — check that the destination path is valid and doesn't already exist: %s"), *NewPath));
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_dependencies
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleGetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString Direction;
	Params->TryGetStringField(TEXT("direction"), Direction);
	if (Direction.IsEmpty())
	{
		Direction = TEXT("both");
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FName PackageName = FName(*BP->GetPackage()->GetName());

	TArray<TSharedPtr<FJsonValue>> DependsOnArr;
	if (Direction == TEXT("depends_on") || Direction == TEXT("both"))
	{
		TArray<FAssetIdentifier> Dependencies;
		AR.GetDependencies(FAssetIdentifier(PackageName), Dependencies);
		for (const FAssetIdentifier& Dep : Dependencies)
		{
			DependsOnArr.Add(MakeShared<FJsonValueString>(Dep.ToString()));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReferencedByArr;
	if (Direction == TEXT("referenced_by") || Direction == TEXT("both"))
	{
		TArray<FAssetIdentifier> Referencers;
		AR.GetReferencers(FAssetIdentifier(PackageName), Referencers);
		for (const FAssetIdentifier& Ref : Referencers)
		{
			ReferencedByArr.Add(MakeShared<FJsonValueString>(Ref.ToString()));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("direction"), Direction);
	Root->SetArrayField(TEXT("depends_on"), DependsOnArr);
	Root->SetArrayField(TEXT("referenced_by"), ReferencedByArr);
	Root->SetNumberField(TEXT("depends_on_count"), DependsOnArr.Num());
	Root->SetNumberField(TEXT("referenced_by_count"), ReferencedByArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  save_asset
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleSaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithAssetLifecycleActions::SaveAsset(Params);
}

// --- save_dirty_assets (gap #10) ---
// Edit actions dirty packages but do not persist them. This sweep saves every dirty Blueprint /
// Widget Blueprint (UWidgetBlueprint : UBlueprint, so one iterator covers both) in a single call,
// reusing the exact SaveLoadedAsset path as save_asset. Closes the data-loss window if the editor
// closes between a batch of edits and a manual save.
FMonolithActionResult FMonolithBlueprintCompileActions::HandleSaveDirtyAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PathPrefix = TEXT("/Game");
	Params->TryGetStringField(TEXT("path_prefix"), PathPrefix); // empty string = no path filter

	TArray<TSharedPtr<FJsonValue>> SavedArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;

	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* BP = *It;
		if (!IsValid(BP))
		{
			continue;
		}
		UPackage* Pkg = BP->GetOutermost();
		if (!Pkg || Pkg == GetTransientPackage() || !Pkg->IsDirty())
		{
			continue;
		}
		const FString PkgName = Pkg->GetName();
		if (!PathPrefix.IsEmpty() && !PkgName.StartsWith(PathPrefix))
		{
			continue;
		}

		const FString AssetPath = BP->GetPathName();
		const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(BP, false);
		(bSaved ? SavedArr : FailedArr).Add(MakeShared<FJsonValueString>(AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("saved"), SavedArr);
	Root->SetArrayField(TEXT("failed"), FailedArr);
	Root->SetNumberField(TEXT("count"), SavedArr.Num());
	return FMonolithActionResult::Success(Root);
}
