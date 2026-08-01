#include "CoreMinimal.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"

#include "MonolithPCGActions.h"
#include "MonolithPCGComponentActions.h"
#include "MonolithPCGGraphAuthoringActions.h"
#include "MonolithPCGResultUtils.h"
#include "MonolithPCGSettingsResolver.h"
#include "MonolithObjectTraversal.h"
#include "MonolithToolRegistry.h"

#include "Elements/PCGAddTag.h"
#include "Elements/PCGSpawnActor.h"
#include "Engine/StaticMeshActor.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGInputOutputSettings.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

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

	// ObjectTools owns the in-memory deletion lifecycle for a loaded fixture.
	// Do not manually clear RF_Standalone, mark packages as garbage, or force a
	// global collection here: a fixture cleanup must not collect unrelated
	// editor worlds that another automation case has just unloaded.  The disk
	// and AssetRegistry cleanup below also handles a disk-only stale fixture.

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

bool SaveFixtureGraph(FAutomationTestBase& Test, UPCGGraph* Graph, const FString& Context)
{
	UEditorAssetSubsystem* AssetSubsystem =
		GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!AssetSubsystem || !Graph)
	{
		Test.AddError(FString::Printf(TEXT("%s has no graph or EditorAssetSubsystem"), *Context));
		return false;
	}
	Graph->MarkPackageDirty();
	if (!AssetSubsystem->SaveLoadedAsset(Graph, false))
	{
		Test.AddError(FString::Printf(TEXT("%s could not save the fixture graph"), *Context));
		return false;
	}
	if (Graph->GetPackage()->IsDirty())
	{
		Test.AddError(FString::Printf(TEXT("%s left the fixture package dirty"), *Context));
		return false;
	}
	return true;
}

FProperty* RequireGraphProperty(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const FName PropertyName)
{
	FProperty* Property = Graph ? FindFProperty<FProperty>(Graph->GetClass(), PropertyName) : nullptr;
	if (!Property)
	{
		Test.AddError(FString::Printf(
			TEXT("UPCGGraph is missing required reflected property '%s'"), *PropertyName.ToString()));
	}
	return Property;
}

bool SetGraphBoolProperty(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const FName PropertyName,
	const bool bValue)
{
	FBoolProperty* Property = CastField<FBoolProperty>(RequireGraphProperty(Test, Graph, PropertyName));
	if (!Property)
	{
		Test.AddError(FString::Printf(
			TEXT("UPCGGraph property '%s' is not a bool"), *PropertyName.ToString()));
		return false;
	}
	Property->SetPropertyValue_InContainer(Graph, bValue);
	return true;
}

bool SetGraphEnumProperty(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const FName PropertyName,
	const UEnum* ExpectedEnum,
	const int64 Value)
{
	FEnumProperty* Property = CastField<FEnumProperty>(RequireGraphProperty(Test, Graph, PropertyName));
	if (!Property || Property->GetEnum() != ExpectedEnum || !Property->GetUnderlyingProperty())
	{
		Test.AddError(FString::Printf(
			TEXT("UPCGGraph property '%s' does not have the expected enum type"), *PropertyName.ToString()));
		return false;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Graph);
	Property->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Value);
	return true;
}

bool SetGraphFloatingPointProperty(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const FName PropertyName,
	const double Value)
{
	FNumericProperty* Property = CastField<FNumericProperty>(RequireGraphProperty(Test, Graph, PropertyName));
	if (!Property || !Property->IsFloatingPoint())
	{
		Test.AddError(FString::Printf(
			TEXT("UPCGGraph property '%s' is not floating point"), *PropertyName.ToString()));
		return false;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Graph);
	Property->SetFloatingPointPropertyValue(ValuePtr, Value);
	return true;
}

bool SetGraphIntegerProperty(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const FName PropertyName,
	const uint64 Value)
{
	FNumericProperty* Property = CastField<FNumericProperty>(RequireGraphProperty(Test, Graph, PropertyName));
	if (!Property || !Property->IsInteger() || !Property->CanHoldValue(Value))
	{
		Test.AddError(FString::Printf(
			TEXT("UPCGGraph property '%s' cannot represent the requested integer"), *PropertyName.ToString()));
		return false;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Graph);
	Property->SetIntPropertyValue(ValuePtr, Value);
	return true;
}

bool SetGraphGridSizeMultiplier(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const double GridSizeMultiplier)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	return SetGraphFloatingPointProperty(
		Test, Graph, TEXT("HiGenGridSizeMultiplier"), GridSizeMultiplier);
#else
	if (!FMath::IsFinite(GridSizeMultiplier) || GridSizeMultiplier <= 0.0)
	{
		Test.AddError(TEXT("UE 5.7 hierarchical grid multiplier must be positive and finite"));
		return false;
	}
	const double GridExponent = FMath::Log2(GridSizeMultiplier);
	const int32 RoundedExponent = FMath::RoundToInt(GridExponent);
	if (RoundedExponent < 0 || RoundedExponent > 10 ||
		!FMath::IsNearlyEqual(GridExponent, static_cast<double>(RoundedExponent)))
	{
		Test.AddError(TEXT("UE 5.7 hierarchical grid multiplier must be an exact power of two from 1 to 1024"));
		return false;
	}
	return SetGraphIntegerProperty(
		Test, Graph, TEXT("HiGenExponential"), static_cast<uint64>(RoundedExponent));
#endif
}

double GetGraphGridSizeMultiplier(const UPCGGraph* Graph)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	return Graph ? Graph->GetGridSizeMultiplier() : 0.0;
#else
	return Graph ? FMath::Pow(2.0, static_cast<double>(Graph->GetGridExponential())) : 0.0;
#endif
}

bool ConfigureGraphHierarchicalGenerationPolicy(
	FAutomationTestBase& Test,
	UPCGGraph* Graph,
	const EPCGHiGenGrid Grid,
	const double GridSizeMultiplier,
	const bool bUse2DGrid)
{
	if (!Graph)
	{
		Test.AddError(TEXT("Cannot configure hierarchical generation on a null UPCGGraph"));
		return false;
	}

	Graph->Modify();
	if (!SetGraphBoolProperty(Test, Graph, TEXT("bUseHierarchicalGeneration"), true) ||
		!SetGraphEnumProperty(
			Test, Graph, TEXT("HiGenGridSize"), StaticEnum<EPCGHiGenGrid>(), static_cast<int64>(Grid)) ||
		!SetGraphGridSizeMultiplier(Test, Graph, GridSizeMultiplier) ||
		!SetGraphBoolProperty(Test, Graph, TEXT("bUse2DGrid"), bUse2DGrid))
	{
		return false;
	}

	if (!Graph->IsHierarchicalGenerationEnabled() || Graph->GetDefaultGrid() != Grid ||
		!FMath::IsNearlyEqual(GetGraphGridSizeMultiplier(Graph), GridSizeMultiplier) ||
		Graph->Use2DGrid() != bUse2DGrid)
	{
		Test.AddError(TEXT("UPCGGraph public hierarchical-generation accessors did not read back the reflected policy"));
		return false;
	}
	return true;
}

bool ConfigureReplacementSourceGraph(FAutomationTestBase& Test, UPCGGraph* Graph)
{
	if (!Graph)
	{
		Test.AddError(TEXT("Replacement source graph is null"));
		return false;
	}
	Graph->Modify();
	Graph->bLandscapeUsesMetadata = false;
	if (!ConfigureGraphHierarchicalGenerationPolicy(
			Test, Graph, EPCGHiGenGrid::Grid64, 2.0, /*bUse2DGrid=*/false))
	{
		return false;
	}
	if (Graph->GetNodes().Num() != 1 || !Graph->GetNodes()[0])
	{
		Test.AddError(TEXT("Replacement source must have exactly one element node"));
		return false;
	}
	UPCGNode* ElementNode = Graph->GetNodes()[0];
	ElementNode->Modify();
	ElementNode->SetNodePosition(321, -654);
#if WITH_EDITORONLY_DATA
	ElementNode->NodeTitleColor = FLinearColor(0.125f, 0.25f, 0.75f, 1.0f);
	ElementNode->NodeComment = TEXT("Replacement editor state");
#endif
	UPCGAddTagSettings* AddTagSettings = Cast<UPCGAddTagSettings>(ElementNode->GetSettings());
	if (!AddTagSettings)
	{
		Test.AddError(TEXT("Replacement source element is not UPCGAddTagSettings"));
		return false;
	}
	AddTagSettings->Modify();
	AddTagSettings->TagsToAdd = TEXT("ReplacementTag:Exact");
	AddTagSettings->Prefix = TEXT("Copied_");
	AddTagSettings->Suffix = TEXT("_State");
	// Keep one reusable inner-object field exactly at its archetype default. The
	// replacement test gives the corresponding target settings object the
	// opposite stale value, proving seeded replay resets omitted default deltas.
	AddTagSettings->bIgnoreTagValueParsing =
		GetDefault<UPCGAddTagSettings>()->bIgnoreTagValueParsing;

	const FName DensityName(TEXT("ReplacementDensity"));
	TArray<FPropertyBagPropertyDesc> Descriptors;
	Descriptors.Emplace(DensityName, EPropertyBagPropertyType::Double);
	if (Graph->AddUserParameters(Descriptors) != EPropertyBagAlterationResult::Success)
	{
		Test.AddError(TEXT("Replacement source could not add its user-parameter schema"));
		return false;
	}
	FInstancedPropertyBag* Bag = Graph->GetMutableUserParametersStruct_Unsafe();
	if (!Bag || Bag->SetValueDouble(DensityName, 0.625) != EPropertyBagResult::Success)
	{
		Test.AddError(TEXT("Replacement source could not set its user-parameter value"));
		return false;
	}

	UPCGNode* InputNode = Graph->GetInputNode();
	UPCGNode* OutputNode = Graph->GetOutputNode();
	UPCGGraphInputOutputSettings* InputSettings =
		InputNode ? Cast<UPCGGraphInputOutputSettings>(InputNode->GetSettings()) : nullptr;
	UPCGGraphInputOutputSettings* OutputSettings =
		OutputNode ? Cast<UPCGGraphInputOutputSettings>(OutputNode->GetSettings()) : nullptr;
	const TArray<FPCGPinProperties> InputDefaults =
		InputSettings ? InputSettings->DefaultOutputPinProperties() : TArray<FPCGPinProperties>();
	const TArray<FPCGPinProperties> OutputDefaults =
		OutputSettings ? OutputSettings->DefaultInputPinProperties() : TArray<FPCGPinProperties>();
	if (!InputSettings || !OutputSettings || InputDefaults.IsEmpty() || OutputDefaults.IsEmpty())
	{
		Test.AddError(TEXT("Replacement source has no editable input/output settings pins"));
		return false;
	}
	InputSettings->Modify();
	OutputSettings->Modify();
	FPCGPinProperties InputPin = InputDefaults[0];
	InputPin.Label = TEXT("ReplacementPayload");
	FPCGPinProperties OutputPin = OutputDefaults[0];
	OutputPin.Label = TEXT("ReplacementPayload");
	const FPCGPinProperties& AddedInputPin = InputSettings->AddPin(InputPin);
	const FPCGPinProperties& AddedOutputPin = OutputSettings->AddPin(OutputPin);
	if (AddedInputPin.Label != InputPin.Label || AddedOutputPin.Label != OutputPin.Label)
	{
		Test.AddError(TEXT("Replacement source custom input/output pin labels collided"));
		return false;
	}
	InputNode->UpdateAfterSettingsChangeDuringCreation();
	OutputNode->UpdateAfterSettingsChangeDuringCreation();
	Graph->AddLabeledEdge(
		InputNode, TEXT("ReplacementPayload"), OutputNode, TEXT("ReplacementPayload"));
	if (!HasConnection(
			InputNode, TEXT("ReplacementPayload"), OutputNode, TEXT("ReplacementPayload")))
	{
		Test.AddError(TEXT("Replacement source could not author its custom input/output edge"));
		return false;
	}
	return SaveFixtureGraph(Test, Graph, TEXT("Configure replacement source"));
}

TSharedPtr<FJsonObject> MakeReplacementParams(
	const FScopedPCGGraphFixture& Source,
	const FScopedPCGGraphFixture& Target)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("source_asset_path"), Source.ObjectPath);
	Params->SetStringField(TEXT("target_asset_path"), Target.ObjectPath);
	return Params;
}

bool RequireReplacementSourceControlStatus(
	FAutomationTestBase& Test,
	const FString& Context,
	const TSharedPtr<FJsonObject>& Result,
	const FString& ExpectedStatus)
{
	const TSharedPtr<FJsonObject>* Prepare = nullptr;
	if (!Result.IsValid() ||
		!Result->TryGetObjectField(TEXT("source_control_prepare"), Prepare) ||
		!Prepare || !Prepare->IsValid())
	{
		Test.AddError(FString::Printf(
			TEXT("%s returned no source_control_prepare object"), *Context));
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s uses handler-owned source control"), *Context),
		(*Prepare)->GetStringField(TEXT("mode")),
		FString(TEXT("handler_owned_pre_mutation")));
	Test.TestEqual(
		*FString::Printf(TEXT("%s has the expected source-control status"), *Context),
		(*Prepare)->GetStringField(TEXT("status")), ExpectedStatus);
	return true;
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
	FMonolithPCGGraphSubgraphAssignmentSaveReloadTest,
	"Monolith.PCG.GraphAuthoring.Subgraph.AssignDryRunSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphSubgraphAssignmentSaveReloadTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	// Destruction is reverse declaration order, so the parent is removed before
	// the child package it references.
	FScopedPCGGraphFixture ChildFixture(TEXT("PCG_SubgraphAssignment_Child"));
	FScopedPCGGraphFixture ParentFixture(TEXT("PCG_SubgraphAssignment_Parent"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create child graph"), CreateGraph(ChildFixture)) ||
		!RequireSuccess(*this, TEXT("create parent graph"), CreateGraph(ParentFixture)))
	{
		return false;
	}

	const FMonolithActionResult AddResult = AddNode(
		ParentFixture, TEXT("PCGSubgraphSettings"), TEXT("Authored_Subgraph"));
	if (!RequireSuccess(*this, TEXT("add subgraph node"), AddResult))
	{
		return false;
	}
	FString NodeId;
	if (!ReadRequiredString(*this, TEXT("add subgraph node"), AddResult, TEXT("node_id"), NodeId))
	{
		return false;
	}

	UPCGGraph* ParentGraph = LoadGraph(ParentFixture);
	UPCGNode* SubgraphNode = FindNodeByTitle(ParentGraph, TEXT("Authored_Subgraph"));
	UPCGSubgraphSettings* SubgraphSettings =
		SubgraphNode ? Cast<UPCGSubgraphSettings>(SubgraphNode->GetSettings()) : nullptr;
	TestNotNull(TEXT("Subgraph settings fixture"), SubgraphSettings);
	if (!SubgraphSettings || !SubgraphSettings->SubgraphInstance)
	{
		return false;
	}
	TestNull(TEXT("Fresh subgraph node has no assigned graph interface"),
		SubgraphSettings->SubgraphInstance->Graph.Get());

	TSharedPtr<FJsonObject> CaseAliasParams = MakeAssetParams(ParentFixture);
	CaseAliasParams->SetStringField(TEXT("node_id"), NodeId);
	CaseAliasParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.ObjectPath.ToLower());
	CaseAliasParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult CaseAliasResult = ExecuteAction(TEXT("set_pcg_subgraph"), CaseAliasParams);
	TestFalse(TEXT("Case-only subgraph aliases are rejected"), CaseAliasResult.bSuccess);
	TestTrue(TEXT("Case-only rejection explains the exact canonical path contract"),
		CaseAliasResult.ErrorMessage.Contains(TEXT("exact canonical object path"), ESearchCase::CaseSensitive));
	TestNull(TEXT("Rejected case-only alias leaves the graph interface unassigned"),
		SubgraphSettings->SubgraphInstance->Graph.Get());

	FScopedPCGGraphFixture RedirectFixture(TEXT("PCG_SubgraphAssignment_Redirect"));
	UPackage* RedirectPackage = CreatePackage(*RedirectFixture.PackageName);
	UObjectRedirector* Redirector = RedirectPackage
		? NewObject<UObjectRedirector>(
			RedirectPackage,
			*FPackageName::GetLongPackageAssetName(RedirectFixture.PackageName),
			RF_Public | RF_Standalone)
		: nullptr;
	TestNotNull(TEXT("Redirector alias fixture"), Redirector);
	if (!Redirector)
	{
		return false;
	}
	Redirector->DestinationObject = LoadGraph(ChildFixture);
	TSharedPtr<FJsonObject> RedirectAliasParams = MakeAssetParams(ParentFixture);
	RedirectAliasParams->SetStringField(TEXT("node_id"), NodeId);
	RedirectAliasParams->SetStringField(TEXT("subgraph_asset_path"), RedirectFixture.ObjectPath);
	RedirectAliasParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult RedirectAliasResult =
		ExecuteAction(TEXT("set_pcg_subgraph"), RedirectAliasParams);
	TestFalse(TEXT("Redirector subgraph aliases are rejected"), RedirectAliasResult.bSuccess);
	TestTrue(TEXT("Redirector rejection explains the exact canonical path contract"),
		RedirectAliasResult.ErrorMessage.Contains(TEXT("exact canonical object path"), ESearchCase::CaseSensitive));
	TestNull(TEXT("Rejected redirector alias leaves the graph interface unassigned"),
		SubgraphSettings->SubgraphInstance->Graph.Get());

	TSharedPtr<FJsonObject> DryRunParams = MakeAssetParams(ParentFixture);
	DryRunParams->SetStringField(TEXT("node_id"), NodeId);
	DryRunParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.PackageName);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	DryRunParams->SetBoolField(TEXT("save"), true);
	const bool bParentDirtyBeforeDryRun = ParentGraph->GetPackage()->IsDirty();
	const FMonolithActionResult DryRunResult = ExecuteAction(TEXT("set_pcg_subgraph"), DryRunParams);
	if (!RequireSuccess(*this, TEXT("set_pcg_subgraph dry-run"), DryRunResult))
	{
		return false;
	}
	FString DryRunStatus;
	TestTrue(TEXT("Dry-run reports would_update"),
		DryRunResult.Result->TryGetStringField(TEXT("status"), DryRunStatus) &&
		DryRunStatus == TEXT("would_update"));
	TestNull(TEXT("Dry-run leaves the graph interface unassigned"),
		SubgraphSettings->SubgraphInstance->Graph.Get());
	TestEqual(TEXT("Dry-run preserves the parent dirty state"),
		ParentGraph->GetPackage()->IsDirty(), bParentDirtyBeforeDryRun);

	TSharedPtr<FJsonObject> ApplyParams = MakeAssetParams(ParentFixture);
	ApplyParams->SetStringField(TEXT("node_id"), NodeId);
	ApplyParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.ObjectPath);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult ApplyResult = ExecuteAction(TEXT("set_pcg_subgraph"), ApplyParams);
	if (!RequireSuccess(*this, TEXT("set_pcg_subgraph apply"), ApplyResult))
	{
		return false;
	}
	bool bSaved = false;
	TestTrue(TEXT("Subgraph assignment reports a persisted parent graph"),
		ApplyResult.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
	FString AssignedPath;
	TestTrue(TEXT("Subgraph assignment returns the exact assigned interface path"),
		ApplyResult.Result->TryGetStringField(TEXT("assigned_subgraph_path"), AssignedPath));
	TestEqual(TEXT("Assigned interface path is canonical"), AssignedPath, ChildFixture.ObjectPath);

	ParentGraph = ReloadGraph(*this, ParentFixture);
	if (!ParentGraph)
	{
		return false;
	}
	SubgraphNode = FindNodeByTitle(ParentGraph, TEXT("Authored_Subgraph"));
	SubgraphSettings = SubgraphNode ? Cast<UPCGSubgraphSettings>(SubgraphNode->GetSettings()) : nullptr;
	TestNotNull(TEXT("Reload preserves subgraph settings"), SubgraphSettings);
	if (!SubgraphSettings || !SubgraphSettings->SubgraphInstance)
	{
		return false;
	}
	TestNotNull(TEXT("Reload preserves the assigned graph interface"),
		SubgraphSettings->SubgraphInstance->Graph.Get());
	if (SubgraphSettings->SubgraphInstance->Graph)
	{
		TestEqual(TEXT("Reload preserves the exact graph interface path"),
			SubgraphSettings->SubgraphInstance->Graph->GetPathName(), ChildFixture.ObjectPath);
	}
	TestEqual(TEXT("Reload resolves the exact concrete child graph"),
		SubgraphSettings->GetSubgraph(), LoadGraph(ChildFixture));
	TestFalse(TEXT("Reloaded parent package is clean"), ParentGraph->GetPackage()->IsDirty());

	TSharedPtr<FJsonObject> NoOpParams = MakeAssetParams(ParentFixture);
	NoOpParams->SetStringField(TEXT("node_id"), TEXT("Authored_Subgraph"));
	NoOpParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.PackageName);
	NoOpParams->SetBoolField(TEXT("dry_run"), false);
	NoOpParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult NoOpResult = ExecuteAction(TEXT("set_pcg_subgraph"), NoOpParams);
	if (!RequireSuccess(*this, TEXT("set_pcg_subgraph no-op"), NoOpResult))
	{
		return false;
	}
	FString NoOpStatus;
	bool bNoOpSaved = true;
	TestTrue(TEXT("Exact repeated assignment reports unchanged"),
		NoOpResult.Result->TryGetStringField(TEXT("status"), NoOpStatus) &&
		NoOpStatus == TEXT("unchanged"));
	TestTrue(TEXT("Exact repeated assignment does not save again"),
		NoOpResult.Result->TryGetBoolField(TEXT("saved"), bNoOpSaved) && !bNoOpSaved);
	FString NoOpAssignedPath;
	TestTrue(TEXT("Exact repeated assignment returns assigned interface read-back"),
		NoOpResult.Result->TryGetStringField(TEXT("assigned_subgraph_path"), NoOpAssignedPath));
	TestEqual(TEXT("No-op read-back preserves the exact assigned interface"),
		NoOpAssignedPath, ChildFixture.ObjectPath);
	TestFalse(TEXT("Exact repeated assignment keeps the parent clean"), ParentGraph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphSubgraphRecursionGuardTest,
	"Monolith.PCG.GraphAuthoring.Subgraph.RecursionGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphSubgraphRecursionGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture ChildFixture(TEXT("PCG_SubgraphRecursion_Child"));
	FScopedPCGGraphFixture ParentFixture(TEXT("PCG_SubgraphRecursion_Parent"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create recursion child"), CreateGraph(ChildFixture)) ||
		!RequireSuccess(*this, TEXT("create recursion parent"), CreateGraph(ParentFixture)) ||
		!RequireSuccess(*this, TEXT("add parent subgraph node"),
			AddNode(ParentFixture, TEXT("PCGSubgraphSettings"), TEXT("Parent_To_Child"))) ||
		!RequireSuccess(*this, TEXT("add child subgraph node"),
			AddNode(ChildFixture, TEXT("PCGSubgraphSettings"), TEXT("Child_To_Parent"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ParentAssignParams = MakeAssetParams(ParentFixture);
	ParentAssignParams->SetStringField(TEXT("node_id"), TEXT("Parent_To_Child"));
	ParentAssignParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.PackageName);
	ParentAssignParams->SetBoolField(TEXT("dry_run"), false);
	ParentAssignParams->SetBoolField(TEXT("save"), false);
	if (!RequireSuccess(*this, TEXT("assign parent to child"),
			ExecuteAction(TEXT("set_pcg_subgraph"), ParentAssignParams)))
	{
		return false;
	}

	UPCGGraph* ChildGraph = LoadGraph(ChildFixture);
	UPCGNode* ChildNode = FindNodeByTitle(ChildGraph, TEXT("Child_To_Parent"));
	UPCGSubgraphSettings* ChildSettings =
		ChildNode ? Cast<UPCGSubgraphSettings>(ChildNode->GetSettings()) : nullptr;
	TestNotNull(TEXT("Child recursion settings fixture"), ChildSettings);
	if (!ChildSettings || !ChildSettings->SubgraphInstance)
	{
		return false;
	}
	TestNull(TEXT("Child recursion node starts unassigned"), ChildSettings->SubgraphInstance->Graph.Get());
	const bool bChildDirtyBeforeReject = ChildGraph->GetPackage()->IsDirty();

	TSharedPtr<FJsonObject> RecursiveParams = MakeAssetParams(ChildFixture);
	RecursiveParams->SetStringField(TEXT("node_id"), TEXT("Child_To_Parent"));
	RecursiveParams->SetStringField(TEXT("subgraph_asset_path"), ParentFixture.PackageName);
	RecursiveParams->SetBoolField(TEXT("dry_run"), true);
	RecursiveParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult RecursiveResult = ExecuteAction(TEXT("set_pcg_subgraph"), RecursiveParams);
	TestFalse(TEXT("Recursive child-to-parent assignment is rejected"), RecursiveResult.bSuccess);
	TestTrue(TEXT("Recursion rejection explains the graph hierarchy"),
		RecursiveResult.ErrorMessage.Contains(TEXT("recursive"), ESearchCase::IgnoreCase));
	TestNull(TEXT("Rejected recursion leaves the child node unassigned"),
		ChildSettings->SubgraphInstance->Graph.Get());
	TestEqual(TEXT("Rejected recursion preserves the child dirty state"),
		ChildGraph->GetPackage()->IsDirty(), bChildDirtyBeforeReject);

	TSharedPtr<FJsonObject> SelfParams = MakeAssetParams(ChildFixture);
	SelfParams->SetStringField(TEXT("node_id"), TEXT("Child_To_Parent"));
	SelfParams->SetStringField(TEXT("subgraph_asset_path"), ChildFixture.PackageName);
	SelfParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult SelfResult = ExecuteAction(TEXT("set_pcg_subgraph"), SelfParams);
	TestFalse(TEXT("Direct self-subgraph assignment is rejected"), SelfResult.bSuccess);
	TestNull(TEXT("Rejected self assignment remains side-effect free"),
		ChildSettings->SubgraphInstance->Graph.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphClassPropertyMetaClassGuardTest,
	"Monolith.PCG.GraphAuthoring.Settings.ClassPropertyMetaClassGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphClassPropertyMetaClassGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_ClassPropertyMetaClassGuard"));
	RegisterGraphAuthoringActions();

	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}

	const FMonolithActionResult AddResult = AddNode(
		Fixture, TEXT("PCGSpawnActorSettings"), TEXT("SpawnActorClassGuard"));
	if (!RequireSuccess(*this, TEXT("add Spawn Actor"), AddResult))
	{
		return false;
	}
	FString NodeId;
	if (!ReadRequiredString(*this, TEXT("add Spawn Actor"), AddResult, TEXT("node_id"), NodeId))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
	InvalidProperties->SetStringField(TEXT("TemplateActorClass"), TEXT("/Script/Engine.Texture2D"));
	TSharedPtr<FJsonObject> InvalidParams = MakeAssetParams(Fixture);
	InvalidParams->SetStringField(TEXT("node_id"), NodeId);
	InvalidParams->SetObjectField(TEXT("properties"), InvalidProperties);
	InvalidParams->SetBoolField(TEXT("dry_run"), true);
	InvalidParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult InvalidResult = ExecuteAction(TEXT("set_pcg_node_params"), InvalidParams);
	TestFalse(TEXT("non-Actor class is rejected by dry-run"), InvalidResult.bSuccess);
	TestTrue(
		TEXT("class rejection names the exact property and invalid class"),
		InvalidResult.ErrorMessage.Contains(TEXT("TemplateActorClass")) &&
			InvalidResult.ErrorMessage.Contains(TEXT("Texture2D")));

	UPCGGraph* Graph = LoadGraph(Fixture);
	UPCGNode* SpawnNode = FindNodeByTitle(Graph, TEXT("SpawnActorClassGuard"));
	UPCGSpawnActorSettings* SpawnSettings = SpawnNode ? Cast<UPCGSpawnActorSettings>(SpawnNode->GetSettings()) : nullptr;
	TestNotNull(TEXT("Spawn Actor settings fixture"), SpawnSettings);
	if (!SpawnSettings)
	{
		return false;
	}
	TestNull(TEXT("rejected class never mutates TemplateActorClass"), SpawnSettings->GetTemplateActorClass().Get());

	TSharedPtr<FJsonObject> ValidProperties = MakeShared<FJsonObject>();
	ValidProperties->SetStringField(TEXT("TemplateActorClass"), TEXT("/Script/Engine.StaticMeshActor"));
	TSharedPtr<FJsonObject> ValidDryRunParams = MakeAssetParams(Fixture);
	ValidDryRunParams->SetStringField(TEXT("node_id"), NodeId);
	ValidDryRunParams->SetObjectField(TEXT("properties"), ValidProperties);
	ValidDryRunParams->SetBoolField(TEXT("dry_run"), true);
	ValidDryRunParams->SetBoolField(TEXT("save"), false);
	if (!RequireSuccess(
			*this,
			TEXT("valid Actor class dry-run"),
			ExecuteAction(TEXT("set_pcg_node_params"), ValidDryRunParams)))
	{
		return false;
	}
	TestNull(TEXT("dry-run leaves TemplateActorClass unchanged"), SpawnSettings->GetTemplateActorClass().Get());

	TSharedPtr<FJsonObject> ApplyParams = MakeAssetParams(Fixture);
	ApplyParams->SetStringField(TEXT("node_id"), NodeId);
	ApplyParams->SetObjectField(TEXT("properties"), ValidProperties);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("save"), false);
	if (!RequireSuccess(
			*this,
			TEXT("valid Actor class apply"),
			ExecuteAction(TEXT("set_pcg_node_params"), ApplyParams)))
	{
		return false;
	}

	Graph = LoadGraph(Fixture);
	SpawnNode = FindNodeByTitle(Graph, TEXT("SpawnActorClassGuard"));
	SpawnSettings = SpawnNode ? Cast<UPCGSpawnActorSettings>(SpawnNode->GetSettings()) : nullptr;
	TestNotNull(TEXT("Spawn Actor settings survive apply"), SpawnSettings);
	if (SpawnSettings)
	{
		TestEqual(
			TEXT("valid class applies exactly"),
			SpawnSettings->GetTemplateActorClass().Get(),
			AStaticMeshActor::StaticClass());
	}
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
	FMonolithPCGGraphContentsReplacementSaveReloadTest,
	"Monolith.PCG.GraphAuthoring.GraphContents.ReplaceDryRunConfirmSaveReloadIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphContentsReplacementSaveReloadTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture TargetFixture(TEXT("PCG_ReplaceContents_Target"));
	FScopedPCGGraphFixture SourceFixture(TEXT("PCG_ReplaceContents_Source"));
	RegisterGraphAuthoringActions();
	if (!RequireSuccess(*this, TEXT("create replacement source"), CreateGraph(SourceFixture)) ||
		!RequireSuccess(*this, TEXT("create replacement target"), CreateGraph(TargetFixture)))
	{
		return false;
	}

	const FMonolithActionResult SourceNodeResult = AddNode(
		SourceFixture, TEXT("PCGAddTagSettings"), TEXT("SourceReplacementNode"));
	const FMonolithActionResult TargetNodeResult = AddNode(
		TargetFixture, TEXT("PCGAddTagSettings"), TEXT("TargetOnlyNode"));
	if (!RequireSuccess(*this, TEXT("add source replacement node"), SourceNodeResult) ||
		!RequireSuccess(*this, TEXT("add target-only node"), TargetNodeResult))
	{
		return false;
	}
	FString SourceNodeId;
	if (!ReadRequiredString(
			*this, TEXT("source node"), SourceNodeResult, TEXT("node_id"), SourceNodeId) ||
		!RequireSuccess(*this, TEXT("connect source input"),
			ConnectNodes(SourceFixture, TEXT("__input__"), TEXT("In"), SourceNodeId, TEXT("In"))) ||
		!RequireSuccess(*this, TEXT("connect source output"),
			ConnectNodes(SourceFixture, SourceNodeId, TEXT("Out"), TEXT("__output__"), TEXT("Out"))))
	{
		return false;
	}
	UPCGGraph* SourceGraph = LoadGraph(SourceFixture);
	UPCGGraph* TargetGraph = LoadGraph(TargetFixture);
	if (!ConfigureReplacementSourceGraph(*this, SourceGraph) || !TargetGraph)
	{
		return false;
	}
	UPCGNode* const TargetOnlyNode = FindNodeByTitle(TargetGraph, TEXT("TargetOnlyNode"));
	UPCGAddTagSettings* const TargetOnlySettings =
		TargetOnlyNode ? Cast<UPCGAddTagSettings>(TargetOnlyNode->GetSettings()) : nullptr;
	if (!TestNotNull(
			TEXT("Replacement target has reusable add-tag settings for the default-reset fixture"),
			TargetOnlySettings))
	{
		return false;
	}
	const bool DefaultIgnoreTagValueParsing =
		GetDefault<UPCGAddTagSettings>()->bIgnoreTagValueParsing;
	TargetOnlySettings->Modify();
	TargetOnlySettings->bIgnoreTagValueParsing = !DefaultIgnoreTagValueParsing;
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	SourceGraph->LastEditedDocuments = {
		FPCGGraphDocumentInfo(SourceGraph, FVector2f(11.0f, 22.0f), 0.75f)
	};
	const TArray<FPCGGraphDocumentInfo> TargetEditorWorkspaceBefore = {
		FPCGGraphDocumentInfo(TargetGraph, FVector2f(-33.0f, 44.0f), 1.25f)
	};
	TargetGraph->LastEditedDocuments = TargetEditorWorkspaceBefore;
#endif
	if (!SaveFixtureGraph(*this, SourceGraph, TEXT("Save donor editor workspace fixture")) ||
		!SaveFixtureGraph(
			*this, TargetGraph, TEXT("Save stale non-default replacement target setting")))
	{
		return false;
	}
	const FString TargetIdentity = TargetGraph->GetPathName();
	UPCGNode* const TargetInputNodeIdentity = TargetGraph->GetInputNode();
	UPCGNode* const TargetOutputNodeIdentity = TargetGraph->GetOutputNode();
	UPCGSettings* const TargetInputSettingsIdentity =
		TargetInputNodeIdentity ? TargetInputNodeIdentity->GetSettings() : nullptr;
	UPCGSettings* const TargetOutputSettingsIdentity =
		TargetOutputNodeIdentity ? TargetOutputNodeIdentity->GetSettings() : nullptr;
	TestFalse(TEXT("Replacement target starts clean"), TargetGraph->GetPackage()->IsDirty());

#if WITH_DEV_AUTOMATION_TESTS
	UE::MonolithPCG::Private::ConfigureGraphContentsReplacementTestFault(
		TargetFixture.ObjectPath,
		UE::MonolithPCG::Private::EPCGGraphContentsReplacementTestFault::None);
	ON_SCOPE_EXIT
	{
		UE::MonolithPCG::Private::ResetGraphContentsReplacementTestFault();
	};
#endif

	TSharedPtr<FJsonObject> CaseAliasParams = MakeReplacementParams(SourceFixture, TargetFixture);
	CaseAliasParams->SetStringField(
		TEXT("source_asset_path"), SourceFixture.ObjectPath.ToLower());
	const FMonolithActionResult CaseAliasResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), CaseAliasParams);
	TestFalse(TEXT("Replacement rejects case-only donor aliases"), CaseAliasResult.bSuccess);
	TestTrue(TEXT("Replacement case-only rejection explains the exact path contract"),
		CaseAliasResult.ErrorMessage.Contains(
			TEXT("exact canonical object path"), ESearchCase::CaseSensitive));
	TestNotNull(TEXT("Rejected case-only donor alias preserves target-only topology"),
		FindNodeByTitle(TargetGraph, TEXT("TargetOnlyNode")));

	// Omitting dry_run exercises the action's safety-default contract.
	const FMonolithActionResult DryRunResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	if (!RequireSuccess(*this, TEXT("replacement default dry-run"), DryRunResult))
	{
		return false;
	}
	FString DryRunStatus;
	TestTrue(TEXT("Default dry-run reports would_replace"),
		DryRunResult.Result->TryGetStringField(TEXT("status"), DryRunStatus) &&
		DryRunStatus == TEXT("would_replace"));
	if (!RequireReplacementSourceControlStatus(
			*this, TEXT("replacement default dry-run"), DryRunResult.Result,
			TEXT("skipped_by_dry_run")))
	{
		return false;
	}
	TestNotNull(TEXT("Dry-run preserves target-only topology"),
		FindNodeByTitle(TargetGraph, TEXT("TargetOnlyNode")));
	TestNull(TEXT("Dry-run does not introduce source topology"),
		FindNodeByTitle(TargetGraph, TEXT("SourceReplacementNode")));
	TestFalse(TEXT("Default dry-run preserves target dirty state"), TargetGraph->GetPackage()->IsDirty());
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TestTrue(TEXT("Dry-run preserves identity-bound target editor workspace state"),
		TargetGraph->LastEditedDocuments == TargetEditorWorkspaceBefore);
#endif

	TSharedPtr<FJsonObject> MissingConfirmParams = MakeReplacementParams(SourceFixture, TargetFixture);
	MissingConfirmParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult MissingConfirmResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MissingConfirmParams);
	TestFalse(TEXT("Commit without confirm is rejected"), MissingConfirmResult.bSuccess);
	TestTrue(TEXT("Confirm rejection is explicit"),
		MissingConfirmResult.ErrorMessage.Contains(TEXT("confirm=true"), ESearchCase::CaseSensitive));
	TestNotNull(TEXT("Rejected commit preserves target-only topology"),
		FindNodeByTitle(TargetGraph, TEXT("TargetOnlyNode")));

	TSharedPtr<FJsonObject> ApplyParams = MakeReplacementParams(SourceFixture, TargetFixture);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult ApplyResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), ApplyParams);
	if (!RequireSuccess(*this, TEXT("replacement commit"), ApplyResult))
	{
		return false;
	}
	FString ApplyStatus;
	bool bSaved = false;
	bool bIdentityPreserved = false;
	TestTrue(TEXT("Replacement reports replaced"),
		ApplyResult.Result->TryGetStringField(TEXT("status"), ApplyStatus) &&
		ApplyStatus == TEXT("replaced"));
	TestTrue(TEXT("Replacement reports persisted target"),
		ApplyResult.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
	TestTrue(TEXT("Replacement reports target identity preservation"),
		ApplyResult.Result->TryGetBoolField(TEXT("target_identity_preserved"), bIdentityPreserved) &&
		bIdentityPreserved);
	if (!RequireReplacementSourceControlStatus(
			*this, TEXT("replacement commit"), ApplyResult.Result, TEXT("prepared")))
	{
		return false;
	}
	bool bPersistentPropertiesVerified = false;
	TestTrue(TEXT("Replacement reports deep persistent-property verification"),
		ApplyResult.Result->TryGetBoolField(
			TEXT("persistent_properties_verified"), bPersistentPropertiesVerified) &&
		bPersistentPropertiesVerified);
	TestEqual(TEXT("Target UObject identity pointer is preserved"), LoadGraph(TargetFixture), TargetGraph);
	TestEqual(TEXT("Target object path is preserved"), TargetGraph->GetPathName(), TargetIdentity);
	TestEqual(TEXT("Target default input node identity is preserved"),
		TargetGraph->GetInputNode(), TargetInputNodeIdentity);
	TestEqual(TEXT("Target default output node identity is preserved"),
		TargetGraph->GetOutputNode(), TargetOutputNodeIdentity);
	TestEqual(TEXT("Target default input settings identity is preserved"),
		TargetGraph->GetInputNode()->GetSettings(), TargetInputSettingsIdentity);
	TestEqual(TEXT("Target default output settings identity is preserved"),
		TargetGraph->GetOutputNode()->GetSettings(), TargetOutputSettingsIdentity);
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TestTrue(TEXT("Commit preserves identity-bound target editor workspace state"),
		TargetGraph->LastEditedDocuments == TargetEditorWorkspaceBefore);
#endif
	TestNull(TEXT("Target-only node is replaced"), FindNodeByTitle(TargetGraph, TEXT("TargetOnlyNode")));
	UPCGNode* ReplacedNode = FindNodeByTitle(TargetGraph, TEXT("SourceReplacementNode"));
	TestNotNull(TEXT("Source element/settings node is present"), ReplacedNode);
	if (ReplacedNode)
	{
		int32 PositionX = 0;
		int32 PositionY = 0;
		ReplacedNode->GetNodePosition(PositionX, PositionY);
		TestEqual(TEXT("Element editor X position is copied"), PositionX, 321);
		TestEqual(TEXT("Element editor Y position is copied"), PositionY, -654);
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("Element editor title color is copied"), ReplacedNode->NodeTitleColor,
			FLinearColor(0.125f, 0.25f, 0.75f, 1.0f));
		TestEqual(TEXT("Element editor comment is copied"), ReplacedNode->NodeComment,
			FString(TEXT("Replacement editor state")));
#endif
		const UPCGAddTagSettings* ReplacedSettings =
			Cast<UPCGAddTagSettings>(ReplacedNode->GetSettings());
		TestNotNull(TEXT("Element settings concrete class is copied"), ReplacedSettings);
		if (ReplacedSettings)
		{
			TestEqual(TEXT("Element settings tag payload is copied"), ReplacedSettings->TagsToAdd,
				FString(TEXT("ReplacementTag:Exact")));
			TestEqual(TEXT("Element settings prefix is copied"), ReplacedSettings->Prefix,
				FString(TEXT("Copied_")));
			TestEqual(TEXT("Element settings suffix is copied"), ReplacedSettings->Suffix,
				FString(TEXT("_State")));
			TestEqual(
				TEXT("Archetype-default source setting replaces the stale reusable-target value"),
				ReplacedSettings->bIgnoreTagValueParsing,
				DefaultIgnoreTagValueParsing);
		}
	}
	TestEqual(TEXT("Exact source edge count is copied"), CountGraphEdges(TargetGraph), CountGraphEdges(SourceGraph));
	TestTrue(TEXT("Graph-level landscape metadata policy is copied"), !TargetGraph->bLandscapeUsesMetadata);
	TestTrue(TEXT("Graph-level hierarchical generation policy is copied"), TargetGraph->IsHierarchicalGenerationEnabled());
	TestEqual(TEXT("Graph-level grid enum is copied"), TargetGraph->GetDefaultGrid(), EPCGHiGenGrid::Grid64);
	TestEqual(TEXT("Graph-level grid multiplier is copied"), GetGraphGridSizeMultiplier(TargetGraph), 2.0);
	TestTrue(TEXT("Graph-level 2D-grid policy is copied"), !TargetGraph->Use2DGrid());
	TestNotNull(TEXT("Custom graph input pin is copied"),
		TargetGraph->GetInputNode()->GetOutputPin(TEXT("ReplacementPayload")));
	TestNotNull(TEXT("Custom graph output pin is copied"),
		TargetGraph->GetOutputNode()->GetInputPin(TEXT("ReplacementPayload")));
	TestTrue(TEXT("Custom input/output edge is copied"), HasConnection(
		TargetGraph->GetInputNode(), TEXT("ReplacementPayload"),
		TargetGraph->GetOutputNode(), TEXT("ReplacementPayload")));
	const FInstancedPropertyBag* TargetBag = TargetGraph->GetUserParametersStruct();
	TestNotNull(TEXT("User-parameter bag is copied"), TargetBag);
	if (TargetBag)
	{
		const TValueOrError<double, EPropertyBagResult> DensityResult =
			TargetBag->GetValueDouble(TEXT("ReplacementDensity"));
		TestTrue(TEXT("User-parameter value is copied"), DensityResult.IsValid());
		if (DensityResult.IsValid())
		{
			TestEqual(TEXT("Copied user-parameter value is exact"), DensityResult.GetValue(), 0.625);
		}
	}
	TestFalse(TEXT("Saved replacement target is clean"), TargetGraph->GetPackage()->IsDirty());

	TargetGraph = ReloadGraph(*this, TargetFixture);
	if (!TargetGraph)
	{
		return false;
	}
	TestEqual(TEXT("Reload preserves canonical target identity"), TargetGraph->GetPathName(), TargetIdentity);
	TestNotNull(TEXT("Reload preserves copied element topology"),
		FindNodeByTitle(TargetGraph, TEXT("SourceReplacementNode")));
	TestTrue(TEXT("Reload preserves custom input/output topology"), HasConnection(
		TargetGraph->GetInputNode(), TEXT("ReplacementPayload"),
		TargetGraph->GetOutputNode(), TEXT("ReplacementPayload")));
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TestTrue(TEXT("Reload preserves identity-bound target editor workspace state"),
		TargetGraph->LastEditedDocuments == TargetEditorWorkspaceBefore);
#endif
	TestFalse(TEXT("Reloaded replacement target is clean"), TargetGraph->GetPackage()->IsDirty());
	const FInstancedPropertyBag* const SourceParameters = SourceGraph->GetUserParametersStruct();
	const FInstancedPropertyBag* const ReloadedTargetParameters =
		TargetGraph->GetUserParametersStruct();
	TestTrue(
		TEXT("Equivalent persisted property bags may have distinct transient script-struct identities"),
		SourceParameters && ReloadedTargetParameters &&
			SourceParameters->GetPropertyBagStruct() != ReloadedTargetParameters->GetPropertyBagStruct());

	UPCGNode* const SourceComparisonNode =
		FindNodeByTitle(SourceGraph, TEXT("SourceReplacementNode"));
	UPCGNode* const TargetComparisonNode =
		FindNodeByTitle(TargetGraph, TEXT("SourceReplacementNode"));
	UPCGPin* const SourceComparisonPin =
		SourceComparisonNode ? SourceComparisonNode->GetInputPin(FName(TEXT("In"))) : nullptr;
	UPCGPin* const TargetComparisonPin =
		TargetComparisonNode ? TargetComparisonNode->GetInputPin(FName(TEXT("In"))) : nullptr;
	if (!TestNotNull(TEXT("Persistent-comparison source pin exists"), SourceComparisonPin) ||
		!TestNotNull(TEXT("Persistent-comparison target pin exists"), TargetComparisonPin))
	{
		return false;
	}

#if WITH_EDITORONLY_DATA
	// FPCGPinProperties stores editor editability caches as reflected Transient
	// members inside an otherwise persistent USTRUCT. A package-persistent
	// comparator must ignore these runtime-only bytes without skipping the
	// containing Properties field or any of its persistent members.
	TargetComparisonPin->Properties.bAllowEditMultipleData =
		!SourceComparisonPin->Properties.bAllowEditMultipleData;
	TargetComparisonPin->Properties.bAllowEditMultipleConnections =
		!SourceComparisonPin->Properties.bAllowEditMultipleConnections;
	const FMonolithActionResult TransientPinDifferenceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	if (!RequireSuccess(
			*this, TEXT("replacement ignores transient nested pin caches"), TransientPinDifferenceResult))
	{
		return false;
	}
	FString TransientPinDifferenceStatus;
	TestTrue(TEXT("Transient nested pin-cache differences remain an exact persistent no-op"),
		TransientPinDifferenceResult.Result->TryGetStringField(
			TEXT("status"), TransientPinDifferenceStatus) &&
		TransientPinDifferenceStatus == TEXT("unchanged"));
	if (!RequireReplacementSourceControlStatus(
			*this, TEXT("transient nested pin-cache comparison"),
			TransientPinDifferenceResult.Result, TEXT("skipped_by_dry_run")))
	{
		return false;
	}
	TestFalse(
		TEXT("Transient nested pin-cache comparison preserves clean package state"),
		TargetGraph->GetPackage()->IsDirty());
	TargetComparisonPin->Properties.bAllowEditMultipleData =
		SourceComparisonPin->Properties.bAllowEditMultipleData;
	TargetComparisonPin->Properties.bAllowEditMultipleConnections =
		SourceComparisonPin->Properties.bAllowEditMultipleConnections;
#endif

	const EPCGPinStatus SourcePinStatus = SourceComparisonPin->Properties.PinStatus;
	TargetComparisonPin->Properties.PinStatus =
		SourcePinStatus == EPCGPinStatus::Normal
			? EPCGPinStatus::Required
			: EPCGPinStatus::Normal;
	const FMonolithActionResult PersistentPinDifferenceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	if (!RequireSuccess(
			*this, TEXT("replacement detects persistent nested pin state"), PersistentPinDifferenceResult))
	{
		return false;
	}
	FString PersistentPinDifferenceStatus;
	FString PersistentPinDifferenceProperty;
	TestTrue(TEXT("Persistent nested pin-state difference requires replacement"),
		PersistentPinDifferenceResult.Result->TryGetStringField(
			TEXT("status"), PersistentPinDifferenceStatus) &&
		PersistentPinDifferenceStatus == TEXT("would_replace"));
	TestTrue(TEXT("Persistent nested pin-state difference is attributed to the complete pin struct"),
		PersistentPinDifferenceResult.Result->TryGetStringField(
			TEXT("first_changed_persistent_property"), PersistentPinDifferenceProperty) &&
		PersistentPinDifferenceProperty.EndsWith(
			TEXT(".Properties"), ESearchCase::CaseSensitive));
	TargetComparisonPin->Properties.PinStatus = SourcePinStatus;
	TestFalse(
		TEXT("Direct persistent-comparison fixture preserves clean package state"),
		TargetGraph->GetPackage()->IsDirty());

	EPropertyBagAlterationResult AddTargetOnlyParameterResult =
		EPropertyBagAlterationResult::SourcePropertyNotFound;
	TargetGraph->UpdateUserParametersStruct(
		[&AddTargetOnlyParameterResult](FInstancedPropertyBag& Parameters)
		{
			FPropertyBagPropertyDesc TargetOnlyDescriptor(
				TEXT("TargetOnlyParameter"), EPropertyBagPropertyType::Double);
			TargetOnlyDescriptor.ID = FGuid::NewGuid();
			AddTargetOnlyParameterResult = Parameters.AddProperties({TargetOnlyDescriptor});
		});
	TestEqual(
		TEXT("Schema-difference fixture adds exactly one target-only descriptor"),
		AddTargetOnlyParameterResult,
		EPropertyBagAlterationResult::Success);
	if (AddTargetOnlyParameterResult != EPropertyBagAlterationResult::Success ||
		!SaveFixtureGraph(*this, TargetGraph, TEXT("Save target-only user-parameter schema")))
	{
		return false;
	}
	const FMonolithActionResult SchemaDifferenceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	if (!RequireSuccess(*this, TEXT("replacement detects one descriptor difference"), SchemaDifferenceResult))
	{
		return false;
	}
	FString SchemaDifferenceStatus;
	FString SchemaDifferenceProperty;
	TestTrue(TEXT("One descriptor difference requires replacement"),
		SchemaDifferenceResult.Result->TryGetStringField(TEXT("status"), SchemaDifferenceStatus) &&
		SchemaDifferenceStatus == TEXT("would_replace"));
	TestTrue(TEXT("Descriptor difference is attributed to the property-bag schema"),
		SchemaDifferenceResult.Result->TryGetStringField(
			TEXT("first_changed_persistent_property"), SchemaDifferenceProperty) &&
		SchemaDifferenceProperty.StartsWith(
			TEXT("UserParameters.schema"), ESearchCase::CaseSensitive));

	EPropertyBagAlterationResult RemoveTargetOnlyParameterResult =
		EPropertyBagAlterationResult::SourcePropertyNotFound;
	TargetGraph->UpdateUserParametersStruct(
		[&RemoveTargetOnlyParameterResult](FInstancedPropertyBag& Parameters)
		{
			RemoveTargetOnlyParameterResult =
				Parameters.RemovePropertyByName(TEXT("TargetOnlyParameter"));
		});
	TestEqual(
		TEXT("Schema-difference fixture restores the source descriptor set"),
		RemoveTargetOnlyParameterResult,
		EPropertyBagAlterationResult::Success);
	if (RemoveTargetOnlyParameterResult != EPropertyBagAlterationResult::Success ||
		!SaveFixtureGraph(*this, TargetGraph, TEXT("Restore source user-parameter schema")))
	{
		return false;
	}

	EPropertyBagResult SetDifferentValueResult = EPropertyBagResult::PropertyNotFound;
	TargetGraph->UpdateUserParametersStruct(
		[&SetDifferentValueResult](FInstancedPropertyBag& Parameters)
		{
			SetDifferentValueResult =
				Parameters.SetValueDouble(TEXT("ReplacementDensity"), 0.375);
		});
	TestEqual(
		TEXT("Value-difference fixture changes only the target parameter value"),
		SetDifferentValueResult,
		EPropertyBagResult::Success);
	if (SetDifferentValueResult != EPropertyBagResult::Success ||
		!SaveFixtureGraph(*this, TargetGraph, TEXT("Save different user-parameter value")))
	{
		return false;
	}
	const FMonolithActionResult ValueDifferenceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	if (!RequireSuccess(*this, TEXT("replacement detects one parameter-value difference"), ValueDifferenceResult))
	{
		return false;
	}
	FString ValueDifferenceStatus;
	FString ValueDifferenceProperty;
	TestTrue(TEXT("One parameter-value difference requires replacement"),
		ValueDifferenceResult.Result->TryGetStringField(TEXT("status"), ValueDifferenceStatus) &&
		ValueDifferenceStatus == TEXT("would_replace"));
	TestTrue(TEXT("Value difference is attributed to the exact property-bag entry"),
		ValueDifferenceResult.Result->TryGetStringField(
			TEXT("first_changed_persistent_property"), ValueDifferenceProperty) &&
		ValueDifferenceProperty == TEXT("UserParameters.value[0:ReplacementDensity]"));

	EPropertyBagResult RestoreValueResult = EPropertyBagResult::PropertyNotFound;
	TargetGraph->UpdateUserParametersStruct(
		[&RestoreValueResult](FInstancedPropertyBag& Parameters)
		{
			RestoreValueResult =
				Parameters.SetValueDouble(TEXT("ReplacementDensity"), 0.625);
		});
	TestEqual(
		TEXT("Value-difference fixture restores the exact source value"),
		RestoreValueResult,
		EPropertyBagResult::Success);
	if (RestoreValueResult != EPropertyBagResult::Success ||
		!SaveFixtureGraph(*this, TargetGraph, TEXT("Restore source user-parameter value")))
	{
		return false;
	}

	const FMonolithActionResult NoOpResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), ApplyParams);
	if (!RequireSuccess(*this, TEXT("replacement idempotent repeat"), NoOpResult))
	{
		return false;
	}
	FString NoOpStatus;
	bool bNoOpSaved = true;
	TestTrue(TEXT("Exact repeat reports unchanged"),
		NoOpResult.Result->TryGetStringField(TEXT("status"), NoOpStatus) &&
		NoOpStatus == TEXT("unchanged"));
	TestTrue(TEXT("Exact repeat skips redundant save"),
		NoOpResult.Result->TryGetBoolField(TEXT("saved"), bNoOpSaved) && !bNoOpSaved);
	if (!RequireReplacementSourceControlStatus(
			*this, TEXT("replacement idempotent repeat"), NoOpResult.Result,
			TEXT("skipped_no_change")))
	{
		return false;
	}
	TestFalse(TEXT("Idempotent repeat keeps target clean"), TargetGraph->GetPackage()->IsDirty());
	TestFalse(TEXT("Source package remains clean and unmodified"), SourceGraph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphUserParametersAtomicTest,
	"Monolith.PCG.GraphAuthoring.UserParameters.AtomicDryRunSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphUserParametersAtomicTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture Fixture(TEXT("PCG_UserParameters"));
	RegisterGraphAuthoringActions();
	if (!RequireSuccess(*this, TEXT("create_pcg_graph"), CreateGraph(Fixture)))
	{
		return false;
	}

	auto MakeUpsert = [](const FString& Name, const FString& Type,
		const TSharedPtr<FJsonValue>& DefaultValue)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("type"), Type);
		Entry->SetField(TEXT("default_value"), DefaultValue);
		return MakeShared<FJsonValueObject>(Entry);
	};
	TArray<TSharedPtr<FJsonValue>> Upserts;
	Upserts.Add(MakeUpsert(TEXT("Density"), TEXT("float"), MakeShared<FJsonValueNumber>(0.25)));
	Upserts.Add(MakeUpsert(TEXT("WorldSeed"), TEXT("int64"),
		MakeShared<FJsonValueString>(TEXT("9223372036854775807"))));
	TSharedPtr<FJsonObject> InvalidNameParams = MakeAssetParams(Fixture);
	InvalidNameParams->SetArrayField(TEXT("upsert"), {
		MakeUpsert(TEXT("Invalid.Name"), TEXT("float"), MakeShared<FJsonValueNumber>(1.0))});
	const FMonolithActionResult InvalidNameResult = ExecuteAction(
		TEXT("set_pcg_graph_user_parameters"), InvalidNameParams);
	TestFalse(TEXT("Property-bag-invalid user-parameter name is rejected"),
		InvalidNameResult.bSuccess);
	TestTrue(TEXT("Invalid user-parameter name rejection identifies the offending name"),
		InvalidNameResult.ErrorMessage.Contains(TEXT("Invalid.Name"), ESearchCase::CaseSensitive));
	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *Fixture.ObjectPath);
	TestEqual(TEXT("Rejected invalid name does not add parameters"),
		Graph && Graph->GetUserParametersStruct()->GetPropertyBagStruct()
			? Graph->GetUserParametersStruct()->GetPropertyBagStruct()->GetPropertyDescs().Num() : 0, 0);

	TSharedPtr<FJsonObject> DryRunParams = MakeAssetParams(Fixture);
	DryRunParams->SetArrayField(TEXT("upsert"), Upserts);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult DryRunResult = ExecuteAction(
		TEXT("set_pcg_graph_user_parameters"), DryRunParams);
	if (!RequireSuccess(*this, TEXT("user-parameter dry-run"), DryRunResult))
	{
		return false;
	}
	Graph = LoadObject<UPCGGraph>(nullptr, *Fixture.ObjectPath);
	TestNotNull(TEXT("Graph remains loaded after dry-run"), Graph);
	TestEqual(TEXT("Dry-run does not add parameters"),
		Graph && Graph->GetUserParametersStruct()->GetPropertyBagStruct()
			? Graph->GetUserParametersStruct()->GetPropertyBagStruct()->GetPropertyDescs().Num() : 0, 0);

	TSharedPtr<FJsonObject> ApplyParams = MakeAssetParams(Fixture);
	ApplyParams->SetArrayField(TEXT("upsert"), Upserts);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("save"), true);
	if (!RequireSuccess(*this, TEXT("user-parameter apply"), ExecuteAction(
		TEXT("set_pcg_graph_user_parameters"), ApplyParams)))
	{
		return false;
	}
	Graph = ReloadGraph(*this, Fixture);
	if (!Graph)
	{
		return false;
	}
	const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
	const FPropertyBagPropertyDesc* Density = Bag ? Bag->FindPropertyDescByName(TEXT("Density")) : nullptr;
	const FPropertyBagPropertyDesc* WorldSeed = Bag ? Bag->FindPropertyDescByName(TEXT("WorldSeed")) : nullptr;
	TestNotNull(TEXT("Density schema persists"), Density);
	TestNotNull(TEXT("WorldSeed schema persists"), WorldSeed);
	if (Density)
	{
		TestEqual(TEXT("Density type persists"), Density->ValueType, EPropertyBagPropertyType::Float);
		const TValueOrError<FString, EPropertyBagResult> Value = Bag->GetValueSerializedString(Density->Name);
		TestTrue(TEXT("Density default is readable"), Value.IsValid());
		float ParsedDensity = 0.0f;
		TestTrue(TEXT("Density default parses after reload"),
			Value.IsValid() && LexTryParseString(ParsedDensity, *Value.GetValue()));
		TestEqual(TEXT("Density default persists"), ParsedDensity, 0.25f);
	}
	if (WorldSeed)
	{
		const TValueOrError<FString, EPropertyBagResult> Value = Bag->GetValueSerializedString(WorldSeed->Name);
		TestTrue(TEXT("WorldSeed default is readable"), Value.IsValid());
		TestEqual(TEXT("Full signed int64 default persists exactly"),
			Value.IsValid() ? Value.GetValue() : FString(), FString(TEXT("9223372036854775807")));
	}
	TestFalse(TEXT("Saved user-parameter graph reloads clean"), Graph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphContentsReplacementLargeGraphBoundedComparisonTest,
	"Monolith.PCG.GraphAuthoring.GraphContents.LargeGraphBoundedPersistentComparison",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphContentsReplacementLargeGraphBoundedComparisonTest::RunTest(
	const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture TargetFixture(TEXT("PCG_ReplaceLarge_Target"));
	FScopedPCGGraphFixture SourceFixture(TEXT("PCG_ReplaceLarge_Source"));
	RegisterGraphAuthoringActions();
	if (!RequireSuccess(*this, TEXT("create large replacement source"), CreateGraph(SourceFixture)) ||
		!RequireSuccess(*this, TEXT("create large replacement target"), CreateGraph(TargetFixture)))
	{
		return false;
	}

	UPCGGraph* const SourceGraph = LoadGraph(SourceFixture);
	UPCGGraph* const TargetGraph = LoadGraph(TargetFixture);
	if (!TestNotNull(TEXT("Large replacement source is loadable"), SourceGraph) ||
		!TestNotNull(TEXT("Large replacement target is loadable"), TargetGraph))
	{
		return false;
	}

	// This is intentionally larger than the 561-node production donor that
	// exposed recursive PPF_DeepComparison amplification. Direct UE authoring
	// avoids timing hundreds of independent action/save calls; the timed region
	// below measures the complete replacement dry-run contract.
	constexpr int32 LargeNodeCount = 600;
	{
		SourceGraph->DisableNotificationsForEditor();
		ON_SCOPE_EXIT
		{
			SourceGraph->EnableNotificationsForEditor();
		};
		for (int32 Index = 0; Index < LargeNodeCount; ++Index)
		{
			UPCGAddTagSettings* Settings = nullptr;
			UPCGNode* const Node = SourceGraph->AddNodeOfType(Settings);
			if (!Node || !Settings)
			{
				AddError(FString::Printf(
					TEXT("Could not create large-graph fixture node %d"), Index));
				return false;
			}
			Node->SetNodeTitle(FName(*FString::Printf(TEXT("Large_%04d"), Index)));
			Node->SetNodePosition((Index % 30) * 240, (Index / 30) * 160);
			Settings->TagsToAdd = FString::Printf(TEXT("LargeReplacement:%04d"), Index);
		}
	}
	TestEqual(
		TEXT("Large source contains the requested element count"),
		SourceGraph->GetNodes().Num(), LargeNodeCount);
	TArray<UObject*> LargeSourceInnerObjects;
	MonolithObjectTraversal::GetObjectsWithOuter(
		SourceGraph,
		LargeSourceInnerObjects,
		true);
	TestTrue(
		*FString::Printf(
			TEXT("Large source exposes at least one node and one settings inner per element (%d inners)"),
			LargeSourceInnerObjects.Num()),
		LargeSourceInnerObjects.Num() >= LargeNodeCount * 2);
	if (!SaveFixtureGraph(*this, SourceGraph, TEXT("Save large replacement source")))
	{
		return false;
	}
	TestEqual(TEXT("Large replacement target starts empty"), TargetGraph->GetNodes().Num(), 0);
	TestFalse(TEXT("Large replacement target starts clean"), TargetGraph->GetPackage()->IsDirty());

#if WITH_DEV_AUTOMATION_TESTS
	UE::MonolithPCG::Private::ConfigureGraphContentsReplacementTestFault(
		TargetFixture.ObjectPath,
		UE::MonolithPCG::Private::EPCGGraphContentsReplacementTestFault::None);
	ON_SCOPE_EXIT
	{
		UE::MonolithPCG::Private::ResetGraphContentsReplacementTestFault();
	};
#endif

	TSharedPtr<FJsonObject> DryRunParams = MakeReplacementParams(SourceFixture, TargetFixture);
	DryRunParams->SetNumberField(TEXT("node_limit"), LargeNodeCount);
	const double DifferentStartSeconds = FPlatformTime::Seconds();
	const FMonolithActionResult DifferentDryRun = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), DryRunParams);
	const double DifferentElapsedSeconds = FPlatformTime::Seconds() - DifferentStartSeconds;
	if (!RequireSuccess(*this, TEXT("large replacement different dry-run"), DifferentDryRun))
	{
		return false;
	}
	FString DifferentStatus;
	TestTrue(
		TEXT("Large different graph reports would_replace"),
		DifferentDryRun.Result->TryGetStringField(TEXT("status"), DifferentStatus) &&
		DifferentStatus == TEXT("would_replace"));
	TestTrue(
		*FString::Printf(
			TEXT("600-node different dry-run remains bounded (elapsed %.3fs, generous ceiling 60s)"),
			DifferentElapsedSeconds),
		DifferentElapsedSeconds < 60.0);
	TestEqual(
		TEXT("Large dry-run preserves target topology"), TargetGraph->GetNodes().Num(), 0);
	TestFalse(
		TEXT("Large dry-run preserves target dirty state"), TargetGraph->GetPackage()->IsDirty());

	TSharedPtr<FJsonObject> ApplyParams = MakeReplacementParams(SourceFixture, TargetFixture);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("save"), true);
	ApplyParams->SetNumberField(TEXT("node_limit"), LargeNodeCount);
	if (!RequireSuccess(
			*this,
			TEXT("large replacement commit"),
			ExecuteAction(TEXT("replace_pcg_graph_contents"), ApplyParams)))
	{
		return false;
	}
	TestEqual(
		TEXT("Large replacement commit copies every element"),
		TargetGraph->GetNodes().Num(), LargeNodeCount);
	TestFalse(TEXT("Large replacement commit saves cleanly"), TargetGraph->GetPackage()->IsDirty());

	const double IdenticalStartSeconds = FPlatformTime::Seconds();
	const FMonolithActionResult IdenticalDryRun = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), DryRunParams);
	const double IdenticalElapsedSeconds = FPlatformTime::Seconds() - IdenticalStartSeconds;
	if (!RequireSuccess(*this, TEXT("large replacement identical dry-run"), IdenticalDryRun))
	{
		return false;
	}
	FString IdenticalStatus;
	bool bSaved = true;
	TestTrue(
		TEXT("Large identical graph reports unchanged"),
		IdenticalDryRun.Result->TryGetStringField(TEXT("status"), IdenticalStatus) &&
		IdenticalStatus == TEXT("unchanged"));
	TestTrue(
		TEXT("Large identical dry-run never saves"),
		IdenticalDryRun.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
	TestTrue(
		*FString::Printf(
			TEXT("600-node identical dry-run remains bounded (elapsed %.3fs, generous ceiling 60s)"),
			IdenticalElapsedSeconds),
		IdenticalElapsedSeconds < 60.0);
	TestFalse(
		TEXT("Large identical dry-run keeps target clean"), TargetGraph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphContentsReplacementRollbackTest,
	"Monolith.PCG.GraphAuthoring.GraphContents.InjectedSaveFailureRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphContentsReplacementRollbackTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture TargetFixture(TEXT("PCG_ReplaceRollback_Target"));
	FScopedPCGGraphFixture SourceFixture(TEXT("PCG_ReplaceRollback_Source"));
	RegisterGraphAuthoringActions();
	if (!RequireSuccess(*this, TEXT("create rollback source"), CreateGraph(SourceFixture)) ||
		!RequireSuccess(*this, TEXT("create rollback target"), CreateGraph(TargetFixture)) ||
		!RequireSuccess(*this, TEXT("add rollback source node"),
			AddNode(SourceFixture, TEXT("PCGAddTagSettings"), TEXT("RollbackSourceNode"))) ||
		!RequireSuccess(*this, TEXT("add rollback target node"),
			AddNode(TargetFixture, TEXT("PCGAddTagSettings"), TEXT("RollbackTargetNode"))))
	{
		return false;
	}
	UPCGGraph* SourceGraph = LoadGraph(SourceFixture);
	UPCGGraph* TargetGraph = LoadGraph(TargetFixture);
	if (!ConfigureReplacementSourceGraph(*this, SourceGraph) || !TargetGraph)
	{
		return false;
	}
	const FString TargetIdentity = TargetGraph->GetPathName();
	const int32 OriginalEdgeCount = CountGraphEdges(TargetGraph);
	const bool bOriginalLandscapeMetadata = TargetGraph->bLandscapeUsesMetadata;
	TestFalse(TEXT("Rollback target starts clean"), TargetGraph->GetPackage()->IsDirty());

#if WITH_DEV_AUTOMATION_TESTS
	UE::MonolithPCG::Private::ConfigureGraphContentsReplacementTestFault(
		TargetFixture.ObjectPath,
		UE::MonolithPCG::Private::EPCGGraphContentsReplacementTestFault::BeforeSave);
	ON_SCOPE_EXIT
	{
		UE::MonolithPCG::Private::ResetGraphContentsReplacementTestFault();
	};
#endif

	TSharedPtr<FJsonObject> ApplyParams = MakeReplacementParams(SourceFixture, TargetFixture);
	ApplyParams->SetBoolField(TEXT("dry_run"), false);
	ApplyParams->SetBoolField(TEXT("confirm"), true);
	ApplyParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult FailureResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), ApplyParams);
	TestFalse(TEXT("Injected pre-save failure is reported"), FailureResult.bSuccess);
	TestTrue(TEXT("Injected failure reports verified rollback"),
		FailureResult.ErrorMessage.Contains(TEXT("rollback=verified"), ESearchCase::CaseSensitive));
	const TSharedPtr<FJsonObject> FailureErrorData =
		MonolithPCGResultUtils::GetErrorDataObject(FailureResult);
	if (!RequireReplacementSourceControlStatus(
			*this, TEXT("replacement rollback failure"), FailureErrorData, TEXT("prepared")))
	{
		return false;
	}
	bool bRollbackPersistentPropertiesVerified = false;
	TestTrue(TEXT("Injected failure reports deep rollback verification"),
		FailureErrorData.IsValid() && FailureErrorData->TryGetBoolField(
			TEXT("rollback_persistent_properties_verified"),
			bRollbackPersistentPropertiesVerified) &&
		bRollbackPersistentPropertiesVerified);
	TestEqual(TEXT("Rollback preserves target UObject identity"), LoadGraph(TargetFixture), TargetGraph);
	TestEqual(TEXT("Rollback preserves target object path"), TargetGraph->GetPathName(), TargetIdentity);
	TestNotNull(TEXT("Rollback restores target-only node"),
		FindNodeByTitle(TargetGraph, TEXT("RollbackTargetNode")));
	TestNull(TEXT("Rollback removes cloned source node"),
		FindNodeByTitle(TargetGraph, TEXT("RollbackSourceNode")));
	TestEqual(TEXT("Rollback restores original edge count"), CountGraphEdges(TargetGraph), OriginalEdgeCount);
	TestEqual(TEXT("Rollback restores graph-level property"),
		TargetGraph->bLandscapeUsesMetadata, bOriginalLandscapeMetadata);
	TestFalse(TEXT("Rollback restores default hierarchical-generation policy"),
		TargetGraph->IsHierarchicalGenerationEnabled());
	TestEqual(TEXT("Rollback restores default grid multiplier"),
		GetGraphGridSizeMultiplier(TargetGraph), 1.0);
	TestTrue(TEXT("Rollback restores default 2D-grid policy"), TargetGraph->Use2DGrid());
	const FInstancedPropertyBag* const RolledBackParameters =
		TargetGraph->GetUserParametersStruct();
	TestTrue(TEXT("Rollback removes the donor user-parameter schema"),
		RolledBackParameters && RolledBackParameters->GetNumPropertiesInBag() == 0);
	TestFalse(TEXT("Rollback restores original clean dirty bit"), TargetGraph->GetPackage()->IsDirty());

	TargetGraph = ReloadGraph(*this, TargetFixture);
	if (!TargetGraph)
	{
		return false;
	}
	TestNotNull(TEXT("Reload confirms original target package bytes remain authoritative"),
		FindNodeByTitle(TargetGraph, TEXT("RollbackTargetNode")));
	TestNull(TEXT("Reload confirms failed replacement was not persisted"),
		FindNodeByTitle(TargetGraph, TEXT("RollbackSourceNode")));
	TestFalse(TEXT("Reloaded rollback target remains clean"), TargetGraph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphContentsReplacementRecursionGuardsTest,
	"Monolith.PCG.GraphAuthoring.GraphContents.RecursionGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphContentsReplacementRecursionGuardsTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGGraphAuthoringTests;
	FScopedPCGGraphFixture TargetFixture(TEXT("PCG_ReplaceRecursion_Target"));
	FScopedPCGGraphFixture SourceFixture(TEXT("PCG_ReplaceRecursion_Source"));
	RegisterGraphAuthoringActions();
	if (!RequireSuccess(*this, TEXT("create recursion replacement source"), CreateGraph(SourceFixture)) ||
		!RequireSuccess(*this, TEXT("create recursion replacement target"), CreateGraph(TargetFixture)))
	{
		return false;
	}
	const FMonolithActionResult AddSubgraphResult = AddNode(
		SourceFixture, TEXT("PCGSubgraphSettings"), TEXT("SourceReferencesTarget"));
	if (!RequireSuccess(*this, TEXT("add target-reference subgraph node"), AddSubgraphResult))
	{
		return false;
	}
	FString SubgraphNodeId;
	if (!ReadRequiredString(
			*this, TEXT("target-reference subgraph node"), AddSubgraphResult,
			TEXT("node_id"), SubgraphNodeId))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssignParams = MakeAssetParams(SourceFixture);
	AssignParams->SetStringField(TEXT("node_id"), SubgraphNodeId);
	AssignParams->SetStringField(TEXT("subgraph_asset_path"), TargetFixture.ObjectPath);
	AssignParams->SetBoolField(TEXT("dry_run"), false);
	AssignParams->SetBoolField(TEXT("save"), true);
	if (!RequireSuccess(
			*this, TEXT("assign replacement target into source donor"),
			ExecuteAction(TEXT("set_pcg_subgraph"), AssignParams)))
	{
		return false;
	}

	UPCGGraph* SourceGraph = LoadGraph(SourceFixture);
	UPCGGraph* TargetGraph = LoadGraph(TargetFixture);
	if (!SourceGraph || !TargetGraph)
	{
		return false;
	}
	TestTrue(TEXT("Recursion fixture donor contains the replacement target"),
		SourceGraph->Contains(TargetGraph));
	TestFalse(TEXT("Recursion fixture target starts clean"), TargetGraph->GetPackage()->IsDirty());
	const FMonolithActionResult ReplaceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	TestFalse(TEXT("Replacement that would make the target recursive is rejected"),
		ReplaceResult.bSuccess);
	TestTrue(TEXT("Replacement recursion rejection is explicit"),
		ReplaceResult.ErrorMessage.Contains(TEXT("recursive"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("Rejected recursive replacement preserves target element count"),
		TargetGraph->GetNodes().Num(), 0);
	TestFalse(TEXT("Rejected recursive replacement preserves target dirty state"),
		TargetGraph->GetPackage()->IsDirty());

	UPCGNode* SourceSubgraphNode = FindNodeByTitle(SourceGraph, TEXT("SourceReferencesTarget"));
	UPCGSubgraphSettings* SourceSubgraphSettings =
		SourceSubgraphNode ? Cast<UPCGSubgraphSettings>(SourceSubgraphNode->GetSettings()) : nullptr;
	if (!TestNotNull(TEXT("Recursion fixture retains its subgraph settings"), SourceSubgraphSettings) ||
		!TestNotNull(TEXT("Recursion fixture retains its graph instance"),
			SourceSubgraphSettings ? SourceSubgraphSettings->SubgraphInstance.Get() : nullptr))
	{
		return false;
	}
	UPCGGraphInterface* const OriginalAssignedInterface =
		SourceSubgraphSettings->SubgraphInstance->Graph.Get();
	SourceSubgraphSettings->SubgraphInstance->Graph = SourceGraph;
	ON_SCOPE_EXIT
	{
		SourceSubgraphSettings->SubgraphInstance->Graph = OriginalAssignedInterface;
	};
	TestTrue(TEXT("Recursion fixture can represent a pre-existing recursive donor"),
		SourceGraph->Contains(SourceGraph));

	const FMonolithActionResult RecursiveSourceResult = ExecuteAction(
		TEXT("replace_pcg_graph_contents"), MakeReplacementParams(SourceFixture, TargetFixture));
	TestFalse(TEXT("An already-recursive source graph is rejected"),
		RecursiveSourceResult.bSuccess);
	TestTrue(TEXT("Recursive-source rejection identifies the invalid donor"),
		RecursiveSourceResult.ErrorMessage.Contains(
			TEXT("source graph contains a recursive static-subgraph hierarchy"),
			ESearchCase::CaseSensitive));
	TestEqual(TEXT("Rejected recursive source preserves target element count"),
		TargetGraph->GetNodes().Num(), 0);
	TestFalse(TEXT("Rejected recursive source preserves target dirty state"),
		TargetGraph->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGGraphAuthoringRegistrationTest, "Monolith.PCG.GraphAuthoring.Registration",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphAuthoringRegistrationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	FMonolithPCGGraphAuthoringActions::RegisterActions(Registry);
	FMonolithPCGComponentActions::RegisterActions(Registry);

	const TArray<FString> ExpectedActions = {
		TEXT("get_status"),
		TEXT("list_graph_assets"),
		TEXT("get_graph_asset"),
		TEXT("remap_graph_references"),
		TEXT("list_components"),
		TEXT("list_pcg_node_types"),
		TEXT("create_pcg_graph"),
		TEXT("get_pcg_graph_info"),
		TEXT("add_pcg_node"),
		TEXT("remove_pcg_node"),
		TEXT("connect_pcg_nodes"),
		TEXT("disconnect_pcg_nodes"),
		TEXT("set_pcg_node_params"),
		TEXT("set_pcg_graph_user_parameters"),
		TEXT("set_pcg_subgraph"),
		TEXT("replace_pcg_graph_contents"),
		TEXT("validate_pcg_graph"),
		TEXT("create_component"),
		TEXT("get_component"),
		TEXT("set_component_graph"),
		TEXT("set_blueprint_component_graph"),
		TEXT("set_component_settings"),
		TEXT("generate_component"),
		TEXT("refresh_component"),
		TEXT("cancel_component"),
		TEXT("cleanup_component"),
		TEXT("get_component_output"),
		TEXT("set_component_user_parameters")};

	for (const FString& Action : ExpectedActions)
	{
		TestTrue(*FString::Printf(TEXT("pcg.%s is registered"), *Action), Registry.HasAction(TEXT("pcg"), Action));
	}
	TestEqual(TEXT("PCG action count is synchronized"), Registry.GetActions(TEXT("pcg")).Num(),
		ExpectedActions.Num());

	const FMonolithActionResult StatusResult =
		Registry.ExecuteAction(TEXT("pcg"), TEXT("get_status"), MakeShared<FJsonObject>());
	TestTrue(TEXT("PCG status succeeds after registration"), StatusResult.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* FutureActions = nullptr;
	TestTrue(TEXT("PCG status returns future_actions"),
		StatusResult.Result.IsValid() &&
		StatusResult.Result->TryGetArrayField(TEXT("future_actions"), FutureActions) &&
		FutureActions);
	bool bImplementedParameterActionStillMarkedFuture = false;
	if (FutureActions)
	{
		for (const TSharedPtr<FJsonValue>& FutureAction : *FutureActions)
		{
			FString FutureActionName;
			bImplementedParameterActionStillMarkedFuture =
				bImplementedParameterActionStillMarkedFuture ||
				(FutureAction.IsValid() &&
				 FutureAction->TryGetString(FutureActionName) &&
				 FutureActionName.Equals(
					 TEXT("pcg.edit_graph_user_parameter_schema"),
					 ESearchCase::CaseSensitive));
		}
	}
	TestFalse(
		TEXT("Implemented graph user-parameter authoring is not advertised as future work"),
		bImplementedParameterActionStillMarkedFuture);

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
