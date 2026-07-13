#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintEditorLibrary.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CreateDelegate.h"
#include "EdGraphSchema_K2.h"

namespace
{
	FString EventDispatcherDisplayName(const UEdGraph* Graph)
	{
		if (!Graph)
		{
			return FString();
		}
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		return DisplayName;
	}

	TArray<FString> EventDispatcherNames(const UBlueprint* BP)
	{
		TArray<FString> Names;
		if (!BP)
		{
			return Names;
		}
		for (const UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			const FString DisplayName = EventDispatcherDisplayName(Graph);
			if (!DisplayName.IsEmpty())
			{
				Names.Add(DisplayName);
			}
		}
		Names.Sort();
		return Names;
	}

	TArray<TSharedPtr<FJsonValue>> GraphStringsToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	TArray<FString> RemoveEventDispatcherAcceptedParameters()
	{
		TArray<FString> Values;
		Values.Add(TEXT("dispatcher_name"));
		Values.Add(TEXT("name"));
		Values.Add(TEXT("missing_ok"));
		Values.Add(TEXT("allow_missing"));
		return Values;
	}

	TArray<FString> RenameFunctionAcceptedParameters()
	{
		TArray<FString> Values;
		Values.Add(TEXT("asset_path"));
		Values.Add(TEXT("old_name"));
		Values.Add(TEXT("new_name"));
		return Values;
	}

	TArray<FString> OverrideParentFunctionAcceptedParameters()
	{
		TArray<FString> Values;
		Values.Add(TEXT("asset_path"));
		Values.Add(TEXT("parent_function_name"));
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
			if (!Graph) continue;

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
			Graphs.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
		return Graphs;
	}

	UEdGraph* FindFunctionGraphByName(const UBlueprint* BP, const FString& FunctionName)
	{
		if (!BP)
		{
			return nullptr;
		}
		for (const auto& GraphRef : BP->FunctionGraphs)
		{
			UEdGraph* Graph = GraphRef;
			if (Graph && Graph->GetName() == FunctionName)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	bool FunctionHasReturnValue(const UFunction* Function)
	{
		if (!Function)
		{
			return false;
		}
		for (TFieldIterator<FProperty> PropIt(Function); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
		{
			const FProperty* Prop = *PropIt;
			if (Prop && Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				return true;
			}
		}
		return false;
	}

	FString NonOverridableReason(const UFunction* Function)
	{
		if (!Function)
		{
			return TEXT("function_not_found_on_parent_class");
		}
		if (Function->HasAnyFunctionFlags(FUNC_Final))
		{
			return TEXT("function_is_final");
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			return TEXT("function_is_static");
		}
		if (Function->HasAnyFunctionFlags(FUNC_Private))
		{
			return TEXT("function_is_private");
		}
		if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			return TEXT("function_is_not_blueprint_event");
		}
		return TEXT("engine_override_resolver_rejected_function");
	}

	TArray<TSharedPtr<FJsonValue>> OverrideableParentFunctionJsonValues(UBlueprint* BP, bool& bOutTruncated)
	{
		bOutTruncated = false;
		TArray<TSharedPtr<FJsonValue>> Functions;
		if (!BP || !BP->ParentClass)
		{
			return Functions;
		}

		TSet<FName> SeenNames;
		for (TFieldIterator<UFunction> FuncIt(BP->ParentClass); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (!Function || SeenNames.Contains(Function->GetFName()))
			{
				continue;
			}
			SeenNames.Add(Function->GetFName());

			UFunction* ResolvedFunction = nullptr;
			UClass* OverrideClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
				BP, Function->GetFName(), &ResolvedFunction);
			if (!OverrideClass || !ResolvedFunction)
			{
				continue;
			}

			TSharedPtr<FJsonObject> FunctionObj = MakeShared<FJsonObject>();
			FunctionObj->SetStringField(TEXT("name"), ResolvedFunction->GetName());
			FunctionObj->SetStringField(TEXT("declaring_class"), OverrideClass->GetName());
			FunctionObj->SetBoolField(TEXT("has_return_value"), FunctionHasReturnValue(ResolvedFunction));
			FunctionObj->SetBoolField(TEXT("already_overridden"), FindFunctionGraphByName(BP, ResolvedFunction->GetName()) != nullptr);
			Functions.Add(MakeShared<FJsonValueObject>(FunctionObj));

			if (Functions.Num() >= 50)
			{
				bOutTruncated = true;
				break;
			}
		}
		return Functions;
	}

	TArray<TSharedPtr<FJsonValue>> NonOverridableParentFunctionMatchJsonValues(const UBlueprint* BP, const FString& FunctionName)
	{
		TArray<TSharedPtr<FJsonValue>> Matches;
		if (!BP || !BP->ParentClass || FunctionName.IsEmpty())
		{
			return Matches;
		}

		for (TFieldIterator<UFunction> FuncIt(BP->ParentClass); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (!Function || Function->GetName() != FunctionName)
			{
				continue;
			}

			TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
			MatchObj->SetStringField(TEXT("name"), Function->GetName());
			MatchObj->SetStringField(TEXT("declaring_class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : FString());
			MatchObj->SetStringField(TEXT("reason"), NonOverridableReason(Function));
			MatchObj->SetBoolField(TEXT("is_blueprint_event"), Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
			MatchObj->SetBoolField(TEXT("is_final"), Function->HasAnyFunctionFlags(FUNC_Final));
			MatchObj->SetBoolField(TEXT("is_static"), Function->HasAnyFunctionFlags(FUNC_Static));
			MatchObj->SetBoolField(TEXT("has_return_value"), FunctionHasReturnValue(Function));
			Matches.Add(MakeShared<FJsonValueObject>(MatchObj));
		}
		return Matches;
	}
}

// --- Registration ---

void FMonolithBlueprintGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_function"),
		TEXT("Add a new function graph to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Function name"), {TEXT("function_name")})
			.Optional(TEXT("is_pure"), TEXT("bool"), TEXT("Mark as pure (no exec pins)"), TEXT("false"))
			.Optional(TEXT("is_const"), TEXT("bool"), TEXT("Mark as const"), TEXT("false"))
			.Optional(TEXT("is_static"), TEXT("bool"), TEXT("Mark as static"), TEXT("false"))
			.Optional(TEXT("call_in_editor"), TEXT("bool"), TEXT("Show 'Call In Editor' button"), TEXT("false"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Function category"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Function tooltip/description"))
			.Optional(TEXT("access"), TEXT("string"), TEXT("Access specifier: Public, Protected, or Private"), TEXT("Public"))
			.Optional(TEXT("replication"), TEXT("string"), TEXT("Replication mode: none, multicast, server, client (default: none)"))
			.Optional(TEXT("reliable"), TEXT("bool"), TEXT("Use reliable replication (default: false)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_function_thread_safe"),
		TEXT("Set (or clear) the 'Thread Safe' flag on an existing Blueprint function graph. Sets FKismetUserDeclaredFunctionMetadata::bThreadSafe on the function's entry node and recompiles. Required for functions called from BlueprintThreadSafeUpdateAnimation. Searches function graphs (including AnimBP function graphs)."),
		FMonolithActionHandler::CreateStatic(&HandleSetFunctionThreadSafe),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"), {TEXT("name")})
			.Optional(TEXT("thread_safe"), TEXT("bool"), TEXT("Set the Thread Safe flag (true) or clear it (false)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("override_parent_function"),
		TEXT("Author a Blueprint override of an overridable parent function (BlueprintImplementableEvent / BlueprintNativeEvent), including those that RETURN a value (e.g. UCommonActivatableWidget::BP_GetDesiredFocusTarget -> UWidget*). add_function cannot do this and the event-node form has no ReturnValue pin. Declaring class is resolved generically by name. Returns graph_name, entry_node_id, return_pin_id/name, override_class, has_return_value."),
		FMonolithActionHandler::CreateStatic(&HandleOverrideParentFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("parent_function_name"), TEXT("string"), TEXT("Name of the overridable parent function"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_function"),
		TEXT("Remove a function graph from a Blueprint by name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Function name to remove"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_function"),
		TEXT("Rename an existing function graph in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRenameFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current function name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New function name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_macro"),
		TEXT("Add a new macro graph to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Macro name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_macro"),
		TEXT("Remove a macro graph from a Blueprint by name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("macro_name"), TEXT("string"), TEXT("Macro name to remove"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_macro"),
		TEXT("Rename an existing macro graph in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRenameMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current macro name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New macro name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_event_dispatcher"),
		TEXT("Add a new event dispatcher (multicast delegate) to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddEventDispatcher),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Event dispatcher name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_function_params"),
		TEXT("Add input/output parameters to a Blueprint function"),
		FMonolithActionHandler::CreateStatic(&HandleSetFunctionParams),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type} objects for inputs"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for outputs"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("implement_interface"),
		TEXT("Add an interface to a Blueprint's implemented interface list"),
		FMonolithActionHandler::CreateStatic(&HandleImplementInterface),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"), TEXT("string"), TEXT("Interface class name (e.g. IMyInterface)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_interface"),
		TEXT("Remove an interface from a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveInterface),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"), TEXT("string"), TEXT("Interface class name to remove"))
			.Optional(TEXT("preserve_functions"), TEXT("bool"), TEXT("Keep stub functions after removal"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reparent_blueprint"),
		TEXT("Change the parent class of a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleReparentBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("new_parent_class"), TEXT("string"), TEXT("New parent class name"))
			.Build());

	// ---- Wave 6 ----

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_event_dispatcher"),
		TEXT("Remove an event dispatcher (multicast delegate) from a Blueprint. Warns if any graph nodes still reference it."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEventDispatcher),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),      TEXT("Blueprint asset path"))
			.Optional(TEXT("dispatcher_name"), TEXT("string"), TEXT("Event dispatcher name (without _Signature suffix); alias: name"))
			.Optional(TEXT("name"),            TEXT("string"), TEXT("Alias for dispatcher_name (matches add_event_dispatcher, which uses name)"))
			.Optional(TEXT("missing_ok"),      TEXT("bool"), TEXT("Return a successful no-op when the dispatcher is already absent"), TEXT("false"))
			.Optional(TEXT("allow_missing"),   TEXT("bool"), TEXT("Alias for missing_ok"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_event_dispatcher_params"),
		TEXT("Set (replace) the signature parameters on an event dispatcher. Existing params are cleared and replaced with the new list."),
		FMonolithActionHandler::CreateStatic(&HandleSetEventDispatcherParams),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),      TEXT("Blueprint asset path"))
			.Optional(TEXT("dispatcher_name"), TEXT("string"), TEXT("Event dispatcher name (without _Signature suffix); alias: name"))
			.Optional(TEXT("name"),            TEXT("string"), TEXT("Alias for dispatcher_name (matches add_event_dispatcher, which uses name)"))
			.Required(TEXT("params"),          TEXT("array"),  TEXT("Array of {name, type} objects for the new signature"))
			.Build());

	// ---- Wave 5 ----

	Registry.RegisterAction(TEXT("blueprint"), TEXT("scaffold_interface_implementation"),
		TEXT("Add an interface to a Blueprint AND create all stub function graphs in one call. Returns the interface name and list of created graphs. Much more useful than implement_interface alone — this one actually wires up the stubs."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldInterfaceImplementation),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),       TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"),   TEXT("string"), TEXT("Interface class name (e.g. BPI_Interactable or IBpi_Interactable)"))
			.Build());
}

// --- add_function ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	Params->TryGetStringField(TEXT("name"), FuncName);
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetName() == FuncName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Function already exists: %s"), *FuncName));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create function graph: %s"), *FuncName));
	}

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);

	// Find the entry node to set metadata and flags
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (EntryNode)
	{
		uint32 ExtraFlags = EntryNode->GetFunctionFlags();

		bool bIsPure = false;
		bool bIsConst = false;
		bool bIsStatic = false;
		bool bCallInEditor = false;
		Params->TryGetBoolField(TEXT("is_pure"), bIsPure);
		Params->TryGetBoolField(TEXT("is_const"), bIsConst);
		Params->TryGetBoolField(TEXT("is_static"), bIsStatic);
		Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor);

		if (bIsPure)        ExtraFlags |= FUNC_BlueprintPure;
		if (bIsConst)       ExtraFlags |= FUNC_Const;
		if (bIsStatic)      ExtraFlags |= FUNC_Static;

		// Access specifier
		FString Access;
		Params->TryGetStringField(TEXT("access"), Access);
		ExtraFlags &= ~(FUNC_Protected | FUNC_Private); // clear existing
		if (Access == TEXT("Protected"))      ExtraFlags |= FUNC_Protected;
		else if (Access == TEXT("Private"))   ExtraFlags |= FUNC_Private;

		// RPC / Multicast replication flags (Phase 5A)
		FString Replication;
		if (Params->TryGetStringField(TEXT("replication"), Replication) && !Replication.IsEmpty() && Replication != TEXT("none"))
		{
			const uint32 FlagsToClear = FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient;
			ExtraFlags &= ~FlagsToClear;

			uint32 NetFlag = 0;
			FString Lower = Replication.ToLower();
			if (Lower == TEXT("multicast"))      NetFlag = FUNC_NetMulticast;
			else if (Lower == TEXT("server"))    NetFlag = FUNC_NetServer;
			else if (Lower == TEXT("client"))    NetFlag = FUNC_NetClient;

			if (NetFlag != 0)
				ExtraFlags |= (FUNC_Net | NetFlag);
		}

		bool bReliable = false;
		if (Params->TryGetBoolField(TEXT("reliable"), bReliable) && bReliable)
			ExtraFlags |= FUNC_NetReliable;

		EntryNode->SetExtraFlags(ExtraFlags);
		EntryNode->MetaData.bCallInEditor = bCallInEditor;

		FString Category;
		Params->TryGetStringField(TEXT("category"), Category);
		if (!Category.IsEmpty())
		{
			EntryNode->MetaData.Category = FText::FromString(Category);
		}

		FString Description;
		Params->TryGetStringField(TEXT("description"), Description);
		if (!Description.IsEmpty())
		{
			EntryNode->MetaData.ToolTip = FText::FromString(Description);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetNumberField(TEXT("node_count"), NewGraph->Nodes.Num());
	return FMonolithActionResult::Success(Root);
}

// --- override_parent_function (gap #6) ---
// Authors a Blueprint override of a parent BlueprintImplementableEvent / BlueprintNativeEvent
// that RETURNS a value (e.g. UCommonActivatableWidget::BP_GetDesiredFocusTarget -> UWidget*).
// add_function can't do this (it makes a fresh graph, ignoring the parent signature) and the
// event-node form has no ReturnValue pin by design. Engine-generic: the declaring class is
// resolved by name via GetOverrideFunctionClass — nothing hardcoded.

FMonolithActionResult FMonolithBlueprintGraphActions::HandleOverrideParentFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ParentFuncName;
	if (!Params->TryGetStringField(TEXT("parent_function_name"), ParentFuncName) || ParentFuncName.IsEmpty())
	{
		bool bTruncated = false;
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("missing_parent_function_parameter"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(OverrideParentFunctionAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("available_override_functions"), OverrideableParentFunctionJsonValues(BP, bTruncated));
		ErrorData->SetBoolField(TEXT("available_override_functions_truncated"), bTruncated);
		return FMonolithActionResult::Error(TEXT("Missing required parameter: parent_function_name"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Retry with parent_function_name set to one of error_data.available_override_functions[].name."));
	}

	// Resolve the parent class that DECLARES this overridable function.
	// Mirrors engine BlueprintEditor.cpp:6857-6870 / DataprepEditorUtils.cpp:211.
	UFunction* OverrideFunc = nullptr;
	UClass* const OverrideFuncClass =
		FBlueprintEditorUtils::GetOverrideFunctionClass(BP, FName(*ParentFuncName), &OverrideFunc);

	if (!OverrideFuncClass || !OverrideFunc)
	{
		bool bTruncated = false;
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("parent_function_not_overridable"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("blueprint"), BP->GetName());
		ErrorData->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : FString());
		ErrorData->SetStringField(TEXT("offending_function"), ParentFuncName);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(OverrideParentFunctionAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("available_override_functions"), OverrideableParentFunctionJsonValues(BP, bTruncated));
		ErrorData->SetBoolField(TEXT("available_override_functions_truncated"), bTruncated);
		ErrorData->SetArrayField(TEXT("non_overridable_matches"), NonOverridableParentFunctionMatchJsonValues(BP, ParentFuncName));
		ErrorData->SetStringField(TEXT("recovery"), TEXT("Choose an available_override_functions[].name value, or add BlueprintImplementableEvent/BlueprintNativeEvent exposure to the parent class."));
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not an overridable function on the parent of %s (not found, or not a BlueprintImplementableEvent / BlueprintNativeEvent)."),
			*ParentFuncName, *BP->GetName()))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Use error_data.available_override_functions for valid parent_function_name values; error_data.non_overridable_matches explains exact-name rejects."));
	}

	// Reject if an override graph with this name already exists.
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetName() == ParentFuncName)
		{
			bool bTruncated = false;
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("failure_cause"), TEXT("override_graph_already_exists"));
			ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
			ErrorData->SetStringField(TEXT("offending_function"), ParentFuncName);
			ErrorData->SetStringField(TEXT("existing_graph"), Existing->GetName());
			ErrorData->SetStringField(TEXT("graph_kind"), BlueprintGraphKind(BP, Existing));
			ErrorData->SetArrayField(TEXT("available_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
			ErrorData->SetArrayField(TEXT("available_override_functions"), OverrideableParentFunctionJsonValues(BP, bTruncated));
			ErrorData->SetBoolField(TEXT("available_override_functions_truncated"), bTruncated);
			ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_functions"));
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Override function graph already exists: %s"), *ParentFuncName))
				.WithErrorData(ErrorData)
				.WithHint(TEXT("The override graph is already present. Continue editing that graph or choose another available parent function."))
				.WithRelatedAction(TEXT("blueprint.get_functions"));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*ParentFuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create override graph: %s"), *ParentFuncName));
	}

	// Override authoring uses the <UClass> template + the declaring class with
	// bIsUserCreated=false. (The <UFunction> overload is copy-signature, NOT override —
	// the gaps-doc had this backwards.) The entry/result nodes inherit the parent signature,
	// so a value-returning override gets its ReturnValue pin.
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/false, OverrideFuncClass);

	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (!EntryNode)  EntryNode  = Cast<UK2Node_FunctionEntry>(Node);
		if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
	}

	// Locate the ReturnValue pin on the result node (the whole point of #6 vs the event form).
	FString ReturnPinId;
	FString ReturnPinName;
	if (ResultNode)
	{
		UEdGraphPin* ReturnPin = nullptr;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
			{
				ReturnPin = Pin;
				break;
			}
		}
		// Fall back to the first non-exec input if the return isn't literally named ReturnValue.
		if (!ReturnPin)
		{
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					ReturnPin = Pin;
					break;
				}
			}
		}
		if (ReturnPin)
		{
			ReturnPinId = ReturnPin->PinId.ToString();
			ReturnPinName = ReturnPin->PinName.ToString();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetStringField(TEXT("entry_node_id"), EntryNode ? EntryNode->GetName() : FString());
	Root->SetStringField(TEXT("return_pin_id"), ReturnPinId);
	Root->SetStringField(TEXT("return_pin_name"), ReturnPinName);
	Root->SetStringField(TEXT("override_class"), OverrideFuncClass->GetName());
	Root->SetBoolField(TEXT("has_return_value"), !ReturnPinId.IsEmpty());
	return FMonolithActionResult::Success(Root);
}

// --- remove_function ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	Params->TryGetStringField(TEXT("name"), FuncName);
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Only search function graphs — not the event graph or macros
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_function"), FuncName);
	return FMonolithActionResult::Success(Root);
}

// --- set_function_thread_safe ---
// Sets/clears FKismetUserDeclaredFunctionMetadata::bThreadSafe on a function graph's entry node.
// The BP compiler turns bThreadSafe into the BlueprintThreadSafe function metadata, i.e. it ticks
// the "Thread Safe" checkbox programmatically. Mirrors HandleRemoveFunction's graph search and
// HandleAddFunction's entry-node-find pattern. Function graphs only (AnimBP function graphs also
// live in BP->FunctionGraphs).

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetFunctionThreadSafe(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	if (!Params->TryGetStringField(TEXT("function_name"), FuncName) || FuncName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("name"), FuncName);
	}
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	bool bThreadSafe = true;
	Params->TryGetBoolField(TEXT("thread_safe"), bThreadSafe);

	// Function graphs only (covers AnimBP function graphs, which also live here).
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	// Find the entry node (mirror HandleAddFunction).
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No function entry node found in graph: %s"), *FuncName));
	}

	EntryNode->Modify();
	EntryNode->MetaData.bThreadSafe = bThreadSafe;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetBoolField(TEXT("thread_safe"), EntryNode->MetaData.bThreadSafe);
	return FMonolithActionResult::Success(Root);
}

// --- rename_function ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRenameFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString OldName;
	Params->TryGetStringField(TEXT("old_name"), OldName);
	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);

	if (OldName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("missing_old_name"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(RenameFunctionAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("available_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_functions"));
		return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Call blueprint.get_functions, then retry with old_name set to an existing function graph."));
	}
	if (NewName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("missing_new_name"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("offending_function"), OldName);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(RenameFunctionAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("available_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_functions"));
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Provide new_name with a non-conflicting function graph name."));
	}

	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, OldName);
	if (!Graph)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("function_not_found"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("offending_function"), OldName);
		ErrorData->SetStringField(TEXT("requested_new_name"), NewName);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(RenameFunctionAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), MakeShared<FJsonObject>());
		ErrorData->SetArrayField(TEXT("candidate_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
		ErrorData->SetArrayField(TEXT("available_graphs"), BlueprintGraphCatalogJsonValues(BP));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_functions"));
		ErrorData->SetStringField(TEXT("graph_read_action"), TEXT("blueprint.list_graphs"));
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *OldName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Retry with old_name set to one of error_data.candidate_functions; use error_data.available_graphs to distinguish macros/event graphs from functions."))
			.WithRelatedActions({ TEXT("blueprint.get_functions"), TEXT("blueprint.list_graphs") });
	}

	// Ensure we're only renaming function graphs, not event graphs or macros
	if (!BP->FunctionGraphs.Contains(Graph))
	{
		FString InterfaceName;
		const FString GraphKind = BlueprintGraphKind(BP, Graph, &InterfaceName);
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("graph_kind_not_function"));
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("offending_graph"), OldName);
		ErrorData->SetStringField(TEXT("graph_kind"), GraphKind);
		if (!InterfaceName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("interface"), InterfaceName);
		}
		ErrorData->SetStringField(TEXT("requested_new_name"), NewName);
		ErrorData->SetArrayField(TEXT("candidate_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
		ErrorData->SetArrayField(TEXT("available_graphs"), BlueprintGraphCatalogJsonValues(BP));
		ErrorData->SetStringField(TEXT("recovery"), TEXT("Use rename_function only for function graphs; use rename_macro for macro graphs. Event graphs and delegate signatures are not renamed by this action."));
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph '%s' is a %s graph, not a function graph"), *OldName, *GraphKind))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Use error_data.graph_kind to choose the correct graph action; rename_function only accepts function graphs."))
			.WithRelatedActions({ TEXT("blueprint.get_functions"), TEXT("blueprint.list_graphs"), TEXT("blueprint.rename_macro") });
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing != Graph && Existing->GetName() == NewName)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("failure_cause"), TEXT("name_conflict"));
			ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
			ErrorData->SetStringField(TEXT("offending_function"), OldName);
			ErrorData->SetStringField(TEXT("requested_new_name"), NewName);
			ErrorData->SetStringField(TEXT("conflicting_graph"), Existing->GetName());
			ErrorData->SetStringField(TEXT("conflicting_graph_kind"), BlueprintGraphKind(BP, Existing));
			ErrorData->SetArrayField(TEXT("available_functions"), GraphStringsToJsonValues(FunctionGraphNames(BP)));
			ErrorData->SetArrayField(TEXT("available_graphs"), BlueprintGraphCatalogJsonValues(BP));
			ErrorData->SetStringField(TEXT("recovery"), TEXT("Choose a new_name that does not already name a function graph."));
			return FMonolithActionResult::Error(FString::Printf(TEXT("A function named '%s' already exists"), *NewName))
				.WithErrorData(ErrorData)
				.WithHint(TEXT("Choose a non-conflicting new_name; error_data.available_functions lists current function graph names."))
				.WithRelatedAction(TEXT("blueprint.get_functions"));
		}
	}

	FBlueprintEditorUtils::RenameGraph(Graph, NewName);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), Graph->GetName());
	Root->SetBoolField(TEXT("references_preserved"), true);
	Root->SetStringField(TEXT("reference_preservation"), TEXT("Renamed the existing function graph in place with FBlueprintEditorUtils::RenameGraph; existing graph object references are preserved."));
	return FMonolithActionResult::Success(Root);
}

// --- add_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString MacroName;
	Params->TryGetStringField(TEXT("name"), MacroName);
	if (MacroName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->MacroGraphs)
	{
		if (Existing && Existing->GetName() == MacroName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Macro already exists: %s"), *MacroName));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create macro graph: %s"), *MacroName));
	}

	FBlueprintEditorUtils::AddMacroGraph(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetNumberField(TEXT("node_count"), NewGraph->Nodes.Num());
	return FMonolithActionResult::Success(Root);
}

// --- remove_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString MacroName;
	Params->TryGetStringField(TEXT("macro_name"), MacroName);
	if (MacroName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: macro_name"));
	}

	// Only search macro graphs — not function graphs or event graphs
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->MacroGraphs)
	{
		if (G && G->GetName() == MacroName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
	}

	FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_macro"), MacroName);
	return FMonolithActionResult::Success(Root);
}

// --- rename_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRenameMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString OldName;
	Params->TryGetStringField(TEXT("old_name"), OldName);
	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);

	if (OldName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name"));
	}
	if (NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"));
	}

	// Search macro graphs specifically
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->MacroGraphs)
	{
		if (G && G->GetName() == OldName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Macro not found: %s"), *OldName));
	}

	// Check for name collision within macro graphs
	for (const UEdGraph* Existing : BP->MacroGraphs)
	{
		if (Existing && Existing != Graph && Existing->GetName() == NewName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("A macro named '%s' already exists"), *NewName));
		}
	}

	FBlueprintEditorUtils::RenameGraph(Graph, *NewName);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), Graph->GetName());
	return FMonolithActionResult::Success(Root);
}

// --- add_event_dispatcher ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName;
	Params->TryGetStringField(TEXT("name"), DispatcherName);
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Generate a unique name for the delegate signature graph
	FName UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(BP, DispatcherName);

	// Check for name collision against the display name (without _Signature suffix)
	for (const UEdGraph* Existing : BP->DelegateSignatureGraphs)
	{
		if (!Existing) continue;
		FString ExistingDisplay = Existing->GetName();
		if (ExistingDisplay.EndsWith(TEXT("_Signature")))
		{
			ExistingDisplay.LeftChopInline(10, EAllowShrinking::No);
		}
		if (ExistingDisplay == DispatcherName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Event dispatcher already exists: %s"), *DispatcherName));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, UniqueName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create delegate signature graph: %s"), *DispatcherName));
	}

	BP->DelegateSignatureGraphs.Add(NewGraph);
	NewGraph->bEditable = false;

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*NewGraph);
	K2Schema->CreateFunctionGraphTerminators(*NewGraph, (UClass*)nullptr);
	K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
	K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Compute display name (strip _Signature if UE added it)
	FString DisplayName = NewGraph->GetName();
	if (DisplayName.EndsWith(TEXT("_Signature")))
	{
		DisplayName.LeftChopInline(10, EAllowShrinking::No);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("dispatcher_name"), DisplayName);
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	return FMonolithActionResult::Success(Root);
}

// --- set_function_params ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetFunctionParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	Params->TryGetStringField(TEXT("function_name"), FuncName);
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	// Only function graphs can have params set this way
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	// Find entry and result nodes
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
		if (EntryNode && ResultNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No FunctionEntry node found in: %s"), *FuncName));
	}

	int32 InputsAdded = 0;
	int32 OutputsAdded = 0;

	// Process inputs — add as user-defined pins on the entry node
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			if (!InputVal->TryGetObject(InputObj) || !InputObj) continue;

			FString PinName, TypeStr;
			(*InputObj)->TryGetStringField(TEXT("name"), PinName);
			(*InputObj)->TryGetStringField(TEXT("type"), TypeStr);

			if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;

			FEdGraphPinType PinType = MonolithBlueprintInternal::ParsePinTypeFromString(TypeStr);
			EntryNode->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output);
			++InputsAdded;
		}
	}

	// Process outputs — add as user-defined pins on the result node
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
	{
		if (!ResultNode)
		{
			// Create a result node if one doesn't exist
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
			ResultNode = Creator.CreateNode();
			ResultNode->NodePosX = EntryNode ? EntryNode->NodePosX + 400 : 0;
			ResultNode->NodePosY = EntryNode ? EntryNode->NodePosY : 0;
			Creator.Finalize();
		}

		for (const TSharedPtr<FJsonValue>& OutputVal : *OutputsArray)
		{
			const TSharedPtr<FJsonObject>* OutputObj = nullptr;
			if (!OutputVal->TryGetObject(OutputObj) || !OutputObj) continue;

			FString PinName, TypeStr;
			(*OutputObj)->TryGetStringField(TEXT("name"), PinName);
			(*OutputObj)->TryGetStringField(TEXT("type"), TypeStr);

			if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;

			FEdGraphPinType PinType = MonolithBlueprintInternal::ParsePinTypeFromString(TypeStr);
			ResultNode->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Input);
			++OutputsAdded;
		}
	}

	if (InputsAdded == 0 && OutputsAdded == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid inputs or outputs provided"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetNumberField(TEXT("inputs_added"), InputsAdded);
	Root->SetNumberField(TEXT("outputs_added"), OutputsAdded);
	return FMonolithActionResult::Success(Root);
}

// --- implement_interface ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleImplementInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName;
	Params->TryGetStringField(TEXT("interface_class"), InterfaceClassName);
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Verify the class exists before attempting to add it. Resolves C++ interfaces by class name
	// AND Blueprint Interface assets by asset path or short name (see ResolveInterfaceClass).
	UClass* InterfaceClass = MonolithBlueprintInternal::ResolveInterfaceClass(InterfaceClassName);
	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Interface class not found: %s. For C++ interfaces use the I-prefixed name; for "
			     "Blueprint interfaces use the asset name or /Game path."), *InterfaceClassName));
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not an interface"), *InterfaceClassName));
	}

	// Check if already implemented
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface == InterfaceClass)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Interface already implemented: %s"), *InterfaceClassName));
		}
	}

	const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, InterfaceClass->GetClassPathName());
	if (!bAdded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to implement interface: %s"), *InterfaceClassName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_class"), InterfaceClassName);
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	return FMonolithActionResult::Success(Root);
}

// --- remove_interface ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName;
	Params->TryGetStringField(TEXT("interface_class"), InterfaceClassName);
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Verify the interface is actually implemented
	UClass* InterfaceClass = nullptr;
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface && Existing.Interface->GetName() == InterfaceClassName)
		{
			InterfaceClass = Existing.Interface;
			break;
		}
	}

	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Interface not implemented by this Blueprint: %s"), *InterfaceClassName));
	}

	bool bPreserveFunctions = false;
	Params->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);

	FBlueprintEditorUtils::RemoveInterface(BP, InterfaceClass->GetClassPathName(), bPreserveFunctions);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_class"), InterfaceClassName);
	Root->SetBoolField(TEXT("functions_preserved"), bPreserveFunctions);
	return FMonolithActionResult::Success(Root);
}

// --- scaffold_interface_implementation ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleScaffoldInterfaceImplementation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName;
	Params->TryGetStringField(TEXT("interface_class"), InterfaceClassName);
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Resolve C++ interfaces by class name AND Blueprint Interface assets by asset path / short
	// name (the generated "<Name>_C" class or an AssetRegistry load — see ResolveInterfaceClass).
	UClass* InterfaceClass = MonolithBlueprintInternal::ResolveInterfaceClass(InterfaceClassName);
	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Interface class not found: %s"), *InterfaceClassName));
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not an interface"), *InterfaceClassName));
	}

	// Check if already implemented
	bool bAlreadyImplemented = false;
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface == InterfaceClass)
		{
			bAlreadyImplemented = true;
			break;
		}
	}

	// Snapshot function graph names before implementing so we can detect which were newly created
	TSet<FName> GraphsBefore;
	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (G) GraphsBefore.Add(G->GetFName());
	}
	TSet<FName> UbergraphsBefore;
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (G) UbergraphsBefore.Add(G->GetFName());
	}

	if (!bAlreadyImplemented)
	{
		// ImplementNewInterface requires FTopLevelAssetPath (not the deprecated FName overload)
		const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, InterfaceClass->GetClassPathName());
		if (!bAdded)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("ImplementNewInterface failed for: %s"), *InterfaceClassName));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	// Collect newly created graphs — compare against pre-implementation snapshots
	// Functions with return values get function graphs; void functions become event nodes in the ubergraph
	TArray<TSharedPtr<FJsonValue>> FunctionsCreated;

	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (!G) continue;
		if (GraphsBefore.Contains(G->GetFName())) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), G->GetName());
		Entry->SetStringField(TEXT("graph_name"), G->GetName());
		Entry->SetBoolField(TEXT("is_event"), false);
		FunctionsCreated.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Also detect new event nodes added to ubergraph pages (void interface functions)
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (!G) continue;
		for (const UEdGraphNode* Node : G->Nodes)
		{
			if (const UK2Node_Event* EvNode = Cast<UK2Node_Event>(Node))
			{
				// Interface events added by ImplementNewInterface will be overrides
				if (EvNode->bOverrideFunction)
				{
					// Check that this event's function is from the interface we just added
					if (EvNode->EventReference.GetMemberParentClass() == InterfaceClass ||
						InterfaceClass->FindFunctionByName(EvNode->EventReference.GetMemberName()))
					{
						// Was this node in the ubergraph before? We don't have per-node snapshot,
						// so report all override events belonging to this interface.
						// When already_implemented=true, we still list them so the caller knows what's there.
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("name"), EvNode->EventReference.GetMemberName().ToString());
						Entry->SetStringField(TEXT("graph_name"), G->GetName());
						Entry->SetBoolField(TEXT("is_event"), true);
						FunctionsCreated.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Root->SetArrayField(TEXT("functions_created"), FunctionsCreated);
	Root->SetBoolField(TEXT("already_implemented"), bAlreadyImplemented);
	if (FunctionsCreated.Num() == 0 && !bAlreadyImplemented)
	{
		Root->SetStringField(TEXT("note"),
			TEXT("No Blueprint-overridable functions found on this interface. "
			     "C++ interfaces with only native functions cannot generate stubs — override them in C++ instead."));
	}
	return FMonolithActionResult::Success(Root);
}

// --- reparent_blueprint ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ClassName;
	Params->TryGetStringField(TEXT("new_parent_class"), ClassName);
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_parent_class"));
	}

	UClass* NewParent = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NewParent)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class not found: %s"), *ClassName));
	}

	if (NewParent->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot reparent to an interface class: %s"), *ClassName));
	}

	FString OldParent = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

	UBlueprintEditorLibrary::ReparentBlueprint(BP, NewParent);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("old_parent_class"), OldParent);
	Root->SetStringField(TEXT("new_parent_class"), NewParent->GetName());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_event_dispatcher  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName;
	Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName);
	if (DispatcherName.IsEmpty())
	{
		// Accept `name` as an alias so the param matches add_event_dispatcher (which takes `name`).
		Params->TryGetStringField(TEXT("name"), DispatcherName);
	}
	bool bMissingOk = false;
	Params->TryGetBoolField(TEXT("missing_ok"), bMissingOk);
	if (!bMissingOk)
	{
		Params->TryGetBoolField(TEXT("allow_missing"), bMissingOk);
	}
	if (DispatcherName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("missing_dispatcher_parameter"));
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(RemoveEventDispatcherAcceptedParameters()));
		ErrorData->SetArrayField(TEXT("available_dispatchers"), GraphStringsToJsonValues(EventDispatcherNames(BP)));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_event_dispatchers"));
		return FMonolithActionResult::Error(TEXT("Missing required parameter: dispatcher_name (or name)"), -32602)
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Call blueprint.get_event_dispatchers to list valid dispatcher names, or pass dispatcher_name/name."))
			.WithRelatedActions({ TEXT("blueprint.get_event_dispatchers"), TEXT("blueprint.get_event_dispatcher_details") });
	}

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		const FString DisplayName = EventDispatcherDisplayName(Graph);
		if (DisplayName == DispatcherName)
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		const TArray<FString> AvailableDispatchers = EventDispatcherNames(BP);
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), TEXT("dispatcher_not_found"));
		ErrorData->SetStringField(TEXT("dispatcher_name"), DispatcherName);
		ErrorData->SetArrayField(TEXT("accepted_parameters"), GraphStringsToJsonValues(RemoveEventDispatcherAcceptedParameters()));
		ErrorData->SetArrayField(TEXT("available_dispatchers"), GraphStringsToJsonValues(AvailableDispatchers));
		ErrorData->SetBoolField(TEXT("missing_ok_allowed"), true);
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_event_dispatchers"));

		if (bMissingOk)
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("asset_path"), AssetPath);
			Root->SetStringField(TEXT("requested_dispatcher"), DispatcherName);
			Root->SetBoolField(TEXT("removed"), false);
			Root->SetBoolField(TEXT("no_op"), true);
			Root->SetArrayField(TEXT("available_dispatchers"), GraphStringsToJsonValues(AvailableDispatchers));
			Root->SetStringField(TEXT("read_action"), TEXT("blueprint.get_event_dispatchers"));
			return FMonolithActionResult::Success(Root);
		}

		return FMonolithActionResult::Error(FString::Printf(
				TEXT("Event dispatcher not found: %s"), *DispatcherName))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Call blueprint.get_event_dispatchers to choose an existing dispatcher, or pass missing_ok=true for cleanup/idempotent delete-first flows."))
			.WithRelatedActions({ TEXT("blueprint.get_event_dispatchers"), TEXT("blueprint.get_event_dispatcher_details") });
	}

	// Warn if any CreateDelegate nodes still reference this dispatcher
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	TArray<FString> Warnings;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (UK2Node_CreateDelegate* CreateDel = Cast<UK2Node_CreateDelegate>(Node))
			{
				if (CreateDel->GetDelegateSignature() &&
					CreateDel->GetDelegateSignature()->GetOuter() == SigGraph)
				{
					Warnings.Add(FString::Printf(TEXT("CreateDelegate node '%s' in graph '%s' still references this dispatcher"),
						*Node->GetName(), *Graph->GetName()));
				}
			}
			else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->FunctionReference.GetMemberName() == FName(*DispatcherName))
				{
					Warnings.Add(FString::Printf(TEXT("CallFunction node '%s' in graph '%s' may reference this dispatcher"),
						*Node->GetName(), *Graph->GetName()));
				}
			}
		}
	}

	FBlueprintEditorUtils::RemoveGraph(BP, SigGraph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("removed_dispatcher"), DispatcherName);
	Root->SetBoolField(TEXT("removed"), true);
	Root->SetBoolField(TEXT("no_op"), false);
	Root->SetArrayField(TEXT("available_dispatchers"), GraphStringsToJsonValues(EventDispatcherNames(BP)));

	TArray<TSharedPtr<FJsonValue>> WarnArr;
	WarnArr.Reserve(Warnings.Num());
	for (const FString& W : Warnings)
	{
		WarnArr.Add(MakeShared<FJsonValueString>(W));
	}
	Root->SetArrayField(TEXT("warnings"), WarnArr);
	Root->SetNumberField(TEXT("warning_count"), WarnArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_event_dispatcher_params  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetEventDispatcherParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName;
	Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName);
	if (DispatcherName.IsEmpty())
	{
		// Accept `name` as an alias so the param matches add_event_dispatcher (which takes `name`).
		Params->TryGetStringField(TEXT("name"), DispatcherName);
	}
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: dispatcher_name (or name)"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("params"), ParamsArray) || !ParamsArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: params (array of {name, type})"));
	}

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		if (DisplayName == DispatcherName)
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Event dispatcher not found: %s"), *DispatcherName));
	}

	// Find the FunctionEntry node in the signature graph
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : SigGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No FunctionEntry node found in dispatcher signature graph: %s"), *DispatcherName));
	}

	// Clear existing user-defined pins safely — iterate a copy since removal mutates the array
	TArray<TSharedPtr<FUserPinInfo>> PinsToRemove = EntryNode->UserDefinedPins;
	for (const TSharedPtr<FUserPinInfo>& PinInfo : PinsToRemove)
	{
		if (PinInfo.IsValid())
		{
			EntryNode->RemoveUserDefinedPin(PinInfo);
		}
	}

	// Add new params
	int32 ParamsAdded = 0;
	for (const TSharedPtr<FJsonValue>& ParamVal : *ParamsArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!ParamVal->TryGetObject(ParamObj) || !ParamObj) continue;

		FString PinName, TypeStr;
		(*ParamObj)->TryGetStringField(TEXT("name"), PinName);
		(*ParamObj)->TryGetStringField(TEXT("type"), TypeStr);
		if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;

		FEdGraphPinType PinType = MonolithBlueprintInternal::ParsePinTypeFromString(TypeStr);
		EntryNode->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output);
		++ParamsAdded;
	}

	// Reconstruct the node to apply pin changes
	EntryNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("dispatcher_name"), DispatcherName);
	Root->SetNumberField(TEXT("params_set"), ParamsAdded);
	return FMonolithActionResult::Success(Root);
}
