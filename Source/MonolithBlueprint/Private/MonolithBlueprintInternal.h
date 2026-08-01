#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "MonolithAssetUtils.h"
#include "MonolithPinTypeGrammar.h"
#include "MonolithPropertyAccessReader.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "EdGraphNode_Comment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "UObject/Package.h"

namespace MonolithBlueprintInternal
{
	/**
	 * Try to resolve a Level Blueprint from a level asset path.
	 * Level Blueprints are ULevelScriptBlueprint sub-objects of ULevel,
	 * not top-level assets, so standard LoadAssetByPath<UBlueprint> won't find them.
	 */
	inline UBlueprint* TryLoadLevelBlueprint(const FString& AssetPath)
	{
		// Support "$current" sentinel — return the level BP of the currently-open level
		if (AssetPath == TEXT("$current"))
		{
			if (!GEditor) return nullptr;
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World || !World->PersistentLevel) return nullptr;
			// bDontCreate=false → create the level script BP if it doesn't exist yet
			return World->PersistentLevel->GetLevelScriptBlueprint(false);
		}

		// Try loading the level package
		UPackage* LevelPackage = LoadPackage(nullptr, *AssetPath, LOAD_NoWarn);
		if (!LevelPackage) return nullptr;

		// Find the UWorld in the package, then get its PersistentLevel
		UWorld* World = nullptr;
		ForEachObjectWithPackage(LevelPackage, [&World](UObject* Obj)
		{
			if (UWorld* W = Cast<UWorld>(Obj))
			{
				World = W;
				return false; // stop iteration
			}
			return true; // continue
		});

		if (!World || !World->PersistentLevel) return nullptr;

		// bDontCreate=true → don't create, just return nullptr if no level BP exists
		return World->PersistentLevel->GetLevelScriptBlueprint(true);
	}

	inline UBlueprint* LoadBlueprintFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath)
	{
		OutAssetPath = Params->GetStringField(TEXT("asset_path"));
		if (OutAssetPath.IsEmpty()) return nullptr;

		// Try standard Blueprint asset load first
		UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(OutAssetPath);
		if (BP) return BP;

		// Fallback: try as a Level Blueprint
		// Heuristic: "$current" sentinel, path contains "/Maps/", or standard load failed
		// (cheap to try — if it's not a level package, LoadPackage just returns null)
		return TryLoadLevelBlueprint(OutAssetPath);
	}

	inline void AddGraphArray(
		TArray<TSharedPtr<FJsonValue>>& OutArr,
		const TArray<TObjectPtr<UEdGraph>>& Graphs,
		const FString& Type,
		const FString& InterfaceName = FString())
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
			GObj->SetStringField(TEXT("name"), Graph->GetName());
			GObj->SetStringField(TEXT("type"), Type);
			GObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			// Disambiguate interface-implementation graphs by their interface (Gap 7).
			if (!InterfaceName.IsEmpty())
			{
				GObj->SetStringField(TEXT("interface"), InterfaceName);
			}
			OutArr.Add(MakeShared<FJsonValueObject>(GObj));
		}
	}

	inline UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName)
	{
		if (GraphName.IsEmpty() && BP->UbergraphPages.Num() > 0)
		{
			return BP->UbergraphPages[0];
		}

		auto SearchArray = [&](const TArray<TObjectPtr<UEdGraph>>& Arr) -> UEdGraph*
		{
			for (const auto& G : Arr)
			{
				if (G && G->GetName() == GraphName) return G;
			}
			return nullptr;
		};

		if (UEdGraph* G = SearchArray(BP->UbergraphPages)) return G;
		if (UEdGraph* G = SearchArray(BP->FunctionGraphs)) return G;
		if (UEdGraph* G = SearchArray(BP->MacroGraphs)) return G;
		if (UEdGraph* G = SearchArray(BP->DelegateSignatureGraphs)) return G;

		// Interface-implementation function graphs live on a separate array (Gap 7).
		for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
		{
			if (UEdGraph* G = SearchArray(Iface.Graphs)) return G;
		}

		// Nested graphs (collapsed-graph composites, macro-instance internals) are
		// not in any top-level array — resolve them via the recursive enumeration
		// so get_graph_data can read inside a composite by name.
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* G : AllGraphs)
		{
			if (G && G->GetName() == GraphName) return G;
		}
		return nullptr;
	}

	// Classify a graph against the Blueprint's top-level arrays. Nested graphs
	// (collapsed-graph composites, macro-instance internals) that are in no
	// top-level array classify as "subgraph" with OutParentGraph (if provided)
	// set to the nearest enclosing graph's name.
	inline FString ClassifyGraphType(const UBlueprint* BP, UEdGraph* Graph,
		FString& OutInterfaceName, FString* OutParentGraph = nullptr)
	{
		OutInterfaceName.Reset();
		if (BP->UbergraphPages.Contains(Graph))          return TEXT("event_graph");
		if (BP->FunctionGraphs.Contains(Graph))          return TEXT("function");
		if (BP->MacroGraphs.Contains(Graph))             return TEXT("macro");
		if (BP->DelegateSignatureGraphs.Contains(Graph)) return TEXT("delegate_signature");
		for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
		{
			if (Iface.Graphs.Contains(Graph))
			{
				if (Iface.Interface)
				{
					OutInterfaceName = Iface.Interface->GetName();
				}
				return TEXT("interface");
			}
		}
		for (const UObject* Outer = Graph->GetOuter(); Outer; Outer = Outer->GetOuter())
		{
			if (const UEdGraph* ParentGraph = Cast<UEdGraph>(Outer))
			{
				if (OutParentGraph)
				{
					*OutParentGraph = ParentGraph->GetName();
				}
				return TEXT("subgraph");
			}
		}
		return TEXT("unknown");
	}

	// The pin-type grammar (both directions) lives in one place —
	// MonolithCore/Public/MonolithPinTypeGrammar.h. These two thin forwarders keep
	// the ~20 existing MonolithBlueprintInternal:: call sites working unchanged.
	inline FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		return MonolithPinTypeGrammar::PinTypeToString(PinType);
	}

	inline FString ContainerPrefix(const FEdGraphPinType& PinType)
	{
		return MonolithPinTypeGrammar::ContainerPrefix(PinType);
	}

	inline TSharedPtr<FJsonObject> SerializePin(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("id"), Pin->PinId.ToString());
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"),
			ContainerPrefix(Pin->PinType) + PinTypeToString(Pin->PinType));

		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ConnArr;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			FString ConnId = FString::Printf(TEXT("%s.%s"),
				*Linked->GetOwningNode()->GetName(),
				*Linked->PinName.ToString());
			ConnArr.Add(MakeShared<FJsonValueString>(ConnId));
		}
		PinObj->SetArrayField(TEXT("connected_to"), ConnArr);
		return PinObj;
	}

	// ============================================================
	//  ResolveDefaultObjectForPin
	//
	//  Resolves the Value string to a UObject* / UClass* appropriate for
	//  a class-typed (PC_Class) or object-typed (PC_Object) pin's
	//  Pin->DefaultObject field. Performs cross-category check (class pin
	//  rejects instance, object pin rejects UClass) and type-constraint
	//  check against Pin->PinType.PinSubCategoryObject.
	//
	//  Returns nullptr and populates OutError on any failure. Does not
	//  mutate the pin.
	// ============================================================

	inline UObject* ResolveDefaultObjectForPin(UEdGraphPin* Pin, const FString& Value, FString& OutError)
	{
		if (!Pin)
		{
			OutError = TEXT("ResolveDefaultObjectForPin: null pin");
			return nullptr;
		}

		const FString PinPath = FString::Printf(TEXT("%s:%s"),
			Pin->GetOwningNode() ? *Pin->GetOwningNode()->GetName() : TEXT("?"),
			*Pin->PinName.ToString());

		const bool bIsClassPin  = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class);
		const bool bIsObjectPin = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object);

		if (!bIsClassPin && !bIsObjectPin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' is not class- or object-typed (category=%s)"),
				*PinPath, *Pin->PinType.PinCategory.ToString());
			return nullptr;
		}

		UObject* Resolved = nullptr;

		if (Value.Contains(TEXT("/")))
		{
			// Path resolution.
			Resolved = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);

			// BP class path retry: PC_Class needs a UClass (the GeneratedClass), not a UBlueprint asset.
			// Fire the retry when value lacks _C AND (load failed OR loaded object isn't a UClass).
			// '/Game/Foo/BP_Bar' loads the UBlueprint successfully — wrong kind for a class pin —
			// so the bare-null check alone misses this case.
			const bool bNeedsClassRetry = bIsClassPin && !Value.EndsWith(TEXT("_C")) &&
				(!Resolved || !Resolved->IsA(UClass::StaticClass()));
			if (bNeedsClassRetry)
			{
				int32 LastSlash = INDEX_NONE;
				if (Value.FindLastChar(TEXT('/'), LastSlash))
				{
					const FString Leaf = Value.Mid(LastSlash + 1);
					const FString RetryPath = FString::Printf(TEXT("%s.%s_C"), *Value, *Leaf);
					if (UObject* Retry = StaticLoadObject(UObject::StaticClass(), nullptr, *RetryPath))
					{
						if (Retry->IsA(UClass::StaticClass()))
						{
							Resolved = Retry;
						}
					}
				}
			}

			if (!Resolved)
			{
				OutError = FString::Printf(
					TEXT("Failed to resolve '%s' as object/class for pin '%s'. Path did not load."),
					*Value, *PinPath);
				return nullptr;
			}
		}
		else
		{
			// Bare-name resolution — PC_Class only.
			if (!bIsClassPin)
			{
				OutError = FString::Printf(
					TEXT("Pin '%s' is object-typed; bare name '%s' not accepted. Use an asset path."),
					*PinPath, *Value);
				return nullptr;
			}

			// UE stores class names without the C++ source-code prefix (APawn → "Pawn",
			// USelectableComponent → "SelectableComponent"). Try four forms in order:
			// (1) value as-is, (2) strip leading A/U, (3) add A prefix, (4) add U prefix.
			// This makes the resolver tolerant of either the C++ identifier form or the
			// engine-internal name. Prefix is invariably uppercase A/U per UE C++ naming
			// discipline, so case-sensitive StartsWith is correct here.
			UClass* ResolvedClass = FindFirstObject<UClass>(*Value, EFindFirstObjectOptions::NativeFirst);
			if (!ResolvedClass && Value.Len() > 1 && (Value.StartsWith(TEXT("A")) || Value.StartsWith(TEXT("U"))))
			{
				const FString Stripped = Value.Mid(1);
				ResolvedClass = FindFirstObject<UClass>(*Stripped, EFindFirstObjectOptions::NativeFirst);
			}
			if (!ResolvedClass && !Value.StartsWith(TEXT("A")))
			{
				ResolvedClass = FindFirstObject<UClass>(
					*FString::Printf(TEXT("A%s"), *Value), EFindFirstObjectOptions::NativeFirst);
			}
			if (!ResolvedClass && !Value.StartsWith(TEXT("U")))
			{
				ResolvedClass = FindFirstObject<UClass>(
					*FString::Printf(TEXT("U%s"), *Value), EFindFirstObjectOptions::NativeFirst);
			}
			if (!ResolvedClass)
			{
				OutError = FString::Printf(
					TEXT("Class '%s' not found (tried as-is, with A/U prefix stripped, and with A/U prefix added). Use a full path for BP classes."),
					*Value);
				return nullptr;
			}
			Resolved = ResolvedClass;
		}

		// Cross-category mismatch — class pin must hold a UClass; object pin must hold an instance.
		if (bIsClassPin && !Resolved->IsA(UClass::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("Pin '%s' expects a class, got instance '%s'. Use a class path or name instead."),
				*PinPath, *Value);
			return nullptr;
		}
		if (bIsObjectPin && Resolved->IsA(UClass::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("Pin '%s' expects an instance, got class '%s'. Use an asset path instead."),
				*PinPath, *Value);
			return nullptr;
		}

		// Type-constraint enforcement against PinSubCategoryObject (the pin's declared base type).
		UClass* PinBase = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
		if (PinBase)
		{
			if (bIsClassPin)
			{
				UClass* ResolvedClass = Cast<UClass>(Resolved);
				if (!ResolvedClass->IsChildOf(PinBase))
				{
					OutError = FString::Printf(
						TEXT("Resolved '%s' is not a subclass of '%s' required by pin '%s'."),
						*ResolvedClass->GetName(), *PinBase->GetName(), *PinPath);
					return nullptr;
				}
			}
			else // bIsObjectPin
			{
				if (!Resolved->IsA(PinBase))
				{
					OutError = FString::Printf(
						TEXT("Resolved '%s' is not an instance of '%s' required by pin '%s'."),
						*Resolved->GetName(), *PinBase->GetName(), *PinPath);
					return nullptr;
				}
			}
		}

		return Resolved;
	}

	// bSafeTitle: report the class name instead of calling GetNodeTitle(). Use for
	// generic-fallback nodes — some legacy node classes crash in GetNodeTitle() on
	// unconfigured state (UK2Node_SpawnActor null-derefs its Blueprint pin's DefaultObject).
	inline TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node, bool bSafeTitle = false)
	{
		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("id"), Node->GetName());
		NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NObj->SetStringField(TEXT("title"), bSafeTitle
			? Node->GetClass()->GetName()
			: Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
		NObj->SetArrayField(TEXT("pos"), PosArr);

		if (!Node->NodeComment.IsEmpty())
		{
			NObj->SetStringField(TEXT("comment"), Node->NodeComment);
		}

		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			NObj->SetStringField(TEXT("function"),
				CallNode->FunctionReference.GetMemberName().ToString());
			if (UClass* OwnerClass = CallNode->FunctionReference.GetMemberParentClass())
			{
				NObj->SetStringField(TEXT("function_class"), OwnerClass->GetName());
			}
		}
		else if (UK2Node_ComponentBoundEvent* BoundNode = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			NObj->SetStringField(TEXT("component_name"),
				BoundNode->ComponentPropertyName.ToString());
			NObj->SetStringField(TEXT("delegate_property_name"),
				BoundNode->DelegatePropertyName.ToString());
			NObj->SetStringField(TEXT("event_name"),
				BoundNode->EventReference.GetMemberName().ToString());
			if (BoundNode->CustomFunctionName != NAME_None)
			{
				NObj->SetStringField(TEXT("custom_name"),
					BoundNode->CustomFunctionName.ToString());
			}
			if (BoundNode->DelegateOwnerClass)
			{
				NObj->SetStringField(TEXT("delegate_owner_class"),
					BoundNode->DelegateOwnerClass->GetName());
			}
		}
		else if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
		{
			NObj->SetStringField(TEXT("delegate_property_name"),
				DelegateNode->DelegateReference.GetMemberName().ToString());
			if (UClass* DelegateOwner = DelegateNode->DelegateReference.GetMemberParentClass())
			{
				NObj->SetStringField(TEXT("delegate_owner_class"), DelegateOwner->GetName());
			}
			NObj->SetBoolField(TEXT("self_context"),
				DelegateNode->DelegateReference.IsSelfContext());
		}
		else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			NObj->SetStringField(TEXT("event_name"),
				EventNode->EventReference.GetMemberName().ToString());
			if (EventNode->CustomFunctionName != NAME_None)
			{
				NObj->SetStringField(TEXT("custom_name"),
					EventNode->CustomFunctionName.ToString());
			}
		}
		else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			if (MacroNode->GetMacroGraph())
			{
				NObj->SetStringField(TEXT("macro_name"),
					MacroNode->GetMacroGraph()->GetName());
			}
		}

		// UK2Node_PropertyAccess — emit the resolved property-access path (Gap 1).
		// The class is MinimalAPI/unlinkable, so this is a string-match + reflective read.
		MonolithPropertyAccessReader::SerializePropertyAccessBlock(Node, NObj);

		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			PinsArr.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
		NObj->SetArrayField(TEXT("pins"), PinsArr);

		return NObj;
	}

	inline TSharedPtr<FJsonObject> TraceExecFlow(
		UEdGraphNode* Node,
		TSet<UEdGraphNode*>& Visited,
		int32 MaxDepth = 100)
	{
		if (!Node || Visited.Contains(Node) || MaxDepth <= 0)
		{
			return nullptr;
		}
		Visited.Add(Node);

		TSharedPtr<FJsonObject> FlowObj = MakeShared<FJsonObject>();
		FlowObj->SetStringField(TEXT("node"),
			Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		FlowObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		TArray<UEdGraphPin*> ExecOutputs;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
				!Pin->bHidden)
			{
				ExecOutputs.Add(Pin);
			}
		}

		if (ExecOutputs.Num() == 1 && ExecOutputs[0]->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ThenArr;
			for (UEdGraphPin* Linked : ExecOutputs[0]->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;
				TSharedPtr<FJsonObject> Next = TraceExecFlow(
					Linked->GetOwningNode(), Visited, MaxDepth - 1);
				if (Next)
				{
					ThenArr.Add(MakeShared<FJsonValueObject>(Next));
				}
			}
			if (ThenArr.Num() > 0)
			{
				FlowObj->SetArrayField(TEXT("then"), ThenArr);
			}
		}
		else if (ExecOutputs.Num() > 1)
		{
			TSharedPtr<FJsonObject> BranchesObj = MakeShared<FJsonObject>();
			for (UEdGraphPin* ExecPin : ExecOutputs)
			{
				TArray<TSharedPtr<FJsonValue>> BranchArr;
				for (UEdGraphPin* Linked : ExecPin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					TSet<UEdGraphNode*> BranchVisited = Visited;
					TSharedPtr<FJsonObject> Next = TraceExecFlow(
						Linked->GetOwningNode(), BranchVisited, MaxDepth - 1);
					if (Next)
					{
						BranchArr.Add(MakeShared<FJsonValueObject>(Next));
					}
				}
				BranchesObj->SetArrayField(ExecPin->PinName.ToString(), BranchArr);
			}
			FlowObj->SetObjectField(TEXT("branches"), BranchesObj);
		}

		return FlowObj;
	}

	inline UEdGraphNode* FindEntryNode(UEdGraph* Graph, const FString& EntryPoint)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (EventName == EntryPoint || EventNode->GetName() == EntryPoint)
					return Node;
				if (EventNode->CustomFunctionName != NAME_None &&
					EventNode->CustomFunctionName.ToString() == EntryPoint)
					return Node;
				FString DisplayTitle = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (DisplayTitle.Contains(EntryPoint))
					return Node;
			}
			if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				if (Graph->GetName() == EntryPoint)
					return Node;
			}
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_FunctionEntry>(Node))
				continue;
			if (Cast<UEdGraphNode_Comment>(Node))
				continue;
			FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (Title.Contains(EntryPoint))
				return Node;
		}
		return nullptr;
	}

	// Find a node by its GetName() across all graphs or a specific graph.
	//
	// When GraphName is empty the search spans every graph. A node ID (GetName())
	// is only unique WITHIN a graph, so the same ID can legitimately exist in more
	// than one graph. If OutMatchGraphs is provided it is filled with the name of
	// every graph containing a match — the caller can then detect a cross-graph ID
	// collision (OutMatchGraphs.Num() > 1) and ask for a disambiguating graph_name.
	// The FIRST match is always returned (back-compat for callers that don't pass
	// OutMatchGraphs); with OutMatchGraphs set the scan continues so all matches
	// are recorded.
	inline UEdGraphNode* FindNodeById(UBlueprint* BP, const FString& GraphName, const FString& NodeId, TArray<FString>* OutMatchGraphs = nullptr)
	{
		auto SearchGraph = [&](UEdGraph* Graph) -> UEdGraphNode*
		{
			if (!Graph) return nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetName() == NodeId) return Node;
			}
			return nullptr;
		};

		if (!GraphName.IsEmpty())
		{
			UEdGraph* Graph = FindGraphByName(BP, GraphName);
			UEdGraphNode* Found = Graph ? SearchGraph(Graph) : nullptr;
			if (Found && OutMatchGraphs)
			{
				OutMatchGraphs->Add(Graph->GetName());
			}
			return Found;
		}

		// graph_name omitted → walk every graph array (same set as before:
		// Ubergraph, Function, Macro). Without OutMatchGraphs, stop at the first
		// match; with it, collect all matches for collision detection.
		UEdGraphNode* First = nullptr;
		const TArray<TObjectPtr<UEdGraph>>* AllArrays[] = { &BP->UbergraphPages, &BP->FunctionGraphs, &BP->MacroGraphs };
		for (const TArray<TObjectPtr<UEdGraph>>* Arr : AllArrays)
		{
			for (const TObjectPtr<UEdGraph>& G : *Arr)
			{
				if (!G) continue;
				if (UEdGraphNode* N = SearchGraph(G))
				{
					if (!First)
					{
						First = N;
					}
					if (OutMatchGraphs)
					{
						OutMatchGraphs->Add(G->GetName());
					}
					else
					{
						return First;
					}
				}
			}
		}
		return First;
	}

	// Build a comma-separated list of non-hidden pin names on a node (for error messages)
	inline FString GetAvailablePinNames(UEdGraphNode* Node, EEdGraphPinDirection Direction = EGPD_MAX)
	{
		if (!Node) return TEXT("(none)");
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			if (Direction != EGPD_MAX && Pin->Direction != Direction) continue;
			Names.Add(Pin->PinName.ToString());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	// Find a pin on a node by name and optional direction.
	// Tries exact match first, then case-insensitive fallback.
	// If OutAvailablePins is provided, it is populated with available pin names on failure.
	inline UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX, FString* OutAvailablePins = nullptr)
	{
		if (!Node) return nullptr;

		// Pass 1: exact match
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString() == PinName)
			{
				if (Direction == EGPD_MAX || Pin->Direction == Direction)
					return Pin;
			}
		}

		// Pass 2: case-insensitive fallback
		FString PinNameLower = PinName.ToLower();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().ToLower() == PinNameLower)
			{
				if (Direction == EGPD_MAX || Pin->Direction == Direction)
					return Pin;
			}
		}

		// No match — populate available pins for caller error messages
		if (OutAvailablePins)
		{
			*OutAvailablePins = GetAvailablePinNames(Node, Direction);
		}
		return nullptr;
	}

	/**
	 * Resolve any FObjectProperty by name on a Blueprint's generated class.
	 * Covers SCS / native ActorComponent subobjects (Actor BPs), UMG named
	 * widgets (Widget BPs — UButton, UTextBlock, etc.), and plain object-typed
	 * Blueprint variables — all compile into FObjectProperty entries on the
	 * generated class. Callers needing component/delegate validation must
	 * combine this with FindMulticastDelegateProperty (the effective filter).
	 * Returns null if no FObjectProperty with that name is found, or its
	 * PropertyClass is null.
	 */
	inline FObjectProperty* FindComponentProperty(UBlueprint* BP, FName ComponentName)
	{
		if (!BP || !BP->GeneratedClass) return nullptr;
		FObjectProperty* Prop = FindFProperty<FObjectProperty>(BP->GeneratedClass, ComponentName);
		if (!Prop || !Prop->PropertyClass) return nullptr;
		return Prop;
	}

	/**
	 * Find a BlueprintAssignable multicast delegate property on a class (or any superclass).
	 * Returns null if not found or not BlueprintAssignable.
	 */
	inline FMulticastDelegateProperty* FindMulticastDelegateProperty(UClass* OwnerClass, FName DelegateName)
	{
		if (!OwnerClass) return nullptr;
		FMulticastDelegateProperty* Prop = FindFProperty<FMulticastDelegateProperty>(OwnerClass, DelegateName);
		if (!Prop) return nullptr;
		if (!Prop->HasAnyPropertyFlags(CPF_BlueprintAssignable)) return nullptr;
		return Prop;
	}

	/**
	 * Resolves the delegate owner class + multicast property for a delegate-node
	 * add_node / resolve_node call (AddDelegate, RemoveDelegate, ClearDelegate,
	 * CallDelegate). Mirrors the prefix-normalization the editor's right-click
	 * menu performs — accepts bare and A/U-prefixed class names. SelfContextClass
	 * is used when 'target_class' is empty (e.g. self-context: BP->GeneratedClass);
	 * pass nullptr if no self-context fallback should be attempted.
	 *
	 * NodeTypeLabel goes into error messages (e.g. "AddDelegate", "RemoveDelegate")
	 * so the caller sees which node type's required parameter was missing.
	 *
	 * On success, returns a Success result with OutOwnerClass / OutDelegateProp /
	 * OutbSelfContext populated. On failure, returns an Error result; outputs are
	 * only meaningful when bSuccess is true — do not read them on the error path.
	 */
	inline FMonolithActionResult ResolveDelegateOwnerAndProperty(
		const TSharedPtr<FJsonObject>& Params,
		UClass* SelfContextClass,
		const TCHAR* NodeTypeLabel,
		UClass*& OutOwnerClass,
		FMulticastDelegateProperty*& OutDelegateProp,
		bool& OutbSelfContext)
	{
		FString DelegateNameStr = Params->GetStringField(TEXT("delegate_property_name"));
		if (DelegateNameStr.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%s requires 'delegate_property_name'"), NodeTypeLabel));
		}

		FString TargetClassName = Params->GetStringField(TEXT("target_class"));
		OutbSelfContext = TargetClassName.IsEmpty();

		if (OutbSelfContext)
		{
			if (!SelfContextClass)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("%s requires either target_class or asset_path (for self-context)"),
					NodeTypeLabel));
			}
			OutOwnerClass = SelfContextClass;
		}
		else
		{
			OutOwnerClass = FindFirstObject<UClass>(*TargetClassName, EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass && !TargetClassName.StartsWith(TEXT("A")))
				OutOwnerClass = FindFirstObject<UClass>(*FString::Printf(TEXT("A%s"), *TargetClassName), EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass && !TargetClassName.StartsWith(TEXT("U")))
				OutOwnerClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *TargetClassName), EFindFirstObjectOptions::NativeFirst);
			// Strip a leading A/U prefix and retry bare — handles callers passing the C++ class name
			// (e.g. "AMyActor", "UMyComponent") when UE's object registry uses the bare form.
			if (!OutOwnerClass && TargetClassName.Len() > 1 &&
				(TargetClassName.StartsWith(TEXT("A")) || TargetClassName.StartsWith(TEXT("U"))))
				OutOwnerClass = FindFirstObject<UClass>(*TargetClassName.Mid(1), EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("%s target_class '%s' not found"), NodeTypeLabel, *TargetClassName));
			}
		}

		OutDelegateProp = FindMulticastDelegateProperty(OutOwnerClass, FName(*DelegateNameStr));
		if (!OutDelegateProp)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("BlueprintAssignable multicast delegate '%s' not found on class '%s'"),
				*DelegateNameStr, *OutOwnerClass->GetName()));
		}

		// Success-side payload is unused — callers only check bSuccess and read the out params.
		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	}

	/** Returns true if a UK2Node_CustomEvent with the given name already exists in any graph of the Blueprint */
	bool HasCustomEventNamed(UBlueprint* BP, FName EventName);
}
