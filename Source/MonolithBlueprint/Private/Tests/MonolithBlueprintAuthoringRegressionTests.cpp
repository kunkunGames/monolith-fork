#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "MonolithBlueprintActions.h"
#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintNodeActions.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintVariableActions.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithBlueprintAuthoringRegressionTests
{
	struct FScopedBlueprintAsset
	{
		explicit FScopedBlueprintAsset(const TCHAR* InAssetName)
			: AssetName(FString::Printf(
				TEXT("%s_%s"),
				InAssetName,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits)))
			, PackageName(FString::Printf(
				TEXT("/Game/Tests/Monolith/Automation/Blueprint/%s"),
				*AssetName))
		{
			// Never reuse or delete a developer-owned package. The GUID makes a
			// collision vanishingly unlikely; both memory and disk checks make the
			// ownership boundary explicit if a stale prior run somehow remains.
			if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
			{
				return;
			}

			Package = CreatePackage(*PackageName);
			if (Package)
			{
				bOwnsPackage = true;
				Blueprint = FKismetEditorUtilities::CreateBlueprint(
					AActor::StaticClass(),
					Package,
					FName(*AssetName),
					BPTYPE_Normal,
					UBlueprint::StaticClass(),
					UBlueprintGeneratedClass::StaticClass(),
					NAME_None);
			}
		}

		~FScopedBlueprintAsset()
		{
			if (!bOwnsPackage)
			{
				return;
			}

			if (Blueprint)
			{
				Blueprint->ClearFlags(RF_Standalone | RF_Public);
				Blueprint->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->ClearFlags(RF_Standalone);
				Package->MarkAsGarbage();
			}

			CollectGarbage(RF_NoFlags);
		}

		FString GetAssetPath() const
		{
			return Blueprint ? Blueprint->GetPathName() : FString();
		}

		FString AssetName;
		FString PackageName;
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		bool bOwnsPackage = false;
	};

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint) return nullptr;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	template <typename TNode>
	bool GraphContainsNode(const UEdGraph* Graph)
	{
		if (!Graph) return false;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<TNode>())
			{
				return true;
			}
		}
		return false;
	}

	bool JsonPinsContain(const TSharedPtr<FJsonObject>& Result, const FString& PinName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject> Pin = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
			FString Name;
			if (Pin.IsValid() && Pin->TryGetStringField(TEXT("name"), Name) && Name == PinName)
			{
				return true;
			}
		}
		return false;
	}

	UEdGraphNode* FindNodeById(const UEdGraph* Graph, const FString& NodeId)
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FName PinName)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == PinName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FString ResultString(const FMonolithActionResult& Result, const TCHAR* Field)
	{
		FString Value;
		if (Result.Result.IsValid())
		{
			Result.Result->TryGetStringField(Field, Value);
		}
		return Value;
	}

	TSharedPtr<FJsonObject> FindArrayObjectByStringField(
		const FMonolithActionResult& Result,
		const TCHAR* ArrayField,
		const TCHAR* ObjectField,
		const FString& Expected,
		int32& OutMatchCount)
	{
		OutMatchCount = 0;
		TSharedPtr<FJsonObject> FirstMatch;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Result.Result.IsValid()
			|| !Result.Result->TryGetArrayField(ArrayField, Values)
			|| !Values)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Actual;
			if (Object.IsValid()
				&& Object->TryGetStringField(ObjectField, Actual)
				&& Actual == Expected)
			{
				++OutMatchCount;
				if (!FirstMatch.IsValid())
				{
					FirstMatch = Object;
				}
			}
		}
		return FirstMatch;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintSpawnActorAliasRegressionTest,
	"Monolith.Blueprint.Authoring.SpawnActorUsesSafeModernNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintSpawnActorAliasRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));

	const FMonolithActionResult Result = FMonolithBlueprintNodeActions::HandleResolveNode(Params);
	TestTrue(TEXT("SpawnActor dry-run resolves without entering the legacy crash-prone node"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		FString ResolvedType;
		FString ResolvedClass;
		Result.Result->TryGetStringField(TEXT("resolved_type"), ResolvedType);
		Result.Result->TryGetStringField(TEXT("resolved_class"), ResolvedClass);
		TestEqual(TEXT("SpawnActor normalizes to SpawnActorFromClass"), ResolvedType, FString(TEXT("SpawnActorFromClass")));
		TestEqual(TEXT("SpawnActor resolves to the modern K2 node"), ResolvedClass, FString(TEXT("K2Node_SpawnActorFromClass")));
	}

	FScopedBlueprintAsset Fixture(TEXT("BP_SpawnActorAliasRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	AddParams->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));
	AddParams->SetStringField(TEXT("actor_class"), TEXT("Actor"));
	const FMonolithActionResult AddResult = FMonolithBlueprintNodeActions::HandleAddNode(AddParams);
	TestTrue(TEXT("SpawnActor add_node uses the configured modern builder"), AddResult.bSuccess);
	if (AddResult.Result.IsValid())
	{
		FString AddedClass;
		AddResult.Result->TryGetStringField(TEXT("class"), AddedClass);
		TestEqual(TEXT("SpawnActor add_node creates the modern K2 node"), AddedClass, FString(TEXT("K2Node_SpawnActorFromClass")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintDispatcherLifecycleRegressionTest,
	"Monolith.Blueprint.Authoring.DispatcherLifecycleAndResolveSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintDispatcherLifecycleRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_DispatcherLifecycleRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	const FName DispatcherName(TEXT("OnPayload"));
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	AddParams->SetStringField(TEXT("name"), DispatcherName.ToString());
	const FMonolithActionResult AddResult = FMonolithBlueprintGraphActions::HandleAddEventDispatcher(AddParams);
	TestTrue(TEXT("add_event_dispatcher succeeds"), AddResult.bSuccess);

	const FBPVariableDescription* DelegateVariable = Fixture.Blueprint->NewVariables.FindByPredicate(
		[DispatcherName](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == DispatcherName;
		});
	TestNotNull(TEXT("Dispatcher creates a Blueprint member variable"), DelegateVariable);
	if (DelegateVariable)
	{
		TestEqual(
			TEXT("Dispatcher member variable has multicast-delegate type"),
			DelegateVariable->VarType.PinCategory,
			UEdGraphSchema_K2::PC_MCDelegate);
	}

	TSharedPtr<FJsonObject> SignatureParams = MakeShared<FJsonObject>();
	SignatureParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	SignatureParams->SetStringField(TEXT("dispatcher_name"), DispatcherName.ToString());
	TSharedPtr<FJsonObject> PayloadParam = MakeShared<FJsonObject>();
	PayloadParam->SetStringField(TEXT("name"), TEXT("Payload"));
	PayloadParam->SetStringField(TEXT("type"), TEXT("int"));
	TArray<TSharedPtr<FJsonValue>> ParamsArray;
	ParamsArray.Add(MakeShared<FJsonValueObject>(PayloadParam));
	SignatureParams->SetArrayField(TEXT("params"), ParamsArray);
	const FMonolithActionResult SignatureResult =
		FMonolithBlueprintGraphActions::HandleSetEventDispatcherParams(SignatureParams);
	TestTrue(TEXT("Dispatcher signature parameter is authored"), SignatureResult.bSuccess);

	UEdGraph* SignatureGraph = Fixture.Blueprint->DelegateSignatureGraphs.Num() > 0
		? Fixture.Blueprint->DelegateSignatureGraphs[0]
		: nullptr;
	UK2Node_FunctionEntry* SignatureEntry = nullptr;
	if (SignatureGraph)
	{
		for (UEdGraphNode* Node : SignatureGraph->Nodes)
		{
			if ((SignatureEntry = Cast<UK2Node_FunctionEntry>(Node)) != nullptr)
			{
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("Dispatcher signature entry exists"), SignatureEntry))
	{
		return false;
	}
	TestEqual(TEXT("Dispatcher begins with one authored parameter"), SignatureEntry->UserDefinedPins.Num(), 1);

	TSharedPtr<FJsonObject> MalformedSignatureParams = MakeShared<FJsonObject>();
	MalformedSignatureParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	MalformedSignatureParams->SetStringField(TEXT("dispatcher_name"), DispatcherName.ToString());
	MalformedSignatureParams->SetArrayField(
		TEXT("params"), { MakeShared<FJsonValueString>(TEXT("not-an-object")) });
	TestFalse(
		TEXT("Malformed dispatcher replacement is rejected"),
		FMonolithBlueprintGraphActions::HandleSetEventDispatcherParams(MalformedSignatureParams).bSuccess);
	TestEqual(
		TEXT("Malformed replacement preserves the existing dispatcher signature"),
		SignatureEntry->UserDefinedPins.Num(),
		1);
	if (SignatureEntry->UserDefinedPins.Num() == 1 && SignatureEntry->UserDefinedPins[0].IsValid())
	{
		TestEqual(
			TEXT("Preserved dispatcher parameter keeps its name"),
			SignatureEntry->UserDefinedPins[0]->PinName,
			FName(TEXT("Payload")));
	}

	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestNotNull(
		TEXT("Compiled Blueprint exposes the dispatcher property"),
		FindFProperty<FMulticastDelegateProperty>(Fixture.Blueprint->GeneratedClass, DispatcherName));

	TSharedPtr<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	ResolveParams->SetStringField(TEXT("node_type"), TEXT("CallDelegate"));
	ResolveParams->SetStringField(TEXT("delegate_property_name"), DispatcherName.ToString());
	const FMonolithActionResult ResolveResult = FMonolithBlueprintNodeActions::HandleResolveNode(ResolveParams);
	TestTrue(TEXT("CallDelegate dry-run resolves the self-context dispatcher"), ResolveResult.bSuccess);
	TestTrue(
		TEXT("CallDelegate dry-run preserves the dispatcher signature pin"),
		JsonPinsContain(ResolveResult.Result, TEXT("Payload")));

	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	RemoveParams->SetStringField(TEXT("dispatcher_name"), DispatcherName.ToString());
	const FMonolithActionResult RemoveResult =
		FMonolithBlueprintGraphActions::HandleRemoveEventDispatcher(RemoveParams);
	TestTrue(TEXT("remove_event_dispatcher succeeds"), RemoveResult.bSuccess);
	TestFalse(
		TEXT("Removing a dispatcher also removes its member variable"),
		Fixture.Blueprint->NewVariables.ContainsByPredicate(
			[DispatcherName](const FBPVariableDescription& Variable)
			{
				return Variable.VarName == DispatcherName;
			}));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintBatchGraphPropagationRegressionTest,
	"Monolith.Blueprint.Authoring.BatchGraphNamePropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintBatchGraphPropagationRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_BatchGraphPropagationRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddFunctionParams = MakeShared<FJsonObject>();
	AddFunctionParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	AddFunctionParams->SetStringField(TEXT("name"), TEXT("BatchTarget"));
	const FMonolithActionResult FunctionResult =
		FMonolithBlueprintGraphActions::HandleAddFunction(AddFunctionParams);
	if (!TestTrue(TEXT("Target function graph created"), FunctionResult.bSuccess))
	{
		return false;
	}

	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages.Num() > 0
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	UEdGraph* FunctionGraph = FindGraph(Fixture.Blueprint, TEXT("BatchTarget"));
	if (!TestNotNull(TEXT("Event graph exists"), EventGraph) ||
		!TestNotNull(TEXT("Function graph exists"), FunctionGraph))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InheritedGraphOp = MakeShared<FJsonObject>();
	InheritedGraphOp->SetStringField(TEXT("op"), TEXT("add_node"));
	InheritedGraphOp->SetStringField(TEXT("node_type"), TEXT("Branch"));

	TSharedPtr<FJsonObject> OverrideGraphOp = MakeShared<FJsonObject>();
	OverrideGraphOp->SetStringField(TEXT("op"), TEXT("add_node"));
	OverrideGraphOp->SetStringField(TEXT("node_type"), TEXT("Sequence"));
	OverrideGraphOp->SetStringField(TEXT("graph_name"), EventGraph->GetName());

	TArray<TSharedPtr<FJsonValue>> Operations;
	Operations.Add(MakeShared<FJsonValueObject>(InheritedGraphOp));
	Operations.Add(MakeShared<FJsonValueObject>(OverrideGraphOp));

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	BatchParams->SetStringField(TEXT("graph_name"), FunctionGraph->GetName());
	BatchParams->SetArrayField(TEXT("operations"), Operations);
	const FMonolithActionResult BatchResult = FMonolithBlueprintNodeActions::HandleBatchExecute(BatchParams);
	TestTrue(TEXT("batch_execute returns successfully"), BatchResult.bSuccess);
	bool bBatchSucceeded = false;
	TestTrue(
		TEXT("Every batch operation succeeds"),
		BatchResult.Result.IsValid() &&
		BatchResult.Result->TryGetBoolField(TEXT("success"), bBatchSucceeded) &&
		bBatchSucceeded);

	TestTrue(TEXT("Top-level graph_name reaches an operation without an override"),
		GraphContainsNode<UK2Node_IfThenElse>(FunctionGraph));
	TestFalse(TEXT("Inherited operation does not fall back to EventGraph"),
		GraphContainsNode<UK2Node_IfThenElse>(EventGraph));
	TestTrue(TEXT("Per-operation graph_name overrides the batch default"),
		GraphContainsNode<UK2Node_ExecutionSequence>(EventGraph));
	TestFalse(TEXT("Override operation does not leak into the batch-default graph"),
		GraphContainsNode<UK2Node_ExecutionSequence>(FunctionGraph));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintArrayFunctionWildcardRegressionTest,
	"Monolith.Blueprint.Authoring.ArrayFunctionWildcardLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintArrayFunctionWildcardRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_ArrayFunctionWildcardRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	ResolveParams->SetStringField(TEXT("function_name"), TEXT("Array_Length"));
	ResolveParams->SetStringField(TEXT("target_class"), TEXT("KismetArrayLibrary"));
	const FMonolithActionResult ResolveResult =
		FMonolithBlueprintNodeActions::HandleResolveNode(ResolveParams);
	TestTrue(TEXT("Array_Length dry-run resolves"), ResolveResult.bSuccess);
	TestEqual(
		TEXT("Dry-run uses the palette's specialized array node"),
		ResultString(ResolveResult, TEXT("resolved_class")),
		FString(TEXT("K2Node_CallArrayFunction")));
	TestEqual(
		TEXT("No-context dry-run keeps the native function-library owner"),
		ResultString(ResolveResult, TEXT("resolved_function_class")),
		FString(TEXT("KismetArrayLibrary")));
	TestTrue(
		TEXT("No-context dry-run materializes the wildcard array pin"),
		ResolveResult.Result.IsValid() &&
		JsonPinsContain(ResolveResult.Result, TEXT("TargetArray")));

	TSharedPtr<FJsonObject> VariableParams = MakeShared<FJsonObject>();
	VariableParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	VariableParams->SetStringField(TEXT("name"), TEXT("Numbers"));
	VariableParams->SetStringField(TEXT("type"), TEXT("array:int"));
	TestTrue(
		TEXT("Typed array variable is authored"),
		FMonolithBlueprintVariableActions::HandleAddVariable(VariableParams).bSuccess);

	TSharedPtr<FJsonObject> GetterParams = MakeShared<FJsonObject>();
	GetterParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	GetterParams->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
	GetterParams->SetStringField(TEXT("variable_name"), TEXT("Numbers"));
	const FMonolithActionResult GetterResult =
		FMonolithBlueprintNodeActions::HandleAddNode(GetterParams);
	TestTrue(TEXT("Typed array getter is authored"), GetterResult.bSuccess);

	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	CallParams->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	CallParams->SetStringField(TEXT("function_name"), TEXT("Array_Length"));
	CallParams->SetStringField(TEXT("target_class"), TEXT("KismetArrayLibrary"));
	const FMonolithActionResult CallResult =
		FMonolithBlueprintNodeActions::HandleAddNode(CallParams);
	TestTrue(TEXT("Array_Length call is authored"), CallResult.bSuccess);
	TestEqual(
		TEXT("Authored call uses the specialized array node"),
		ResultString(CallResult, TEXT("class")),
		FString(TEXT("K2Node_CallArrayFunction")));

	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages.Num() > 0
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	UK2Node_CallArrayFunction* ArrayCall = Cast<UK2Node_CallArrayFunction>(
		FindNodeById(EventGraph, ResultString(CallResult, TEXT("id"))));
	if (!TestNotNull(TEXT("Specialized array call exists in the graph"), ArrayCall))
	{
		return false;
	}
	UEdGraphPin* TargetArrayPin = FindPinByName(ArrayCall, TEXT("TargetArray"));
	if (!TestNotNull(TEXT("Array_Length exposes TargetArray"), TargetArrayPin))
	{
		return false;
	}
	TestEqual(
		TEXT("Unconnected array parameter begins wildcard"),
		TargetArrayPin->PinType.PinCategory,
		UEdGraphSchema_K2::PC_Wildcard);

	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	ConnectParams->SetStringField(TEXT("source_node"), ResultString(GetterResult, TEXT("id")));
	ConnectParams->SetStringField(TEXT("source_pin"), TEXT("Numbers"));
	ConnectParams->SetStringField(TEXT("target_node"), ResultString(CallResult, TEXT("id")));
	ConnectParams->SetStringField(TEXT("target_pin"), TEXT("TargetArray"));
	TestTrue(
		TEXT("Typed array connects through the schema"),
		FMonolithBlueprintNodeActions::HandleConnectPins(ConnectParams).bSuccess);
	TestEqual(
		TEXT("Connection resolves wildcard element type"),
		TargetArrayPin->PinType.PinCategory,
		UEdGraphSchema_K2::PC_Int);

	TSharedPtr<FJsonObject> DisconnectParams = MakeShared<FJsonObject>();
	DisconnectParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	DisconnectParams->SetStringField(TEXT("node_id"), ResultString(CallResult, TEXT("id")));
	DisconnectParams->SetStringField(TEXT("pin_name"), TEXT("TargetArray"));
	TestTrue(
		TEXT("Array pin disconnects through the graph schema"),
		FMonolithBlueprintNodeActions::HandleDisconnectPins(DisconnectParams).bSuccess);
	TestEqual(
		TEXT("Disconnect restores wildcard type"),
		TargetArrayPin->PinType.PinCategory,
		UEdGraphSchema_K2::PC_Wildcard);

	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("Blueprint compiles after wildcard connect/disconnect"), Fixture.Blueprint->Status != BS_Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintLocalVariableBindingRegressionTest,
	"Monolith.Blueprint.Authoring.LocalVariableNodesMaterializePins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintLocalVariableBindingRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_LocalVariableBindingRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	TSharedPtr<FJsonObject> FunctionParams = MakeShared<FJsonObject>();
	FunctionParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	FunctionParams->SetStringField(TEXT("name"), TEXT("ScopedFunction"));
	TestTrue(
		TEXT("Function graph is authored"),
		FMonolithBlueprintGraphActions::HandleAddFunction(FunctionParams).bSuccess);

	TSharedPtr<FJsonObject> LocalParams = MakeShared<FJsonObject>();
	LocalParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	LocalParams->SetStringField(TEXT("function_name"), TEXT("ScopedFunction"));
	LocalParams->SetStringField(TEXT("name"), TEXT("LocalCount"));
	LocalParams->SetStringField(TEXT("type"), TEXT("int"));
	TestTrue(
		TEXT("Function local is authored"),
		FMonolithBlueprintVariableActions::HandleAddLocalVariable(LocalParams).bSuccess);

	auto AddLocalAccess = [&Fixture](const FString& NodeType)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
		Params->SetStringField(TEXT("graph_name"), TEXT("ScopedFunction"));
		Params->SetStringField(TEXT("node_type"), NodeType);
		Params->SetStringField(TEXT("variable_name"), TEXT("LocalCount"));
		return FMonolithBlueprintNodeActions::HandleAddNode(Params);
	};

	const FMonolithActionResult GetterResult = AddLocalAccess(TEXT("VariableGet"));
	const FMonolithActionResult SetterResult = AddLocalAccess(TEXT("VariableSet"));
	TestTrue(TEXT("Local getter is authored"), GetterResult.bSuccess);
	TestTrue(TEXT("Local setter is authored"), SetterResult.bSuccess);

	UEdGraph* FunctionGraph = FindGraph(Fixture.Blueprint, TEXT("ScopedFunction"));
	UEdGraphPin* GetterPin = FindPinByName(
		FindNodeById(FunctionGraph, ResultString(GetterResult, TEXT("id"))),
		TEXT("LocalCount"));
	UEdGraphPin* SetterPin = FindPinByName(
		FindNodeById(FunctionGraph, ResultString(SetterResult, TEXT("id"))),
		TEXT("LocalCount"));
	if (!TestNotNull(TEXT("Local getter materializes its data pin"), GetterPin) ||
		!TestNotNull(TEXT("Local setter materializes its data pin"), SetterPin))
	{
		return false;
	}
	TestEqual(TEXT("Local getter pin is typed"), GetterPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Int);
	TestEqual(TEXT("Local setter pin is typed"), SetterPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Int);

	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("Blueprint with local Get/Set nodes compiles"), Fixture.Blueprint->Status != BS_Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintEnumVariableTypeRegressionTest,
	"Monolith.Blueprint.Authoring.EnumVariableUsesByteSubobjectConvention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintEnumVariableTypeRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_EnumVariableTypeRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	UEnum* CollisionChannelEnum = FindFirstObject<UEnum>(
		TEXT("ECollisionChannel"),
		EFindFirstObjectOptions::NativeFirst);
	if (!TestNotNull(TEXT("Native collision-channel enum is loaded"), CollisionChannelEnum))
	{
		return false;
	}

	auto VerifyRejectedVariableType = [this, &Fixture](const FString& VariableName, const FString& TypeString)
	{
		TSharedPtr<FJsonObject> InvalidVariableParams = MakeShared<FJsonObject>();
		InvalidVariableParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
		InvalidVariableParams->SetStringField(TEXT("name"), VariableName);
		InvalidVariableParams->SetStringField(TEXT("type"), TypeString);
		const FMonolithActionResult InvalidVariableResult =
			FMonolithBlueprintVariableActions::HandleAddVariable(InvalidVariableParams);
		TestFalse(
			FString::Printf(TEXT("Invalid type '%s' is rejected"), *TypeString),
			InvalidVariableResult.bSuccess);
		TestFalse(
			FString::Printf(TEXT("Invalid type '%s' leaves no member behind"), *TypeString),
			Fixture.Blueprint->NewVariables.ContainsByPredicate(
				[VariableName](const FBPVariableDescription& Candidate)
				{
					return Candidate.VarName == FName(*VariableName);
				}));
	};
	VerifyRejectedVariableType(TEXT("TypoEnumMustNotBecomeByte"), TEXT("enum:MonolithDefinitelyMissingEnum"));
	VerifyRejectedVariableType(TEXT("BareTypoMustNotBecomeBool"), TEXT("monolith_typo"));
	VerifyRejectedVariableType(TEXT("MissingObjectMustNotPersist"), TEXT("object:MonolithDefinitelyMissingClass"));
	VerifyRejectedVariableType(TEXT("MissingStructMustNotPersist"), TEXT("struct:MonolithDefinitelyMissingStruct"));

	TSharedPtr<FJsonObject> FunctionParams = MakeShared<FJsonObject>();
	FunctionParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	FunctionParams->SetStringField(TEXT("name"), TEXT("AtomicEnumSignature"));
	TestTrue(
		TEXT("Function for signature atomicity is authored"),
		FMonolithBlueprintGraphActions::HandleAddFunction(FunctionParams).bSuccess);

	TSharedPtr<FJsonObject> ValidInput = MakeShared<FJsonObject>();
	ValidInput->SetStringField(TEXT("name"), TEXT("ValidInput"));
	ValidInput->SetStringField(TEXT("type"), TEXT("int"));
	TSharedPtr<FJsonObject> InvalidOutput = MakeShared<FJsonObject>();
	InvalidOutput->SetStringField(TEXT("name"), TEXT("InvalidOutput"));
	InvalidOutput->SetStringField(TEXT("type"), TEXT("enum:MonolithDefinitelyMissingEnum"));
	TSharedPtr<FJsonObject> SignatureParams = MakeShared<FJsonObject>();
	SignatureParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	SignatureParams->SetStringField(TEXT("function_name"), TEXT("AtomicEnumSignature"));
	SignatureParams->SetArrayField(
		TEXT("inputs"), { MakeShared<FJsonValueObject>(ValidInput) });
	SignatureParams->SetArrayField(
		TEXT("outputs"), { MakeShared<FJsonValueObject>(InvalidOutput) });
	TestFalse(
		TEXT("A mixed valid/invalid signature is rejected before mutation"),
		FMonolithBlueprintGraphActions::HandleSetFunctionParams(SignatureParams).bSuccess);

	UEdGraph* AtomicFunctionGraph = FindGraph(Fixture.Blueprint, TEXT("AtomicEnumSignature"));
	UK2Node_FunctionEntry* AtomicEntry = nullptr;
	if (AtomicFunctionGraph)
	{
		for (UEdGraphNode* Node : AtomicFunctionGraph->Nodes)
		{
			if ((AtomicEntry = Cast<UK2Node_FunctionEntry>(Node)) != nullptr)
			{
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("Atomicity function entry exists"), AtomicEntry))
	{
		return false;
	}
	TestEqual(
		TEXT("Invalid output does not leave the earlier valid input pin behind"),
		AtomicEntry->UserDefinedPins.Num(),
		0);

	TSharedPtr<FJsonObject> VariableParams = MakeShared<FJsonObject>();
	VariableParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	VariableParams->SetStringField(TEXT("name"), TEXT("CollisionChannel"));
	VariableParams->SetStringField(TEXT("type"), TEXT("enum:ECollisionChannel"));
	TestTrue(
		TEXT("Enum member variable is authored"),
		FMonolithBlueprintVariableActions::HandleAddVariable(VariableParams).bSuccess);

	const FBPVariableDescription* Variable = Fixture.Blueprint->NewVariables.FindByPredicate(
		[](const FBPVariableDescription& Candidate)
		{
			return Candidate.VarName == FName(TEXT("CollisionChannel"));
		});
	if (!TestNotNull(TEXT("Enum variable descriptor exists"), Variable))
	{
		return false;
	}
	TestEqual(TEXT("Enum variable uses byte category"), Variable->VarType.PinCategory, UEdGraphSchema_K2::PC_Byte);
	TestEqual(
		TEXT("Enum variable retains its UEnum subobject"),
		Cast<UEnum>(Variable->VarType.PinSubCategoryObject.Get()),
		CollisionChannelEnum);

	TSharedPtr<FJsonObject> MapVariableParams = MakeShared<FJsonObject>();
	MapVariableParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	MapVariableParams->SetStringField(TEXT("name"), TEXT("ChannelCounts"));
	MapVariableParams->SetStringField(TEXT("type"), TEXT("map:enum:ECollisionChannel:int"));
	TestTrue(
		TEXT("Map parser preserves a named enum key boundary"),
		FMonolithBlueprintVariableActions::HandleAddVariable(MapVariableParams).bSuccess);
	const FBPVariableDescription* MapVariable = Fixture.Blueprint->NewVariables.FindByPredicate(
		[](const FBPVariableDescription& Candidate)
		{
			return Candidate.VarName == FName(TEXT("ChannelCounts"));
		});
	if (!TestNotNull(TEXT("Enum-keyed map descriptor exists"), MapVariable))
	{
		return false;
	}
	TestEqual(TEXT("Enum-keyed map keeps map container"), MapVariable->VarType.ContainerType, EPinContainerType::Map);
	TestEqual(TEXT("Enum-keyed map uses byte key category"), MapVariable->VarType.PinCategory, UEdGraphSchema_K2::PC_Byte);
	TestEqual(
		TEXT("Enum-keyed map retains its UEnum key subobject"),
		Cast<UEnum>(MapVariable->VarType.PinSubCategoryObject.Get()),
		CollisionChannelEnum);
	TestEqual(
		TEXT("Enum-keyed map retains its integer value type"),
		MapVariable->VarType.PinValueType.TerminalCategory,
		UEdGraphSchema_K2::PC_Int);

	const FString InvalidStructPackageName = FString::Printf(
		TEXT("/Game/Tests/Monolith/Automation/Blueprint/S_InvalidFields_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> ValidStructField = MakeShared<FJsonObject>();
	ValidStructField->SetStringField(TEXT("name"), TEXT("ValidField"));
	ValidStructField->SetStringField(TEXT("type"), TEXT("int"));
	TSharedPtr<FJsonObject> InvalidStructParams = MakeShared<FJsonObject>();
	InvalidStructParams->SetStringField(TEXT("save_path"), InvalidStructPackageName);
	InvalidStructParams->SetArrayField(
		TEXT("fields"),
		{
			MakeShared<FJsonValueString>(TEXT("not-an-object")),
			MakeShared<FJsonValueObject>(ValidStructField)
		});
	TestFalse(
		TEXT("Malformed struct fields are rejected before asset creation"),
		FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct(InvalidStructParams).bSuccess);
	TestNull(
		TEXT("Malformed struct fields leave no in-memory package"),
		FindPackage(nullptr, *InvalidStructPackageName));
	TestFalse(
		TEXT("Malformed struct fields leave no on-disk package"),
		FPackageName::DoesPackageExist(InvalidStructPackageName));

	TSharedPtr<FJsonObject> GetterParams = MakeShared<FJsonObject>();
	GetterParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	GetterParams->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
	GetterParams->SetStringField(TEXT("variable_name"), TEXT("CollisionChannel"));
	const FMonolithActionResult GetterResult =
		FMonolithBlueprintNodeActions::HandleAddNode(GetterParams);
	TestTrue(TEXT("Enum getter is authored"), GetterResult.bSuccess);

	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages.Num() > 0
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	UEdGraphPin* EnumPin = FindPinByName(
		FindNodeById(EventGraph, ResultString(GetterResult, TEXT("id"))),
		TEXT("CollisionChannel"));
	if (!TestNotNull(TEXT("Enum getter materializes its data pin"), EnumPin))
	{
		return false;
	}
	TestEqual(TEXT("Enum getter uses byte category"), EnumPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Byte);
	TestEqual(
		TEXT("Enum getter retains its UEnum subobject"),
		Cast<UEnum>(EnumPin->PinType.PinSubCategoryObject.Get()),
		CollisionChannelEnum);

	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("Blueprint with enum getter compiles"), Fixture.Blueprint->Status != BS_Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintRecursiveGraphDiscoveryRegressionTest,
	"Monolith.Blueprint.Authoring.RecursiveGraphDiscoveryAndReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintRecursiveGraphDiscoveryRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_RecursiveGraphDiscoveryRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages.Num() > 0
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
	{
		return false;
	}

	FGraphNodeCreator<UK2Node_Composite> CompositeCreator(*EventGraph);
	UK2Node_Composite* Composite = CompositeCreator.CreateNode(/*bSelectNewNode=*/false);
	CompositeCreator.Finalize();
	if (!TestNotNull(TEXT("Composite node created"), Composite)
		|| !TestNotNull(TEXT("Composite bound graph created"), Composite->BoundGraph.Get()))
	{
		return false;
	}
	Composite->OnRenameNode(TEXT("NestedAuditGraph"));
	UEdGraph* NestedGraph = Composite->BoundGraph;
	const FString NestedGraphName = NestedGraph->GetName();

	TSharedPtr<FJsonObject> VariableParams = MakeShared<FJsonObject>();
	VariableParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	VariableParams->SetStringField(TEXT("name"), TEXT("NestedAuditValue"));
	VariableParams->SetStringField(TEXT("type"), TEXT("int"));
	TestTrue(
		TEXT("Member used by the nested graph is authored"),
		FMonolithBlueprintVariableActions::HandleAddVariable(VariableParams).bSuccess);

	TSharedPtr<FJsonObject> GetterParams = MakeShared<FJsonObject>();
	GetterParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	GetterParams->SetStringField(TEXT("graph_name"), NestedGraphName);
	GetterParams->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
	GetterParams->SetStringField(TEXT("variable_name"), TEXT("NestedAuditValue"));
	TestTrue(
		TEXT("add_node resolves and writes into a recursively nested graph"),
		FMonolithBlueprintNodeActions::HandleAddNode(GetterParams).bSuccess);

	TSharedPtr<FJsonObject> AssetParams = MakeShared<FJsonObject>();
	AssetParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	const FMonolithActionResult ListResult = FMonolithBlueprintActions::HandleListGraphs(AssetParams);
	TestTrue(TEXT("list_graphs succeeds"), ListResult.bSuccess);
	int32 NestedListCount = 0;
	const TSharedPtr<FJsonObject> ListedNestedGraph = FindArrayObjectByStringField(
		ListResult, TEXT("graphs"), TEXT("name"), NestedGraphName, NestedListCount);
	TestEqual(TEXT("Nested graph is listed exactly once"), NestedListCount, 1);
	if (!TestTrue(TEXT("Nested graph has list metadata"), ListedNestedGraph.IsValid()))
	{
		return false;
	}
	FString ListedType;
	FString ListedParent;
	ListedNestedGraph->TryGetStringField(TEXT("type"), ListedType);
	ListedNestedGraph->TryGetStringField(TEXT("parent_graph"), ListedParent);
	TestEqual(TEXT("Nested graph is classified as a subgraph"), ListedType, FString(TEXT("subgraph")));
	TestEqual(TEXT("Nested graph reports its enclosing graph"), ListedParent, EventGraph->GetName());

	TSharedPtr<FJsonObject> GraphDataParams = MakeShared<FJsonObject>();
	GraphDataParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	GraphDataParams->SetStringField(TEXT("graph_name"), NestedGraphName);
	const FMonolithActionResult GraphDataResult =
		FMonolithBlueprintActions::HandleGetGraphData(GraphDataParams);
	TestTrue(TEXT("get_graph_data resolves the nested graph"), GraphDataResult.bSuccess);
	TestEqual(
		TEXT("get_graph_data preserves subgraph classification"),
		ResultString(GraphDataResult, TEXT("graph_type")),
		FString(TEXT("subgraph")));
	TestEqual(
		TEXT("get_graph_data preserves parent provenance"),
		ResultString(GraphDataResult, TEXT("parent_graph")),
		EventGraph->GetName());

	TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	SearchParams->SetStringField(TEXT("query"), TEXT("K2Node_VariableGet"));
	const FMonolithActionResult SearchResult = FMonolithBlueprintActions::HandleSearchNodes(SearchParams);
	TestTrue(TEXT("search_nodes succeeds"), SearchResult.bSuccess);
	int32 NestedSearchCount = 0;
	const TSharedPtr<FJsonObject> NestedSearchMatch = FindArrayObjectByStringField(
		SearchResult, TEXT("results"), TEXT("graph"), NestedGraphName, NestedSearchCount);
	TestEqual(TEXT("Nested getter appears once in search results"), NestedSearchCount, 1);
	TestEqual(
		TEXT("Nested search result has subgraph provenance"),
		NestedSearchMatch.IsValid() ? NestedSearchMatch->GetStringField(TEXT("graph_type")) : FString(),
		FString(TEXT("subgraph")));

	TSharedPtr<FJsonObject> ReferenceParams = MakeShared<FJsonObject>();
	ReferenceParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	ReferenceParams->SetStringField(TEXT("variable_name"), TEXT("NestedAuditValue"));
	const FMonolithActionResult ReferenceResult =
		FMonolithBlueprintActions::HandleFindVariableReferences(ReferenceParams);
	TestTrue(TEXT("find_variable_references succeeds"), ReferenceResult.bSuccess);
	int32 NestedReferenceCount = 0;
	const TSharedPtr<FJsonObject> NestedReference = FindArrayObjectByStringField(
		ReferenceResult, TEXT("references"), TEXT("graph"), NestedGraphName, NestedReferenceCount);
	TestEqual(TEXT("Nested variable reference appears exactly once"), NestedReferenceCount, 1);
	TestEqual(
		TEXT("Nested reference has parent provenance"),
		NestedReference.IsValid() ? NestedReference->GetStringField(TEXT("parent_graph")) : FString(),
		EventGraph->GetName());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintTimelineTrackRegressionTest,
	"Monolith.Blueprint.Authoring.TimelineTrackReconstructsPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintTimelineTrackRegressionTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintAuthoringRegressionTests;
	FScopedBlueprintAsset Fixture(TEXT("BP_TimelineTrackRegression"));
	if (!TestNotNull(TEXT("Disposable Blueprint created"), Fixture.Blueprint))
	{
		return false;
	}

	TSharedPtr<FJsonObject> TimelineParams = MakeShared<FJsonObject>();
	TimelineParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	TimelineParams->SetStringField(TEXT("timeline_name"), TEXT("Motion"));
	const FMonolithActionResult TimelineResult =
		FMonolithBlueprintNodeActions::HandleAddTimeline(TimelineParams);
	if (!TestTrue(TEXT("Timeline created"), TimelineResult.bSuccess))
	{
		return false;
	}

	UK2Node_Timeline* TimelineNode = nullptr;
	for (UEdGraph* Graph : Fixture.Blueprint->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* Candidate = Cast<UK2Node_Timeline>(Node);
			if (Candidate && Candidate->TimelineName == FName(TEXT("Motion")))
			{
				TimelineNode = Candidate;
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("Timeline graph node exists"), TimelineNode))
	{
		return false;
	}

	TSharedPtr<FJsonObject> TrackParams = MakeShared<FJsonObject>();
	TrackParams->SetStringField(TEXT("asset_path"), Fixture.GetAssetPath());
	TrackParams->SetStringField(TEXT("timeline_name"), TEXT("Motion"));
	TrackParams->SetStringField(TEXT("track_name"), TEXT("TrackAlpha"));
	TrackParams->SetStringField(TEXT("track_type"), TEXT("float"));
	const FMonolithActionResult TrackResult =
		FMonolithBlueprintNodeActions::HandleAddTimelineTrack(TrackParams);
	TestTrue(TEXT("Timeline track created"), TrackResult.bSuccess);

	double NodesReconstructed = 0.0;
	TestTrue(
		TEXT("add_timeline_track reports reconstructing the matching timeline node"),
		TrackResult.Result.IsValid() &&
		TrackResult.Result->TryGetNumberField(TEXT("nodes_reconstructed"), NodesReconstructed) &&
		NodesReconstructed == 1.0);

	bool bTrackPinFound = false;
	for (const UEdGraphPin* Pin : TimelineNode->Pins)
	{
		if (Pin && Pin->PinName == FName(TEXT("TrackAlpha")))
		{
			bTrackPinFound = true;
			break;
		}
	}
	TestTrue(TEXT("The reconstructed timeline node exposes the new track pin"), bTrackPinFound);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
