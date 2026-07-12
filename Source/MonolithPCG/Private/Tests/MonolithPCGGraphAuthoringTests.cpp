#include "CoreMinimal.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"

#include "MonolithPCGActions.h"
#include "MonolithPCGGraphAuthoringActions.h"
#include "MonolithPCGSettingsResolver.h"
#include "MonolithToolRegistry.h"

#include "Elements/PCGAddTag.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace MonolithPCGGraphAuthoringTests
{
FString GetObjectPath(const FString& PackageName)
{
	return PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
}

FString GetPackageFilename(const FString& PackageName)
{
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, Filename, FPackageName::GetAssetPackageExtension()))
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(Filename);
}

void CloseAssetEditors(UObject* Asset)
{
	if (Asset && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
		}
	}
}

void RefreshAssetRegistryAfterDelete(const FString& PackageName, const FString& Filename)
{
	if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
	{
		if (!Filename.IsEmpty())
		{
			AssetRegistry->ScanModifiedAssetFiles({Filename});
		}
		AssetRegistry->ScanPathsSynchronous(
			{FPackageName::GetLongPackagePath(PackageName)}, /*bForceRescan=*/true);
	}
}

void CleanupPackage(const FString& PackageName)
{
	const FString ObjectPath = GetObjectPath(PackageName);
	const FString Filename = GetPackageFilename(PackageName);

	UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath);
	if (!Asset && !Filename.IsEmpty() && IFileManager::Get().FileExists(*Filename))
	{
		Asset = LoadObject<UObject>(nullptr, *ObjectPath);
	}

	if (Asset)
	{
		CloseAssetEditors(Asset);
		if (GEditor)
		{
			TArray<UObject*> AssetsToDelete = {Asset};
			ObjectTools::ForceDeleteObjects(AssetsToDelete, /*ShowConfirmation=*/false);
		}
	}

	if (UPackage* RemainingPackage = FindPackage(nullptr, *PackageName))
	{
		ResetLoaders(RemainingPackage);
		RemainingPackage->SetDirtyFlag(false);

		TArray<UObject*> PackageObjects;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		GetObjectsWithPackage(RemainingPackage, PackageObjects, EGetObjectsFlags::IncludeNestedObjects);
#else
		GetObjectsWithPackage(RemainingPackage, PackageObjects, true);
#endif
		for (UObject* Object : PackageObjects)
		{
			if (Object)
			{
				Object->ClearFlags(RF_Standalone);
				Object->MarkAsGarbage();
			}
		}
		RemainingPackage->MarkAsGarbage();
	}
	CollectGarbage(RF_NoFlags);

	if (!Filename.IsEmpty())
	{
		IFileManager::Get().Delete(*Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("uexp")), false, true, true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("ubulk")), false, true, true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("uptnl")), false, true, true);
	}
	RefreshAssetRegistryAfterDelete(PackageName, Filename);
}

class FScopedPCGGraphFixture
{
public:
	explicit FScopedPCGGraphFixture(const TCHAR* InAssetName)
		: PackageName(FString::Printf(TEXT("/Game/Developers/MonolithTests/PCG/%s"), InAssetName))
		, ObjectPath(GetObjectPath(PackageName))
		, Filename(GetPackageFilename(PackageName))
	{
		Cleanup();
	}

	~FScopedPCGGraphFixture()
	{
		Cleanup();
	}

	void Cleanup() const
	{
		CleanupPackage(PackageName);
	}

	bool ExistsOnDisk() const
	{
		return !Filename.IsEmpty() && IFileManager::Get().FileExists(*Filename);
	}

	bool ExistsInAssetRegistry() const
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (!AssetRegistry)
		{
			return false;
		}
		TArray<FAssetData> Assets;
		AssetRegistry->GetAssetsByPackageName(FName(*PackageName), Assets);
		return !Assets.IsEmpty();
	}

	FString PackageName;
	FString ObjectPath;
	FString Filename;
};

void RegisterGraphAuthoringActions()
{
	FMonolithPCGGraphAuthoringActions::RegisterActions(FMonolithToolRegistry::Get());
}

TSharedPtr<FJsonObject> MakeAssetParams(const FScopedPCGGraphFixture& Fixture)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Fixture.PackageName);
	return Params;
}

FMonolithActionResult ExecuteAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("pcg"), Action, Params);
}

bool RequireSuccess(FAutomationTestBase& Test, const FString& Context, const FMonolithActionResult& Result)
{
	if (!Result.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("%s failed: %s"), *Context, *Result.ErrorMessage));
		return false;
	}
	if (!Result.Result.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s returned success without a result object"), *Context));
		return false;
	}
	return true;
}

bool ReadRequiredString(
	FAutomationTestBase& Test,
	const FString& Context,
	const FMonolithActionResult& Result,
	const TCHAR* Field,
	FString& OutValue)
{
	if (!Result.Result.IsValid() || !Result.Result->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		Test.AddError(FString::Printf(TEXT("%s did not return non-empty '%s'"), *Context, Field));
		return false;
	}
	return true;
}

FMonolithActionResult CreateGraph(const FScopedPCGGraphFixture& Fixture)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture);
	Params->SetBoolField(TEXT("save"), true);
	return ExecuteAction(TEXT("create_pcg_graph"), Params);
}

FMonolithActionResult AddNode(
	const FScopedPCGGraphFixture& Fixture,
	const FString& NodeType,
	const FString& NodeTitle)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture);
	Params->SetStringField(TEXT("node_type"), NodeType);
	Params->SetStringField(TEXT("node_title"), NodeTitle);
	Params->SetBoolField(TEXT("save"), true);
	return ExecuteAction(TEXT("add_pcg_node"), Params);
}

FMonolithActionResult ConnectNodes(
	const FScopedPCGGraphFixture& Fixture,
	const FString& SourceNode,
	const FString& SourcePin,
	const FString& TargetNode,
	const FString& TargetPin)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture);
	Params->SetStringField(TEXT("source_node"), SourceNode);
	Params->SetStringField(TEXT("source_pin"), SourcePin);
	Params->SetStringField(TEXT("target_node"), TargetNode);
	Params->SetStringField(TEXT("target_pin"), TargetPin);
	Params->SetBoolField(TEXT("save"), true);
	return ExecuteAction(TEXT("connect_pcg_nodes"), Params);
}

FMonolithActionResult DisconnectNodes(
	const FScopedPCGGraphFixture& Fixture,
	const FString& SourceNode,
	const FString& SourcePin,
	const FString& TargetNode,
	const FString& TargetPin)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture);
	Params->SetStringField(TEXT("source_node"), SourceNode);
	Params->SetStringField(TEXT("source_pin"), SourcePin);
	Params->SetStringField(TEXT("target_node"), TargetNode);
	Params->SetStringField(TEXT("target_pin"), TargetPin);
	Params->SetBoolField(TEXT("save"), true);
	return ExecuteAction(TEXT("disconnect_pcg_nodes"), Params);
}

UPCGGraph* LoadGraph(const FScopedPCGGraphFixture& Fixture)
{
	return LoadObject<UPCGGraph>(nullptr, *Fixture.ObjectPath);
}

UPCGGraph* ReloadGraph(FAutomationTestBase& Test, const FScopedPCGGraphFixture& Fixture)
{
	UPCGGraph* Graph = LoadGraph(Fixture);
	if (!Graph)
	{
		Test.AddError(FString::Printf(TEXT("Could not load fixture before reload: %s"), *Fixture.ObjectPath));
		return nullptr;
	}

	CloseAssetEditors(Graph);
	UPackage* Package = Graph->GetOutermost();
	Package->SetDirtyFlag(false);
	FText ReloadError;
	if (!UPackageTools::ReloadPackages(
			{Package}, ReloadError, EReloadPackagesInteractionMode::AssumeNegative))
	{
		Test.AddError(FString::Printf(
			TEXT("Could not reload fixture %s: %s"), *Fixture.PackageName, *ReloadError.ToString()));
		return nullptr;
	}

	Graph = LoadGraph(Fixture);
	if (!Graph)
	{
		Test.AddError(FString::Printf(TEXT("Could not load fixture after reload: %s"), *Fixture.ObjectPath));
	}
	return Graph;
}

UPCGNode* FindNodeByTitle(UPCGGraph* Graph, const FString& NodeTitle)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (Node && Node->GetAuthoredTitleName().ToString() == NodeTitle)
		{
			return Node;
		}
	}
	return nullptr;
}

int32 CountNodesByTitle(const UPCGGraph* Graph, const FString& NodeTitle)
{
	int32 Count = 0;
	if (Graph)
	{
		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (Node && Node->GetAuthoredTitleName().ToString() == NodeTitle)
			{
				++Count;
			}
		}
	}
	return Count;
}

TSharedPtr<FJsonObject> FindSerializedNodeByTitle(
	const TSharedPtr<FJsonObject>& GraphInfo,
	const FString& NodeTitle)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!GraphInfo.IsValid() || !GraphInfo->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
		FString AuthoredTitle;
		if (Node.IsValid() && Node->TryGetStringField(TEXT("authored_title"), AuthoredTitle) &&
			AuthoredTitle == NodeTitle)
		{
			return Node;
		}
	}
	return nullptr;
}

bool HasConnection(
	UPCGNode* SourceNode,
	const TCHAR* SourcePinLabel,
	UPCGNode* TargetNode,
	const TCHAR* TargetPinLabel)
{
	if (!SourceNode || !TargetNode)
	{
		return false;
	}
	const UPCGPin* SourcePin = SourceNode->GetOutputPin(FName(SourcePinLabel));
	const UPCGPin* TargetPin = TargetNode->GetInputPin(FName(TargetPinLabel));
	if (!SourcePin || !TargetPin)
	{
		return false;
	}
	for (const UPCGEdge* Edge : SourcePin->Edges)
	{
		if (Edge && Edge->InputPin == SourcePin && Edge->OutputPin == TargetPin)
		{
			return true;
		}
	}
	return false;
}

int32 CountGraphEdges(const UPCGGraph* Graph)
{
	if (!Graph)
	{
		return 0;
	}

	int32 EdgeCount = 0;
	auto CountNodeOutputs = [&EdgeCount](const UPCGNode* Node)
	{
		if (!Node)
		{
			return;
		}
		for (const UPCGPin* Pin : Node->GetOutputPins())
		{
			if (Pin)
			{
				EdgeCount += Pin->Edges.Num();
			}
		}
	};

	CountNodeOutputs(Graph->GetInputNode());
	for (const UPCGNode* Node : Graph->GetNodes())
	{
		CountNodeOutputs(Node);
	}
	CountNodeOutputs(Graph->GetOutputNode());
	return EdgeCount;
}

bool ErrorContainsAny(const FString& Error, const TArray<FString>& Terms)
{
	for (const FString& Term : Terms)
	{
		if (Error.Contains(Term, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}
} // namespace MonolithPCGGraphAuthoringTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphCreateSaveReloadTest,
	"Monolith.PCG.GraphAuthoring.Asset.CreateSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphCreateSaveReloadTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_CreateSaveReload"));
	RegisterGraphAuthoringActions();

	const FMonolithActionResult CreateResult = CreateGraph(Fixture);
	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateResult))
	{
		return false;
	}

	FString ReturnedPath;
	TestTrue(
		TEXT("Create returns the canonical object path"),
		ReadRequiredString(*this, TEXT("create_pcg_graph"), CreateResult, TEXT("asset_path"), ReturnedPath));
	TestEqual(TEXT("Canonical object path is stable"), ReturnedPath, Fixture.ObjectPath);

	bool bSaved = false;
	TestTrue(
		TEXT("Create reports a persisted package"),
		CreateResult.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
	TestTrue(TEXT("Created PCG graph exists on disk"), Fixture.ExistsOnDisk());
	TestTrue(TEXT("Created PCG graph is visible in the asset registry"), Fixture.ExistsInAssetRegistry());

	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (!ReloadedGraph)
	{
		return false;
	}
	TestEqual(TEXT("Reload preserves the PCG graph object path"), ReloadedGraph->GetPathName(), Fixture.ObjectPath);
	TestEqual(TEXT("Fresh graph has no element nodes"), ReloadedGraph->GetNodes().Num(), 0);
	TestEqual(TEXT("Fresh graph has no edges"), CountGraphEdges(ReloadedGraph), 0);
	TestFalse(TEXT("Reloaded saved package is clean"), ReloadedGraph->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphEditSaveReloadTest,
	"Monolith.PCG.GraphAuthoring.Asset.EditSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphEditSaveReloadTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_EditSaveReload"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}

	const FMonolithActionResult AddResult = AddNode(
		Fixture, TEXT("PCGAddTagSettings"), TEXT("Benchmark_AddTags"));
	if (!RequireSuccess(*this, TEXT("add_pcg_node"), AddResult))
	{
		return false;
	}
	FString NodeId;
	if (!ReadRequiredString(*this, TEXT("add_pcg_node"), AddResult, TEXT("node_id"), NodeId))
	{
		return false;
	}

	const FString ExpectedTags = TEXT("Monolith.Automation,Benchmark");
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("TagsToAdd"), ExpectedTags);
	TSharedPtr<FJsonObject> SetParams = MakeAssetParams(Fixture);
	SetParams->SetStringField(TEXT("node_id"), NodeId);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	SetParams->SetBoolField(TEXT("save"), true);
	if (!RequireSuccess(
			*this,
			TEXT("set_pcg_node_params"),
			ExecuteAction(TEXT("set_pcg_node_params"), SetParams)))
	{
		return false;
	}

	if (!RequireSuccess(
			*this,
			TEXT("connect graph input to Add Tags"),
			ConnectNodes(Fixture, TEXT("__input__"), TEXT("In"), NodeId, TEXT("In"))) ||
		!RequireSuccess(
			*this,
			TEXT("connect Add Tags to graph output"),
			ConnectNodes(Fixture, NodeId, TEXT("Out"), TEXT("__output__"), TEXT("Out"))))
	{
		return false;
	}

	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (!ReloadedGraph)
	{
		return false;
	}
	UPCGNode* ReloadedNode = FindNodeByTitle(ReloadedGraph, TEXT("Benchmark_AddTags"));
	TestNotNull(TEXT("Authored node persists after package reload"), ReloadedNode);
	if (!ReloadedNode)
	{
		return false;
	}

	const UPCGAddTagSettings* ReloadedSettings = Cast<UPCGAddTagSettings>(ReloadedNode->GetSettings());
	TestNotNull(TEXT("Reloaded node preserves UPCGAddTagSettings type"), ReloadedSettings);
	if (ReloadedSettings)
	{
		TestEqual(TEXT("Strict settings write persists after reload"), ReloadedSettings->TagsToAdd, ExpectedTags);
	}
	TestEqual(TEXT("Element node persists after reload"), ReloadedGraph->GetNodes().Num(), 1);
	TestEqual(TEXT("Both authored edges persist after reload"), CountGraphEdges(ReloadedGraph), 2);
	TestTrue(
		TEXT("Graph input edge persists"),
		HasConnection(
			ReloadedGraph->GetInputNode(), TEXT("In"), ReloadedNode, TEXT("In")));
	TestTrue(
		TEXT("Graph output edge persists"),
		HasConnection(
			ReloadedNode, TEXT("Out"), ReloadedGraph->GetOutputNode(), TEXT("Out")));

	TSharedPtr<FJsonObject> ValidateParams = MakeAssetParams(Fixture);
	ValidateParams->SetBoolField(TEXT("require_output_connection"), true);
	ValidateParams->SetBoolField(TEXT("require_no_isolated_nodes"), true);
	const FMonolithActionResult ValidateResult = ExecuteAction(TEXT("validate_pcg_graph"), ValidateParams);
	if (!RequireSuccess(*this, TEXT("validate_pcg_graph"), ValidateResult))
	{
		return false;
	}
	bool bValid = false;
	TestTrue(
		TEXT("Reloaded authored graph passes strict structural validation"),
		ValidateResult.Result->TryGetBoolField(TEXT("valid"), bValid) && bValid);
	TestFalse(TEXT("Reloaded saved package remains clean"), ReloadedGraph->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphAmbiguousAuthoredTitleRejectTest,
	"Monolith.PCG.GraphAuthoring.Node.AmbiguousAuthoredTitleReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphAmbiguousAuthoredTitleRejectTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_AmbiguousAuthoredTitle"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	UPCGGraph* Graph = LoadGraph(Fixture);
	TestNotNull(TEXT("Ambiguous-title fixture graph is loadable"), Graph);
	if (!Graph)
	{
		return false;
	}

	UPCGAddTagSettings* FirstSettings = nullptr;
	UPCGAddTagSettings* SecondSettings = nullptr;
	UPCGNode* FirstNode = Graph->AddNodeOfType(FirstSettings);
	UPCGNode* SecondNode = Graph->AddNodeOfType(SecondSettings);
	TestNotNull(TEXT("UE API creates the first duplicate-title fixture node"), FirstNode);
	TestNotNull(TEXT("UE API creates the second duplicate-title fixture node"), SecondNode);
	if (!FirstNode || !SecondNode)
	{
		return false;
	}

	const FString DuplicateTitle = TEXT("Ambiguous_Title");
	FirstNode->SetNodeTitle(FName(*DuplicateTitle));
	SecondNode->SetNodeTitle(FName(*DuplicateTitle));
	TestEqual(TEXT("Fixture contains exactly two nodes before the rejected action"), Graph->GetNodes().Num(), 2);
	TestEqual(TEXT("UE fixture preserves both duplicate authored titles"),
		CountNodesByTitle(Graph, DuplicateTitle), 2);

	TSharedPtr<FJsonObject> AddParams = MakeAssetParams(Fixture);
	AddParams->SetStringField(TEXT("node_type"), TEXT("PCGAddTagSettings"));
	AddParams->SetStringField(TEXT("node_title"), DuplicateTitle);
	AddParams->SetStringField(TEXT("existing_policy"), TEXT("return_existing"));
	AddParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult AddResult = ExecuteAction(TEXT("add_pcg_node"), AddParams);
	TestFalse(TEXT("add_pcg_node rejects an ambiguous authored-title lookup"), AddResult.bSuccess);
	TestTrue(TEXT("Ambiguous-title rejection identifies the ambiguity"),
		AddResult.ErrorMessage.Contains(TEXT("ambiguous"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("Rejected ambiguous-title add leaves the graph node count unchanged"),
		Graph->GetNodes().Num(), 2);
	TestEqual(TEXT("Rejected ambiguous-title add does not create a third title match"),
		CountNodesByTitle(Graph, DuplicateTitle), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphInfoResponseBoundsTest,
	"Monolith.PCG.GraphAuthoring.Read.GraphInfoResponseBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphInfoResponseBoundsTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_GraphInfoResponseBounds"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)) ||
		!RequireSuccess(
			*this,
			TEXT("add bounded projection node"),
			AddNode(Fixture, TEXT("PCGProjectionSettings"), TEXT("Bounded_Projection"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> PinBoundParams = MakeAssetParams(Fixture);
	PinBoundParams->SetNumberField(TEXT("pin_limit"), 1);
	const FMonolithActionResult PinBoundResult =
		ExecuteAction(TEXT("get_pcg_graph_info"), PinBoundParams);
	if (!RequireSuccess(*this, TEXT("get_pcg_graph_info with pin_limit"), PinBoundResult))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> ProjectionNode =
		FindSerializedNodeByTitle(PinBoundResult.Result, TEXT("Bounded_Projection"));
	TestTrue(TEXT("Bounded graph info still returns the authored projection node"), ProjectionNode.IsValid());
	if (!ProjectionNode.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* InputPins = nullptr;
	bool bInputPinsTruncated = false;
	TestTrue(TEXT("pin_limit bounds the projection node's returned input pins"),
		ProjectionNode->TryGetArrayField(TEXT("input_pins"), InputPins) && InputPins && InputPins->Num() == 1);
	TestTrue(TEXT("Projection node reports its input-pin truncation"),
		ProjectionNode->TryGetBoolField(TEXT("input_pins_truncated"), bInputPinsTruncated) &&
			bInputPinsTruncated);

	TSharedPtr<FJsonObject> ResponseBoundParams = MakeAssetParams(Fixture);
	ResponseBoundParams->SetNumberField(TEXT("response_item_limit"), 1);
	const FMonolithActionResult ResponseBoundResult =
		ExecuteAction(TEXT("get_pcg_graph_info"), ResponseBoundParams);
	if (!RequireSuccess(*this, TEXT("get_pcg_graph_info with response_item_limit"), ResponseBoundResult))
	{
		return false;
	}
	double ResponseItemLimit = 0.0;
	double ReturnedResponseItemCount = 0.0;
	bool bResponseTruncated = false;
	TestTrue(TEXT("Graph info echoes the enforced response item limit"),
		ResponseBoundResult.Result->TryGetNumberField(TEXT("response_item_limit"), ResponseItemLimit) &&
			ResponseItemLimit == 1.0);
	TestTrue(TEXT("Graph info reports a bounded non-empty response item count"),
		ResponseBoundResult.Result->TryGetNumberField(
			TEXT("returned_response_item_count"), ReturnedResponseItemCount) &&
			ReturnedResponseItemCount > 0.0 && ReturnedResponseItemCount <= ResponseItemLimit);
	TestTrue(TEXT("Graph info reports top-level response truncation when the shared budget is exhausted"),
		ResponseBoundResult.Result->TryGetBoolField(TEXT("response_truncated"), bResponseTruncated) &&
			bResponseTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphSelfEdgeRejectTest,
	"Monolith.PCG.GraphAuthoring.Edge.SelfEdgeReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphSelfEdgeRejectTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_SelfEdgeReject"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	const FMonolithActionResult AddResult = AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("SelfNode"));
	if (!RequireSuccess(*this, TEXT("add SelfNode"), AddResult))
	{
		return false;
	}
	FString NodeId;
	if (!ReadRequiredString(*this, TEXT("add SelfNode"), AddResult, TEXT("node_id"), NodeId))
	{
		return false;
	}

	const FMonolithActionResult ConnectResult =
		ConnectNodes(Fixture, NodeId, TEXT("Out"), NodeId, TEXT("In"));
	TestFalse(TEXT("A PCG node cannot connect to itself"), ConnectResult.bSuccess);
	TestTrue(
		TEXT("Self-edge rejection explains the violated invariant"),
		ConnectResult.ErrorMessage.Contains(TEXT("self"), ESearchCase::IgnoreCase));

	UPCGGraph* InMemoryGraph = LoadGraph(Fixture);
	TestNotNull(TEXT("Graph remains loadable after rejected self-edge"), InMemoryGraph);
	TestEqual(TEXT("Rejected self-edge does not mutate the in-memory graph"), CountGraphEdges(InMemoryGraph), 0);
	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (ReloadedGraph)
	{
		TestEqual(TEXT("Rejected self-edge is not persisted"), CountGraphEdges(ReloadedGraph), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphCycleRejectTest,
	"Monolith.PCG.GraphAuthoring.Edge.MultiNodeCycleReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphCycleRejectTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_MultiNodeCycleReject"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	const FMonolithActionResult AddA = AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("Cycle_A"));
	const FMonolithActionResult AddB = AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("Cycle_B"));
	if (!RequireSuccess(*this, TEXT("add Cycle_A"), AddA) ||
		!RequireSuccess(*this, TEXT("add Cycle_B"), AddB))
	{
		return false;
	}
	FString NodeAId;
	FString NodeBId;
	if (!ReadRequiredString(*this, TEXT("add Cycle_A"), AddA, TEXT("node_id"), NodeAId) ||
		!ReadRequiredString(*this, TEXT("add Cycle_B"), AddB, TEXT("node_id"), NodeBId))
	{
		return false;
	}

	if (!RequireSuccess(
			*this,
			TEXT("connect Cycle_A to Cycle_B"),
			ConnectNodes(Fixture, NodeAId, TEXT("Out"), NodeBId, TEXT("In"))))
	{
		return false;
	}
	const FMonolithActionResult CycleResult =
		ConnectNodes(Fixture, NodeBId, TEXT("Out"), NodeAId, TEXT("In"));
	TestFalse(TEXT("A back-edge that closes a multi-node cycle is rejected"), CycleResult.bSuccess);
	TestTrue(
		TEXT("Cycle rejection explains the violated invariant"),
		CycleResult.ErrorMessage.Contains(TEXT("cycle"), ESearchCase::IgnoreCase));

	UPCGGraph* InMemoryGraph = LoadGraph(Fixture);
	UPCGNode* NodeA = FindNodeByTitle(InMemoryGraph, TEXT("Cycle_A"));
	UPCGNode* NodeB = FindNodeByTitle(InMemoryGraph, TEXT("Cycle_B"));
	TestEqual(TEXT("Rejected back-edge does not change the in-memory edge count"), CountGraphEdges(InMemoryGraph), 1);
	TestTrue(TEXT("Original acyclic edge is preserved"), HasConnection(NodeA, TEXT("Out"), NodeB, TEXT("In")));
	TestFalse(TEXT("Rejected cyclic edge is absent"), HasConnection(NodeB, TEXT("Out"), NodeA, TEXT("In")));

	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (ReloadedGraph)
	{
		NodeA = FindNodeByTitle(ReloadedGraph, TEXT("Cycle_A"));
		NodeB = FindNodeByTitle(ReloadedGraph, TEXT("Cycle_B"));
		TestEqual(TEXT("Only the original edge persists"), CountGraphEdges(ReloadedGraph), 1);
		TestTrue(TEXT("Original edge persists after reload"), HasConnection(NodeA, TEXT("Out"), NodeB, TEXT("In")));
		TestFalse(TEXT("Cyclic back-edge is not persisted"), HasConnection(NodeB, TEXT("Out"), NodeA, TEXT("In")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphSingleInputReplacementRejectTest,
	"Monolith.PCG.GraphAuthoring.Edge.SingleInputImplicitReplacementReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphSingleInputReplacementRejectTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_SingleInputReplacementReject"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	const FMonolithActionResult AddA =
		AddNode(Fixture, TEXT("PCGCreatePointsGridSettings"), TEXT("SingleSource_A"));
	const FMonolithActionResult AddB =
		AddNode(Fixture, TEXT("PCGCreatePointsGridSettings"), TEXT("SingleSource_B"));
	const FMonolithActionResult AddTarget =
		AddNode(Fixture, TEXT("PCGProjectionSettings"), TEXT("SingleTarget"));
	if (!RequireSuccess(*this, TEXT("add SingleSource_A"), AddA) ||
		!RequireSuccess(*this, TEXT("add SingleSource_B"), AddB) ||
		!RequireSuccess(*this, TEXT("add SingleTarget"), AddTarget))
	{
		return false;
	}
	FString NodeAId;
	FString NodeBId;
	FString TargetId;
	if (!ReadRequiredString(*this, TEXT("add SingleSource_A"), AddA, TEXT("node_id"), NodeAId) ||
		!ReadRequiredString(*this, TEXT("add SingleSource_B"), AddB, TEXT("node_id"), NodeBId) ||
		!ReadRequiredString(*this, TEXT("add SingleTarget"), AddTarget, TEXT("node_id"), TargetId))
	{
		return false;
	}

	UPCGGraph* Graph = LoadGraph(Fixture);
	UPCGNode* TargetNode = FindNodeByTitle(Graph, TEXT("SingleTarget"));
	const UPCGPin* SingleInput =
		TargetNode ? TargetNode->GetInputPin(FName(TEXT("Projection Target"))) : nullptr;
	TestNotNull(TEXT("Projection Target exposes the expected deterministic input pin"), SingleInput);
	if (!SingleInput)
	{
		return false;
	}
	TestFalse(TEXT("Projection Target input rejects multiple connections by engine contract"),
		SingleInput->AllowsMultipleConnections());

	if (!RequireSuccess(
			*this,
			TEXT("connect first source to occupied-input fixture"),
			ConnectNodes(Fixture, NodeAId, TEXT("Out"), TargetId, TEXT("Projection Target"))))
	{
		return false;
	}
	const FMonolithActionResult ReplacementResult =
		ConnectNodes(Fixture, NodeBId, TEXT("Out"), TargetId, TEXT("Projection Target"));
	TestFalse(TEXT("Connecting a second source does not implicitly replace a single-input edge"),
		ReplacementResult.bSuccess);
	TestTrue(
		TEXT("Occupied-input rejection explains replacement policy"),
		ErrorContainsAny(
			ReplacementResult.ErrorMessage,
			{TEXT("already connected"), TEXT("multiple"), TEXT("replace"), TEXT("occupied")}));

	Graph = LoadGraph(Fixture);
	UPCGNode* NodeA = FindNodeByTitle(Graph, TEXT("SingleSource_A"));
	UPCGNode* NodeB = FindNodeByTitle(Graph, TEXT("SingleSource_B"));
	TargetNode = FindNodeByTitle(Graph, TEXT("SingleTarget"));
	TestTrue(
		TEXT("Original single-input edge remains connected"),
		HasConnection(NodeA, TEXT("Out"), TargetNode, TEXT("Projection Target")));
	TestFalse(
		TEXT("Rejected replacement source remains disconnected"),
		HasConnection(NodeB, TEXT("Out"), TargetNode, TEXT("Projection Target")));
	TestEqual(TEXT("Rejected replacement keeps one in-memory edge"), CountGraphEdges(Graph), 1);

	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (ReloadedGraph)
	{
		NodeA = FindNodeByTitle(ReloadedGraph, TEXT("SingleSource_A"));
		NodeB = FindNodeByTitle(ReloadedGraph, TEXT("SingleSource_B"));
		TargetNode = FindNodeByTitle(ReloadedGraph, TEXT("SingleTarget"));
		TestTrue(
			TEXT("Original single-input edge persists"),
			HasConnection(NodeA, TEXT("Out"), TargetNode, TEXT("Projection Target")));
		TestFalse(
			TEXT("Implicit replacement edge is not persisted"),
			HasConnection(NodeB, TEXT("Out"), TargetNode, TEXT("Projection Target")));
		TestEqual(TEXT("Reloaded graph retains exactly one edge"), CountGraphEdges(ReloadedGraph), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphHalfAttachedEdgeRejectTest,
	"Monolith.PCG.GraphAuthoring.Validation.HalfAttachedEdgeReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphHalfAttachedEdgeRejectTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_HalfAttachedEdgeReject"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	const FMonolithActionResult AddSource =
		AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("HalfEdge_Source"));
	const FMonolithActionResult AddTarget =
		AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("HalfEdge_Target"));
	if (!RequireSuccess(*this, TEXT("add HalfEdge_Source"), AddSource) ||
		!RequireSuccess(*this, TEXT("add HalfEdge_Target"), AddTarget))
	{
		return false;
	}
	FString SourceId;
	FString TargetId;
	if (!ReadRequiredString(*this, TEXT("add HalfEdge_Source"), AddSource, TEXT("node_id"), SourceId) ||
		!ReadRequiredString(*this, TEXT("add HalfEdge_Target"), AddTarget, TEXT("node_id"), TargetId) ||
		!RequireSuccess(
			*this,
			TEXT("connect half-edge fixture"),
			ConnectNodes(Fixture, SourceId, TEXT("Out"), TargetId, TEXT("In"))))
	{
		return false;
	}

	UPCGGraph* Graph = LoadGraph(Fixture);
	UPCGNode* SourceNode = FindNodeByTitle(Graph, TEXT("HalfEdge_Source"));
	UPCGNode* TargetNode = FindNodeByTitle(Graph, TEXT("HalfEdge_Target"));
	UPCGPin* SourcePin = SourceNode ? SourceNode->GetOutputPin(FName(TEXT("Out"))) : nullptr;
	UPCGPin* TargetPin = TargetNode ? TargetNode->GetInputPin(FName(TEXT("In"))) : nullptr;
	TestNotNull(TEXT("Half-edge fixture source pin exists"), SourcePin);
	TestNotNull(TEXT("Half-edge fixture target pin exists"), TargetPin);
	if (!SourcePin || !TargetPin)
	{
		return false;
	}

	UPCGEdge* FixtureEdge = nullptr;
	for (UPCGEdge* Edge : SourcePin->Edges)
	{
		if (Edge && Edge->InputPin == SourcePin && Edge->OutputPin == TargetPin)
		{
			FixtureEdge = Edge;
			break;
		}
	}
	TestNotNull(TEXT("Half-edge fixture starts with one complete edge"), FixtureEdge);
	if (!FixtureEdge)
	{
		return false;
	}

	int32 TargetEdgeIndex = INDEX_NONE;
	for (int32 Index = 0; Index < TargetPin->Edges.Num(); ++Index)
	{
		if (TargetPin->Edges[Index].Get() == FixtureEdge)
		{
			TargetEdgeIndex = Index;
			break;
		}
	}
	TestTrue(TEXT("Complete fixture edge is attached to the target pin"), TargetEdgeIndex != INDEX_NONE);
	if (TargetEdgeIndex == INDEX_NONE)
	{
		return false;
	}

	TargetPin->Edges.RemoveAt(TargetEdgeIndex, 1, EAllowShrinking::No);
	TSharedPtr<FJsonObject> ValidateParams = MakeAssetParams(Fixture);
	const FMonolithActionResult CorruptValidation =
		ExecuteAction(TEXT("validate_pcg_graph"), ValidateParams);
	TargetPin->Edges.Insert(FixtureEdge, TargetEdgeIndex);

	if (!RequireSuccess(*this, TEXT("validate half-attached edge fixture"), CorruptValidation))
	{
		return false;
	}
	bool bValid = true;
	double ErrorCount = 0.0;
	double InvalidEdgeCount = 0.0;
	TestTrue(TEXT("Structural validator rejects an edge attached only to its source pin"),
		CorruptValidation.Result->TryGetBoolField(TEXT("valid"), bValid) && !bValid);
	TestTrue(TEXT("Half-attached edge contributes a structural validation error"),
		CorruptValidation.Result->TryGetNumberField(TEXT("error_count"), ErrorCount) && ErrorCount >= 1.0);
	TestTrue(TEXT("Half-attached edge contributes to invalid_edge_count"),
		CorruptValidation.Result->TryGetNumberField(TEXT("invalid_edge_count"), InvalidEdgeCount) &&
			InvalidEdgeCount >= 1.0);
	TestTrue(TEXT("Fixture restoration reattaches the exact edge to both endpoint pins"),
		SourcePin->Edges.Contains(FixtureEdge) && TargetPin->Edges.Contains(FixtureEdge));

	const FMonolithActionResult RestoredValidation =
		ExecuteAction(TEXT("validate_pcg_graph"), ValidateParams);
	if (!RequireSuccess(*this, TEXT("validate restored complete edge fixture"), RestoredValidation))
	{
		return false;
	}
	bValid = false;
	TestTrue(TEXT("Restored fixture passes structural validation"),
		RestoredValidation.Result->TryGetBoolField(TEXT("valid"), bValid) && bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphDisconnectRemovePersistenceTest,
	"Monolith.PCG.GraphAuthoring.Asset.DisconnectRemovePersistenceAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphDisconnectRemovePersistenceTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_DisconnectRemovePersistence"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}
	const FMonolithActionResult AddA = AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("Persist_A"));
	const FMonolithActionResult AddB = AddNode(Fixture, TEXT("PCGAddTagSettings"), TEXT("Persist_B"));
	if (!RequireSuccess(*this, TEXT("add Persist_A"), AddA) ||
		!RequireSuccess(*this, TEXT("add Persist_B"), AddB))
	{
		return false;
	}
	FString NodeAId;
	FString NodeBId;
	if (!ReadRequiredString(*this, TEXT("add Persist_A"), AddA, TEXT("node_id"), NodeAId) ||
		!ReadRequiredString(*this, TEXT("add Persist_B"), AddB, TEXT("node_id"), NodeBId))
	{
		return false;
	}

	if (!RequireSuccess(
			*this,
			TEXT("connect Persist_A to Persist_B"),
			ConnectNodes(Fixture, NodeAId, TEXT("Out"), NodeBId, TEXT("In"))))
	{
		return false;
	}
	const FMonolithActionResult DisconnectResult =
		DisconnectNodes(Fixture, NodeAId, TEXT("Out"), NodeBId, TEXT("In"));
	if (!RequireSuccess(*this, TEXT("disconnect Persist_A from Persist_B"), DisconnectResult))
	{
		return false;
	}
	bool bRemoved = false;
	TestTrue(
		TEXT("Disconnect reports that it removed the edge"),
		DisconnectResult.Result->TryGetBoolField(TEXT("removed"), bRemoved) && bRemoved);

	UPCGGraph* ReloadedGraph = ReloadGraph(*this, Fixture);
	if (!ReloadedGraph)
	{
		return false;
	}
	TestEqual(TEXT("Disconnected graph reloads with both nodes"), ReloadedGraph->GetNodes().Num(), 2);
	TestEqual(TEXT("Disconnected edge is absent after reload"), CountGraphEdges(ReloadedGraph), 0);

	TSharedPtr<FJsonObject> RemoveParams = MakeAssetParams(Fixture);
	RemoveParams->SetStringField(TEXT("node_id"), NodeBId);
	RemoveParams->SetBoolField(TEXT("save"), true);
	if (!RequireSuccess(
			*this,
			TEXT("remove Persist_B"),
			ExecuteAction(TEXT("remove_pcg_node"), RemoveParams)))
	{
		return false;
	}

	ReloadedGraph = ReloadGraph(*this, Fixture);
	if (!ReloadedGraph)
	{
		return false;
	}
	TestEqual(TEXT("Node removal persists after reload"), ReloadedGraph->GetNodes().Num(), 1);
	TestNotNull(TEXT("Unremoved node remains after reload"), FindNodeByTitle(ReloadedGraph, TEXT("Persist_A")));
	TestNull(TEXT("Removed node is absent after reload"), FindNodeByTitle(ReloadedGraph, TEXT("Persist_B")));
	TestEqual(TEXT("Removed node leaves no residual edge"), CountGraphEdges(ReloadedGraph), 0);

	Fixture.Cleanup();
	TestFalse(TEXT("Fixture cleanup removes the saved package file"), Fixture.ExistsOnDisk());
	TestFalse(TEXT("Fixture cleanup removes the asset-registry entry"), Fixture.ExistsInAssetRegistry());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphPreSaveValidationPolicyTest,
	"Monolith.PCG.GraphAuthoring.Guard.PreSaveValidationPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphPreSaveValidationPolicyTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	RegisterGraphAuthoringActions();

	const TArray<FString> GraphMutatingActions = {
		TEXT("create_pcg_graph"),
		TEXT("add_pcg_node"),
		TEXT("remove_pcg_node"),
		TEXT("connect_pcg_nodes"),
		TEXT("disconnect_pcg_nodes"),
		TEXT("set_pcg_node_params")};
	for (const FString& Action : GraphMutatingActions)
	{
		const FMonolithActionExecutionPolicy Policy =
			FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("pcg"), Action);
		TestFalse(
			*FString::Printf(
				TEXT("pcg.%s relies on authoritative in-handler pre-save validation, not late validation"),
				*Action),
			Policy.bPostEditValidation);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGGraphAuthoringRegistrationTest, "Monolith.PCG.GraphAuthoring.Registration",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphAuthoringRegistrationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	FMonolithPCGGraphAuthoringActions::RegisterActions(Registry);

	const TArray<FString> ExpectedActions = {TEXT("get_status"),		  TEXT("list_graph_assets"),
											 TEXT("get_graph_asset"),	  TEXT("remap_graph_references"),
											 TEXT("list_components"),	  TEXT("list_pcg_node_types"),
											 TEXT("create_pcg_graph"),	  TEXT("get_pcg_graph_info"),
											 TEXT("add_pcg_node"),		  TEXT("remove_pcg_node"),
											 TEXT("connect_pcg_nodes"),	  TEXT("disconnect_pcg_nodes"),
											 TEXT("set_pcg_node_params"), TEXT("validate_pcg_graph")};

	for (const FString& Action : ExpectedActions)
	{
		TestTrue(*FString::Printf(TEXT("pcg.%s is registered"), *Action), Registry.HasAction(TEXT("pcg"), Action));
	}
	TestEqual(TEXT("PCG action count is synchronized"), Registry.GetNamespaceActionCount(TEXT("pcg")),
			  ExpectedActions.Num());

	const TArray<FString> MutatingActions = {TEXT("remap_graph_references"), TEXT("create_pcg_graph"),
											 TEXT("add_pcg_node"),			 TEXT("remove_pcg_node"),
											 TEXT("connect_pcg_nodes"),		 TEXT("disconnect_pcg_nodes"),
											 TEXT("set_pcg_node_params")};
	for (const FString& Action : MutatingActions)
	{
		const FMonolithActionExecutionPolicy Policy = Registry.GetActionExecutionPolicy(TEXT("pcg"), Action);
		TestEqual(*FString::Printf(TEXT("pcg.%s uses the guarded transaction policy"), *Action),
				  Policy.PolicyId, FString(TEXT("transaction_optional")));
		if (Action != TEXT("remap_graph_references"))
		{
			TestFalse(*FString::Printf(TEXT("pcg.%s validates before persistence instead of after it"), *Action),
					  Policy.bPostEditValidation);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGSettingsResolverTest, "Monolith.PCG.GraphAuthoring.SettingsResolver",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGSettingsResolverTest::RunTest(const FString& Parameters)
{
	const TArray<FMonolithPCGSettingsTypeInfo> Types = FMonolithPCGSettingsResolver::ListTypes();
	TestTrue(TEXT("At least one concrete UPCGSettings class is discoverable"), !Types.IsEmpty());

	if (!Types.IsEmpty())
	{
		const FMonolithPCGSettingsTypeInfo& First = Types[0];
		TArray<FString> Candidates;
		FString Error;
		UClass* ResolvedByName = FMonolithPCGSettingsResolver::Resolve(First.ClassName, Candidates, Error);
		TestEqual(TEXT("Exact reflected class name resolves deterministically"), ResolvedByName, First.SettingsClass);

		Candidates.Reset();
		Error.Reset();
		UClass* ResolvedByPath = FMonolithPCGSettingsResolver::Resolve(First.ClassPath, Candidates, Error);
		TestEqual(TEXT("Exact reflected class path resolves deterministically"), ResolvedByPath, First.SettingsClass);
	}

	TArray<FString> Candidates;
	FString Error;
	TestNull(TEXT("Unknown settings type is rejected"),
			 FMonolithPCGSettingsResolver::Resolve(TEXT("DefinitelyNotAPCGSettingsType"), Candidates, Error));
	TestTrue(TEXT("Unknown type error is actionable"), Error.Contains(TEXT("not found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGGraphAuthoringParamGuardTest, "Monolith.PCG.GraphAuthoring.ParamGuard",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphAuthoringParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGGraphAuthoringActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Benchmarks/AssetAuthoring/PCG/PCG_InvalidPolicy"));
	Params->SetStringField(TEXT("existing_policy"), TEXT("overwrite"));
	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("create_pcg_graph"), Params);
	TestFalse(TEXT("Unknown existing_policy is rejected before mutation"), Result.bSuccess);
	TestTrue(TEXT("Policy error lists accepted behavior"), Result.ErrorMessage.Contains(TEXT("return_existing")));

	return true;
}
