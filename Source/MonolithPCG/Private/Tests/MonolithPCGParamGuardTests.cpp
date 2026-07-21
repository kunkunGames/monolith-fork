#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/AutomationTest.h"
#include "MonolithPCGActions.h"
#include "MonolithToolRegistry.h"
#include "PCGGraph.h"
#include "Elements/IO/PCGLoadAssetElement.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithPCGRemapTests
{
	UPCGGraph* CreateGraphAsset(FString& OutObjectPath, UPackage*& OutPackage)
	{
		const FString AssetName = TEXT("PCG_Remap_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = TEXT("/Game/Developers/MonolithTests/") + AssetName;
		OutPackage = CreatePackage(*PackageName);
		UPCGGraph* Graph = NewObject<UPCGGraph>(
			OutPackage,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Graph)
		{
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Graph);
		Graph->PostEditChange();
		OutPackage->SetDirtyFlag(false);
		OutObjectPath = Graph->GetPathName();
		return Graph;
	}

	void DiscardGraphAsset(UPCGGraph* Graph, UPackage* Package)
	{
		if (Graph)
		{
			FAssetRegistryModule::AssetDeleted(Graph);
			Graph->ClearFlags(RF_Public | RF_Standalone);
			Graph->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_AllowPackageLinkerMismatch);
			Graph->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	}

	TSharedPtr<FJsonObject> MakeMutationParams(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> RootRemaps = MakeShared<FJsonObject>();
		RootRemaps->SetStringField(TEXT("/Game/Old"), TEXT("/Game/New"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), ObjectPath);
		Params->SetObjectField(TEXT("root_remaps"), RootRemaps);
		Params->SetBoolField(TEXT("dry_run"), false);
		Params->SetBoolField(TEXT("confirm"), true);
		Params->SetBoolField(TEXT("require_targets"), false);
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardPCGGraphAssetRejectsUnsafePathTest, "Monolith.ParamGuard.MonolithPCG.GraphAssetRejectsUnsafePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardPCGGraphAssetRejectsUnsafePathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	TestTrue(TEXT("pcg.get_graph_asset action is registered"), Registry.HasAction(TEXT("pcg"), TEXT("get_graph_asset")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PCGGraph.uasset"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
	TestFalse(TEXT("GetGraphAsset rejects out-of-project paths"), Result.bSuccess);
	TestTrue(TEXT("GetGraphAsset reports the project-owned package boundary"), Result.ErrorMessage.Contains(TEXT("project")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGRemapGraphReferenceContractTest, "Monolith.PCG.RemapGraphReferenceContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGRemapGraphReferenceContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	TestTrue(TEXT("pcg.remap_graph_references action is registered"), Registry.HasAction(TEXT("pcg"), TEXT("remap_graph_references")));
	TestEqual(
		TEXT("pcg.remap_graph_references uses a guarded transaction policy"),
		Registry.GetActionExecutionPolicy(TEXT("pcg"), TEXT("remap_graph_references")).PolicyId,
		FString(TEXT("transaction_optional")));

	TSharedPtr<FJsonObject> RootRemaps = MakeShared<FJsonObject>();
	RootRemaps->SetStringField(TEXT("/Game/Wall"), TEXT("/Game/Remapped/Wall"));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing/PCG_Graph"));
		Params->SetObjectField(TEXT("root_remaps"), RootRemaps);
		Params->SetBoolField(TEXT("dry_run"), false);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("remap_graph_references"), Params);
		TestFalse(TEXT("Mutation requires explicit confirmation"), Result.bSuccess);
		TestTrue(TEXT("Mutation guard reports confirm requirement"), Result.ErrorMessage.Contains(TEXT("confirm=true")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PCGGraph.uasset"));
		Params->SetObjectField(TEXT("root_remaps"), RootRemaps);
		Params->SetBoolField(TEXT("dry_run"), true);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("remap_graph_references"), Params);
		TestFalse(TEXT("Filesystem paths are rejected"), Result.bSuccess);
		TestTrue(TEXT("Filesystem path error identifies Unreal path contract"), Result.ErrorMessage.Contains(TEXT("Unreal package")));
	}

	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPCGRemapSoftObjectPathTest, "Monolith.PCG.RemapSoftObjectPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGRemapSoftObjectPathTest::RunTest(const FString& Parameters)
{
	TMap<FString, FString> RootRemaps;
	RootRemaps.Add(TEXT("/Game/PCG/PCG_Default"), TEXT("/SpeedMaps/ProjectMGH/Authoring/PCG_ProjectMGHLayout"));
	RootRemaps.Add(TEXT("/Game/Wall"), TEXT("/SpeedMaps/Meshes/Wall"));

	FSoftObjectPath RemappedPath;
	TestTrue(
		TEXT("Exact graph package remap succeeds"),
		FMonolithPCGActions::RemapSoftObjectPathForTest(
			FSoftObjectPath(TEXT("/Game/PCG/PCG_Default.PCG_Default")),
			RootRemaps,
			RemappedPath));
	TestEqual(
		TEXT("Exact graph remap renames the top-level asset with its destination package"),
		RemappedPath.ToString(),
		FString(TEXT("/SpeedMaps/ProjectMGH/Authoring/PCG_ProjectMGHLayout.PCG_ProjectMGHLayout")));

	TestTrue(
		TEXT("Descendant mesh package remap succeeds"),
		FMonolithPCGActions::RemapSoftObjectPathForTest(
			FSoftObjectPath(TEXT("/Game/Wall/SM_Wall_I_100.SM_Wall_I_100")),
			RootRemaps,
			RemappedPath));
	TestEqual(
		TEXT("Descendant mesh remap preserves the asset name"),
		RemappedPath.ToString(),
		FString(TEXT("/SpeedMaps/Meshes/Wall/SM_Wall_I_100.SM_Wall_I_100")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGRemapDispatchesSettingsPropertyEventTest,
	"Monolith.PCG.Remap.DispatchesSettingsPropertyEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGRemapDispatchesSettingsPropertyEventTest::RunTest(const FString& Parameters)
{
	FString ObjectPath;
	UPackage* Package = nullptr;
	UPCGGraph* Graph = MonolithPCGRemapTests::CreateGraphAsset(ObjectPath, Package);
	if (!TestNotNull(TEXT("Creates a test PCG graph asset"), Graph))
	{
		return false;
	}

	UPCGLoadDataAssetSettings* Settings = nullptr;
	UPCGNode* Node = Graph->AddNodeOfType(Settings);
	if (!TestNotNull(TEXT("Creates a load-data-asset PCG node"), Node)
		|| !TestNotNull(TEXT("Creates load-data-asset settings"), Settings))
	{
		MonolithPCGRemapTests::DiscardGraphAsset(Graph, Package);
		return false;
	}

	const FSoftObjectPath OldPath(TEXT("/Game/Old/DA_Source.DA_Source"));
	const FSoftObjectPath NewPath(TEXT("/Game/New/DA_Source.DA_Source"));
	Settings->Asset = TSoftObjectPtr<UPCGDataAsset>(OldPath);
	Package->SetDirtyFlag(false);

	int32 AssetPropertyEventCount = 0;
	const FDelegateHandle PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[Settings, &AssetPropertyEventCount](UObject* Object, FPropertyChangedEvent& Event)
		{
			if (Object == Settings
				&& Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UPCGLoadDataAssetSettings, Asset))
			{
				++AssetPropertyEventCount;
			}
		});

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("pcg"),
		TEXT("remap_graph_references"),
		MonolithPCGRemapTests::MakeMutationParams(ObjectPath));
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);

	TestTrue(TEXT("Reference remap succeeds"), Result.bSuccess);
	TestEqual(
		TEXT("Load-data-asset reference is remapped"),
		Settings->Asset.ToSoftObjectPath().ToString(),
		NewPath.ToString());
	TestTrue(TEXT("Asset receives a property-specific post-edit event"), AssetPropertyEventCount > 0);
	TestTrue(TEXT("Successful unsaved remap leaves the graph package dirty"), Package->IsDirty());

	MonolithPCGRemapTests::DiscardGraphAsset(Graph, Package);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGRemapRejectsSetDestinationCollisionTest,
	"Monolith.PCG.Remap.RejectsSetDestinationCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGRemapRejectsSetDestinationCollisionTest::RunTest(const FString& Parameters)
{
	FString ObjectPath;
	UPackage* Package = nullptr;
	UPCGGraph* Graph = MonolithPCGRemapTests::CreateGraphAsset(ObjectPath, Package);
	if (!TestNotNull(TEXT("Creates a test PCG graph asset"), Graph))
	{
		return false;
	}

	const TSoftObjectPtr<UPCGGraph> First(FSoftObjectPath(TEXT("/Game/Old/A/PCG_A.PCG_A")));
	const TSoftObjectPtr<UPCGGraph> Second(FSoftObjectPath(TEXT("/Game/Old/B/PCG_B.PCG_B")));
	Graph->GraphCustomization.FilteredSubgraphTypes.Add(First);
	Graph->GraphCustomization.FilteredSubgraphTypes.Add(Second);
	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> RootRemaps = MakeShared<FJsonObject>();
	RootRemaps->SetStringField(TEXT("/Game/Old/A/PCG_A"), TEXT("/Game/New/PCG_Target"));
	RootRemaps->SetStringField(TEXT("/Game/Old/B/PCG_B"), TEXT("/Game/New/PCG_Target"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ObjectPath);
	Params->SetObjectField(TEXT("root_remaps"), RootRemaps);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), true);
	Params->SetBoolField(TEXT("require_targets"), false);
	Params->SetBoolField(TEXT("save"), false);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("pcg"),
		TEXT("remap_graph_references"),
		Params);

	TestFalse(TEXT("Preflight rejects converging TSet destinations"), Result.bSuccess);
	TestTrue(TEXT("Collision failure returns structured diagnostics"), Result.ErrorData.IsValid());
	TestEqual(TEXT("Collision preflight preserves both set entries"), Graph->GraphCustomization.FilteredSubgraphTypes.Num(), 2);
	TestTrue(TEXT("Collision preflight preserves the first path"), Graph->GraphCustomization.FilteredSubgraphTypes.Contains(First));
	TestTrue(TEXT("Collision preflight preserves the second path"), Graph->GraphCustomization.FilteredSubgraphTypes.Contains(Second));
	TestFalse(TEXT("Collision preflight preserves the original package dirty state"), Package->IsDirty());

	MonolithPCGRemapTests::DiscardGraphAsset(Graph, Package);
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardPCGGetGraphAssetParamsTest, "Monolith.ParamGuard.MonolithPCG.GetGraphAssetParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardPCGGetGraphAssetParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("asset_path"), true);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects boolean asset_path"), Result.bSuccess);
		TestTrue(TEXT("Error mentions string"), Result.ErrorMessage.Contains(TEXT("string")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/PCG"));
		Params->SetStringField(TEXT("include_tags"), TEXT("true"));
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects string include_tags"), Result.bSuccess);
		TestTrue(TEXT("Error mentions boolean"), Result.ErrorMessage.Contains(TEXT("boolean")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/PCG"));
		Params->SetStringField(TEXT("tag_limit"), TEXT("50"));
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects string tag_limit"), Result.bSuccess);
		TestTrue(TEXT("Error mentions numeric type"),
			Result.ErrorMessage.Contains(TEXT("number")) || Result.ErrorMessage.Contains(TEXT("integer")));
	}

	return true;
}
