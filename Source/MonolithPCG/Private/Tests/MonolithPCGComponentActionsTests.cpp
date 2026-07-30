#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"

#include "MonolithPCGComponentActions.h"
#include "MonolithPCGResultUtils.h"
#include "MonolithToolRegistry.h"

#include "Grid/PCGPartitionActor.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithPCGComponentActionsTests
{
// Component lifecycle behavior is exercised entirely in the editor world with
// a transient graph. Disk-backed graph package cleanup remains owned by the
// graph-authoring fixture in MonolithPCGGraphAuthoringTests.cpp.
UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

class FScopedActorFixture
{
public:
	explicit FScopedActorFixture(
		UWorld* InWorld,
		UClass* InActorClass = AActor::StaticClass(),
		const TCHAR* InBaseName = TEXT("MonolithPCGComponentActionTestActor"))
		: World(InWorld)
	{
		if (!World || !InActorClass || !InActorClass->IsChildOf(AActor::StaticClass()))
		{
			return;
		}
		LevelPackage = World->GetCurrentLevel() ? World->GetCurrentLevel()->GetOutermost() : nullptr;
		bLevelWasDirty = LevelPackage && LevelPackage->IsDirty();

		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(
			World,
			InActorClass,
			FName(InBaseName));
		SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;
		Actor = World->SpawnActor<AActor>(InActorClass, FTransform::Identity, SpawnParams);
		if (Actor && InActorClass == AActor::StaticClass())
		{
			Actor->SetActorLabel(TEXT("Monolith PCG Component Action Test"));
		}
	}

	~FScopedActorFixture()
	{
		if (World && IsValid(Actor))
		{
			if (GEditor)
			{
				World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
			}
			else
			{
				World->DestroyActor(Actor);
			}
		}
		if (LevelPackage)
		{
			LevelPackage->SetDirtyFlag(bLevelWasDirty);
		}
	}

	UWorld* World = nullptr;
	AActor* Actor = nullptr;
	UPackage* LevelPackage = nullptr;
	bool bLevelWasDirty = false;
};

bool AddValidBoundsRoot(FAutomationTestBase& Test, AActor* Actor)
{
	if (!Actor)
	{
		Test.AddError(TEXT("Cannot create PCG test bounds without an actor"));
		return false;
	}
	if (Actor->GetRootComponent())
	{
		Test.AddError(TEXT("PCG test actor unexpectedly already has a root component"));
		return false;
	}

	UBoxComponent* BoundsComponent = NewObject<UBoxComponent>(
		Actor,
		UBoxComponent::StaticClass(),
		MakeUniqueObjectName(Actor, UBoxComponent::StaticClass(), FName(TEXT("MonolithPCGActionTestBounds"))),
		RF_Transient);
	if (!BoundsComponent)
	{
		Test.AddError(TEXT("Could not create the PCG test bounds component"));
		return false;
	}

	BoundsComponent->SetBoxExtent(FVector(100.0));
	Actor->AddInstanceComponent(BoundsComponent);
	if (!Actor->SetRootComponent(BoundsComponent))
	{
		BoundsComponent->DestroyComponent();
		Test.AddError(TEXT("Could not install the PCG test bounds component as the actor root"));
		return false;
	}

	BoundsComponent->RegisterComponent();
	if (!BoundsComponent->IsRegistered()
		|| Actor->GetComponentsBoundingBox(/*bNonColliding=*/true).IsValid == 0)
	{
		BoundsComponent->DestroyComponent();
		Test.AddError(TEXT("PCG test actor did not expose valid registered generation bounds"));
		return false;
	}

	return true;
}

void RegisterActions()
{
	FMonolithPCGComponentActions::RegisterActions(FMonolithToolRegistry::Get());
}

FMonolithActionResult ExecuteAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("pcg"), Action, Params);
}

UPCGComponent* FindComponentExactOnActor(AActor* Actor, const FString& ComponentPath)
{
	if (!Actor)
	{
		return nullptr;
	}
	TArray<UPCGComponent*> Components;
	Actor->GetComponents(Components);
	for (UPCGComponent* Component : Components)
	{
		if (IsValid(Component) && Component->GetPathName().Equals(ComponentPath, ESearchCase::CaseSensitive))
		{
			return Component;
		}
	}
	return nullptr;
}

bool RequireSuccess(
	FAutomationTestBase& Test,
	const FString& Context,
	const FMonolithActionResult& Result)
{
	if (!Result.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("%s failed: %s"), *Context, *Result.ErrorMessage));
		return false;
	}
	if (!Result.Result.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s returned no result object"), *Context));
		return false;
	}
	return true;
}

bool RequireTransientSourceControlPrepare(
	FAutomationTestBase& Test,
	const FString& Context,
	const TSharedPtr<FJsonObject>& Result)
{
	const TSharedPtr<FJsonObject>* Prepare = nullptr;
	if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("source_control_prepare"), Prepare) ||
		!Prepare || !Prepare->IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s returned no source_control_prepare object"), *Context));
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s uses handler-owned pre-mutation preparation"), *Context),
		(*Prepare)->GetStringField(TEXT("mode")),
		FString(TEXT("handler_owned_pre_mutation")));
	Test.TestEqual(
		*FString::Printf(TEXT("%s skips transient fixture packages explicitly"), *Context),
		(*Prepare)->GetStringField(TEXT("status")),
		FString(TEXT("skipped_non_project_package")));

	const TSharedPtr<FJsonObject>* BeforeAction = nullptr;
	if (!(*Prepare)->TryGetObjectField(TEXT("before_action"), BeforeAction) ||
		!BeforeAction || !BeforeAction->IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s returned no source-control before_action result"), *Context));
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s treats the non-project skip as non-fatal"), *Context),
		(*BeforeAction)->GetBoolField(TEXT("ok")));
	Test.TestEqual(
		*FString::Printf(TEXT("%s reports the before_action skip reason"), *Context),
		(*BeforeAction)->GetStringField(TEXT("status")),
		FString(TEXT("skipped_non_project_package")));
	return true;
}

UPCGComponent* CreateComponent(
	FAutomationTestBase& Test,
	AActor* Actor,
	FString& OutComponentPath,
	TSharedPtr<FJsonObject>* OutResult = nullptr)
{
	OutComponentPath.Reset();
	if (OutResult)
	{
		OutResult->Reset();
	}
	if (!Actor)
	{
		Test.AddError(TEXT("Cannot create the test component without an actor"));
		return nullptr;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("MonolithPCGActionTestComponent"));
	Params->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult Result = ExecuteAction(TEXT("create_component"), Params);
	if (!RequireSuccess(Test, TEXT("create_component"), Result))
	{
		return nullptr;
	}
	if (!Result.Result->TryGetStringField(TEXT("component_path"), OutComponentPath) || OutComponentPath.IsEmpty())
	{
		Test.AddError(TEXT("create_component did not return a canonical component_path"));
		return nullptr;
	}
	if (OutResult)
	{
		*OutResult = Result.Result;
	}

	UPCGComponent* Component = FindComponentExactOnActor(Actor, OutComponentPath);
	if (!Component)
	{
		Test.AddError(FString::Printf(
			TEXT("create_component returned an unresolved path: %s"),
			*OutComponentPath));
	}
	return Component;
}

UPCGGraph* CreateGraphAsset(FString& OutObjectPath, UPackage*& OutPackage)
{
	OutObjectPath.Reset();
	OutPackage = nullptr;
	const FString AssetName = TEXT("PCG_ComponentLifecycle_") +
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/Developers/MonolithTests/PCG/") + AssetName;
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

class FScopedGraphAssetFixture
{
public:
	FScopedGraphAssetFixture()
	{
		Graph = CreateGraphAsset(ObjectPath, Package);
	}

	~FScopedGraphAssetFixture()
	{
		DiscardGraphAsset(Graph, Package);
	}

	UPCGGraph* Graph = nullptr;
	UPackage* Package = nullptr;
	FString ObjectPath;
};

class FScopedBlueprintPCGTemplateFixture
{
public:
	FScopedBlueprintPCGTemplateFixture()
	{
		const FString AssetName = TEXT("BP_PCGTemplate_") +
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = TEXT("/Game/Developers/MonolithTests/PCG/") + AssetName;
		Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return;
		}
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("MonolithPCGComponentActionsTests")));
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return;
		}
		Node = Blueprint->SimpleConstructionScript->CreateNode(
			UPCGComponent::StaticClass(), FName(*ComponentName));
		if (!Node)
		{
			return;
		}
		Blueprint->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Template = Cast<UPCGComponent>(Node->ComponentTemplate);
#if WITH_EDITORONLY_DATA
		if (Template)
		{
			Template->bRegenerateInEditor = false;
		}
#endif
		ObjectPath = Blueprint->GetPathName();
		Package->SetDirtyFlag(false);
	}

	~FScopedBlueprintPCGTemplateFixture()
	{
		if (Blueprint)
		{
			FAssetRegistryModule::AssetDeleted(Blueprint);
			Blueprint->ClearFlags(RF_Public | RF_Standalone);
			Blueprint->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_AllowPackageLinkerMismatch);
			Blueprint->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	}

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	USCS_Node* Node = nullptr;
	UPCGComponent* Template = nullptr;
	FString ObjectPath;
	FString ComponentName = TEXT("LayoutGenerator");
};

TArray<TPair<FString, TSharedPtr<FJsonObject>>> BuildMutationGuardCases(
	const FString& ComponentPath,
	int32 OriginalSeed)
{
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> MutationCases;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		Params->SetStringField(
			TEXT("graph_asset_path"),
			TEXT("/Game/Developers/MonolithTests/PCG/PCG_MutationGuard.PCG_MutationGuard"));
		Params->SetBoolField(TEXT("save"), false);
		MutationCases.Emplace(TEXT("set_component_graph"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		Params->SetNumberField(TEXT("seed"), OriginalSeed + 1);
		Params->SetBoolField(TEXT("save"), false);
		MutationCases.Emplace(TEXT("set_component_settings"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		Params->SetBoolField(TEXT("force"), false);
		MutationCases.Emplace(TEXT("generate_component"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		MutationCases.Emplace(TEXT("refresh_component"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		MutationCases.Emplace(TEXT("cancel_component"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		Params->SetBoolField(TEXT("remove_components"), true);
		MutationCases.Emplace(TEXT("cleanup_component"), Params);
	}
	{
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		Values->SetNumberField(TEXT("ArbitraryParameter"), 1);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_path"), ComponentPath);
		Params->SetObjectField(TEXT("values"), Values);
		Params->SetBoolField(TEXT("save"), false);
		MutationCases.Emplace(TEXT("set_component_user_parameters"), Params);
	}
	return MutationCases;
}

bool ReadDoubleParameter(
	FAutomationTestBase& Test,
	const FInstancedPropertyBag& Bag,
	const FName Name,
	double& OutValue)
{
	const TValueOrError<double, EPropertyBagResult> Result = Bag.GetValueDouble(Name);
	if (!Result.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("Could not read double user parameter '%s'"), *Name.ToString()));
		return false;
	}
	OutValue = Result.GetValue();
	return true;
}

bool ReadInt32Parameter(
	FAutomationTestBase& Test,
	const FInstancedPropertyBag& Bag,
	const FName Name,
	int32& OutValue)
{
	const TValueOrError<int32, EPropertyBagResult> Result = Bag.GetValueInt32(Name);
	if (!Result.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("Could not read int32 user parameter '%s'"), *Name.ToString()));
		return false;
	}
	OutValue = Result.GetValue();
	return true;
}

bool ReadInt64Parameter(
	FAutomationTestBase& Test,
	const FInstancedPropertyBag& Bag,
	const FName Name,
	int64& OutValue)
{
	const TValueOrError<int64, EPropertyBagResult> Result = Bag.GetValueInt64(Name);
	if (!Result.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("Could not read int64 user parameter '%s'"), *Name.ToString()));
		return false;
	}
	OutValue = Result.GetValue();
	return true;
}
} // namespace MonolithPCGComponentActionsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGBlueprintComponentGraphAssignmentTest,
	"Monolith.PCG.Component.BlueprintTemplateGraphAssignment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGBlueprintComponentGraphAssignmentTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	FScopedGraphAssetFixture GraphFixture;
	FScopedBlueprintPCGTemplateFixture BlueprintFixture;
	if (!TestNotNull(TEXT("Blueprint fixture is created"), BlueprintFixture.Blueprint) ||
		!TestNotNull(TEXT("Blueprint PCG template is created"), BlueprintFixture.Template) ||
		!TestNotNull(TEXT("Graph fixture is created"), GraphFixture.Graph))
	{
		return false;
	}
	if (!TestNotNull(
		TEXT("Blueprint PCG template owns a graph instance"),
		BlueprintFixture.Template->GetGraphInstance()))
	{
		return false;
	}
	TestNull(
		TEXT("Blueprint PCG template starts without an assigned graph"),
		BlueprintFixture.Template->GetGraphInstance()->Graph.Get());

	auto BuildParams = [&]()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint_asset_path"), BlueprintFixture.ObjectPath);
		Params->SetStringField(TEXT("component_name"), BlueprintFixture.ComponentName);
		Params->SetStringField(TEXT("graph_asset_path"), GraphFixture.ObjectPath);
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	};

	TSharedPtr<FJsonObject> DryRunParams = BuildParams();
	const FMonolithActionResult DryRunResult =
		ExecuteAction(TEXT("set_blueprint_component_graph"), DryRunParams);
	if (!RequireSuccess(*this, TEXT("set_blueprint_component_graph dry run"), DryRunResult))
	{
		return false;
	}
	TestTrue(TEXT("Blueprint graph assignment defaults to dry-run"), DryRunResult.Result->GetBoolField(TEXT("dry_run")));
	TestTrue(TEXT("Dry-run reports the pending graph change"), DryRunResult.Result->GetBoolField(TEXT("would_change")));
	TestFalse(TEXT("Dry-run reports no committed mutation"), DryRunResult.Result->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("Dry-run does not prepare source control"), DryRunResult.Result->HasField(TEXT("source_control_prepare")));
	TestNull(
		TEXT("Dry-run leaves the template graph unchanged"),
		BlueprintFixture.Template->GetGraphInstance()->Graph.Get());

	TSharedPtr<FJsonObject> MissingConfirmParams = BuildParams();
	MissingConfirmParams->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult MissingConfirmResult =
		ExecuteAction(TEXT("set_blueprint_component_graph"), MissingConfirmParams);
	TestFalse(TEXT("Commit without confirm is rejected"), MissingConfirmResult.bSuccess);
	TestTrue(TEXT("Missing-confirm error names confirm"), MissingConfirmResult.ErrorMessage.Contains(TEXT("confirm")));
	TestNull(
		TEXT("Rejected commit leaves the template graph unchanged"),
		BlueprintFixture.Template->GetGraphInstance()->Graph.Get());

	TSharedPtr<FJsonObject> CommitParams = BuildParams();
	CommitParams->SetBoolField(TEXT("dry_run"), false);
	CommitParams->SetBoolField(TEXT("confirm"), true);
	const FMonolithActionResult CommitResult =
		ExecuteAction(TEXT("set_blueprint_component_graph"), CommitParams);
	if (!RequireSuccess(*this, TEXT("set_blueprint_component_graph commit"), CommitResult))
	{
		return false;
	}
	TestTrue(TEXT("Commit reports one exact change"), CommitResult.Result->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("save=false does not write the fixture package"), CommitResult.Result->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Commit reports successful Blueprint compilation"), CommitResult.Result->GetBoolField(TEXT("compile_succeeded")));

	UPCGComponent* ReadBackTemplate = nullptr;
	if (BlueprintFixture.Blueprint && BlueprintFixture.Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Candidate : BlueprintFixture.Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Candidate && Candidate->GetVariableName().ToString() == BlueprintFixture.ComponentName)
			{
				ReadBackTemplate = Cast<UPCGComponent>(Candidate->ComponentTemplate);
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("Compiled Blueprint retains the exact PCG template"), ReadBackTemplate) ||
		!TestNotNull(TEXT("Compiled template retains its graph instance"), ReadBackTemplate->GetGraphInstance()))
	{
		return false;
	}
	TestTrue(
		TEXT("Compiled template reads back the exact requested graph"),
		ReadBackTemplate->GetGraphInstance()->Graph.Get() == GraphFixture.Graph);

	TSharedPtr<FJsonObject> IdempotentParams = BuildParams();
	const FMonolithActionResult IdempotentResult =
		ExecuteAction(TEXT("set_blueprint_component_graph"), IdempotentParams);
	if (!RequireSuccess(*this, TEXT("set_blueprint_component_graph idempotent dry run"), IdempotentResult))
	{
		return false;
	}
	TestFalse(TEXT("Repeated dry-run reports no remaining change"), IdempotentResult.Result->GetBoolField(TEXT("would_change")));
	TestFalse(TEXT("Repeated dry-run stays side-effect-free"), IdempotentResult.Result->GetBoolField(TEXT("changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGComponentExactPathSettingsTest,
	"Monolith.PCG.Component.ExactPathAndSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGComponentExactPathSettingsTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor))
	{
		return false;
	}
	if (!AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}
	const int32 InitialInstanceComponentCount = Fixture.Actor->GetInstanceComponents().Num();

	TSharedPtr<FJsonObject> ShortPathParams = MakeShared<FJsonObject>();
	ShortPathParams->SetStringField(TEXT("actor_path"), Fixture.Actor->GetName());
	ShortPathParams->SetStringField(TEXT("component_name"), TEXT("RejectedShortPathComponent"));
	ShortPathParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult ShortPathResult = ExecuteAction(TEXT("create_component"), ShortPathParams);
	TestFalse(TEXT("create_component rejects a short actor name"), ShortPathResult.bSuccess);
	TestEqual(
		TEXT("Rejected short path does not create an instance component"),
		Fixture.Actor->GetInstanceComponents().Num(),
		InitialInstanceComponentCount);

	FString ComponentPath;
	TSharedPtr<FJsonObject> CreateResult;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath, &CreateResult);
	if (!TestNotNull(TEXT("Exact actor path creates a PCG component"), Component))
	{
		return false;
	}
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("create_component changed path"), CreateResult))
	{
		return false;
	}
	TestEqual(TEXT("Returned path is the component's exact path"), ComponentPath, Component->GetPathName());
	TestTrue(
		TEXT("Created component is persisted in the actor instance-component list"),
		Fixture.Actor->GetInstanceComponents().Contains(Component));
	TestTrue(TEXT("Created component is registered"), Component->IsRegistered());

	TSharedPtr<FJsonObject> ShortComponentPathParams = MakeShared<FJsonObject>();
	ShortComponentPathParams->SetStringField(TEXT("component_path"), Component->GetName());
	const FMonolithActionResult ShortComponentPathResult =
		ExecuteAction(TEXT("get_component"), ShortComponentPathParams);
	TestFalse(TEXT("get_component rejects a short component name"), ShortComponentPathResult.bSuccess);

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("component_path"), ComponentPath);
	const FMonolithActionResult GetResult = ExecuteAction(TEXT("get_component"), GetParams);
	if (!RequireSuccess(*this, TEXT("get_component"), GetResult))
	{
		return false;
	}
	TestEqual(
		TEXT("get_component preserves exact component identity"),
		GetResult.Result->GetStringField(TEXT("component_path")),
		ComponentPath);
	TestEqual(
		TEXT("get_component reports the exact owner actor"),
		GetResult.Result->GetStringField(TEXT("actor_path")),
		Fixture.Actor->GetPathName());
	const TSharedPtr<FJsonValue>* GenerationTaskId = GetResult.Result->Values.Find(TEXT("generation_task_id"));
	const TSharedPtr<FJsonValue>* CleanupTaskId = GetResult.Result->Values.Find(TEXT("cleanup_task_id"));
	TestTrue(
		TEXT("Generation task id is serialized as a JSON string even when invalid"),
		GenerationTaskId && GenerationTaskId->IsValid() && (*GenerationTaskId)->Type == EJson::String);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TestTrue(TEXT("UE 5.8 exposes cleanup task identity"),
		GetResult.Result->GetBoolField(TEXT("cleanup_task_id_supported")));
	TestTrue(
		TEXT("Cleanup task id is serialized as a JSON string even when invalid"),
		CleanupTaskId && CleanupTaskId->IsValid() && (*CleanupTaskId)->Type == EJson::String);
#else
	TestFalse(TEXT("UE 5.7 reports cleanup task identity as unsupported"),
		GetResult.Result->GetBoolField(TEXT("cleanup_task_id_supported")));
	TestNull(TEXT("UE 5.7 does not fabricate an unavailable cleanup task id"), CleanupTaskId);
#endif

	TSharedPtr<FJsonObject> InvalidOutputLimitParams = MakeShared<FJsonObject>();
	InvalidOutputLimitParams->SetStringField(TEXT("component_path"), ComponentPath);
	InvalidOutputLimitParams->SetNumberField(TEXT("output_limit"), 0);
	const FMonolithActionResult InvalidOutputLimit =
		ExecuteAction(TEXT("get_component_output"), InvalidOutputLimitParams);
	TestFalse(TEXT("get_component_output rejects output_limit below one"), InvalidOutputLimit.bSuccess);
	TestTrue(
		TEXT("Output limit error names the invalid field"),
		InvalidOutputLimit.ErrorMessage.Contains(TEXT("output_limit")));

	TSharedPtr<FJsonObject> BoundedOutputParams = MakeShared<FJsonObject>();
	BoundedOutputParams->SetStringField(TEXT("component_path"), ComponentPath);
	BoundedOutputParams->SetNumberField(TEXT("output_limit"), 1);
	BoundedOutputParams->SetNumberField(TEXT("tag_limit"), 1);
	BoundedOutputParams->SetNumberField(TEXT("resource_limit"), 1);
	BoundedOutputParams->SetNumberField(TEXT("managed_object_limit"), 1);
	const FMonolithActionResult BoundedOutput =
		ExecuteAction(TEXT("get_component_output"), BoundedOutputParams);
	if (!RequireSuccess(*this, TEXT("get_component_output bounded idle read"), BoundedOutput))
	{
		return false;
	}
	TestEqual(TEXT("Output response echoes the enforced output bound"),
		static_cast<int32>(BoundedOutput.Result->GetNumberField(TEXT("output_limit"))), 1);
	TestEqual(TEXT("Output response echoes the enforced per-item tag bound"),
		static_cast<int32>(BoundedOutput.Result->GetNumberField(TEXT("tag_limit"))), 1);
	TestEqual(TEXT("Output response echoes the enforced resource bound"),
		static_cast<int32>(BoundedOutput.Result->GetNumberField(TEXT("resource_limit"))), 1);

	TSharedPtr<FJsonObject> SettingsParams = MakeShared<FJsonObject>();
	SettingsParams->SetStringField(TEXT("component_path"), ComponentPath);
	SettingsParams->SetNumberField(TEXT("seed"), 1789);
	SettingsParams->SetBoolField(TEXT("activated"), false);
	SettingsParams->SetBoolField(TEXT("partitioned"), false);
	SettingsParams->SetStringField(TEXT("generation_trigger"), TEXT("on_demand"));
	SettingsParams->SetBoolField(TEXT("generate_on_drop_when_on_demand"), true);
	SettingsParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult SettingsResult = ExecuteAction(TEXT("set_component_settings"), SettingsParams);
	if (!RequireSuccess(*this, TEXT("set_component_settings"), SettingsResult))
	{
		return false;
	}
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("set_component_settings changed path"), SettingsResult.Result))
	{
		return false;
	}
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Settings edit preserves the exact live component"), Component))
	{
		return false;
	}
	TestEqual(TEXT("Seed is committed exactly"), Component->Seed, 1789);
	TestFalse(TEXT("Activated flag is committed exactly"), Component->bActivated);
	TestFalse(TEXT("Partitioned flag is committed exactly"), Component->bIsComponentPartitioned);
	TestEqual(
		TEXT("Generation trigger is committed exactly"),
		Component->GenerationTrigger,
		EPCGComponentGenerationTrigger::GenerateOnDemand);
	TestTrue(
		TEXT("Generate-on-drop is enabled for the on-demand trigger"),
		Component->bGenerateOnDropWhenTriggerOnDemand);

	for (const FString& RejectedTrigger : {FString(TEXT("on_load")), FString(TEXT("at_runtime"))})
	{
		TSharedPtr<FJsonObject> TriggerOnlyParams = MakeShared<FJsonObject>();
		TriggerOnlyParams->SetStringField(TEXT("component_path"), ComponentPath);
		TriggerOnlyParams->SetStringField(TEXT("generation_trigger"), RejectedTrigger);
		TriggerOnlyParams->SetBoolField(TEXT("save"), false);
		const FMonolithActionResult TriggerOnlyResult =
			ExecuteAction(TEXT("set_component_settings"), TriggerOnlyParams);
		TestFalse(
			*FString::Printf(TEXT("Existing generate-on-drop rejects trigger-only change to %s"), *RejectedTrigger),
			TriggerOnlyResult.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("Rejected %s batch identifies generate-on-drop incompatibility"), *RejectedTrigger),
			TriggerOnlyResult.ErrorMessage.Contains(TEXT("generate_on_drop_when_on_demand")));
		Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
		if (!TestNotNull(TEXT("Rejected trigger-only batch keeps the exact component"), Component))
		{
			return false;
		}
		TestEqual(
			*FString::Printf(TEXT("Rejected %s batch preserves on-demand trigger"), *RejectedTrigger),
			Component->GenerationTrigger,
			EPCGComponentGenerationTrigger::GenerateOnDemand);
		TestTrue(
			*FString::Printf(TEXT("Rejected %s batch preserves generate-on-drop"), *RejectedTrigger),
			Component->bGenerateOnDropWhenTriggerOnDemand);
	}

	TSharedPtr<FJsonObject> InvalidSettingsParams = MakeShared<FJsonObject>();
	InvalidSettingsParams->SetStringField(TEXT("component_path"), ComponentPath);
	InvalidSettingsParams->SetNumberField(TEXT("seed"), 9001);
	InvalidSettingsParams->SetStringField(TEXT("generation_trigger"), TEXT("not_a_trigger"));
	InvalidSettingsParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult InvalidSettingsResult =
		ExecuteAction(TEXT("set_component_settings"), InvalidSettingsParams);
	TestFalse(TEXT("Invalid component settings are rejected"), InvalidSettingsResult.bSuccess);
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Rejected settings keep the exact live component"), Component))
	{
		return false;
	}
	TestEqual(TEXT("A rejected settings batch is atomic"), Component->Seed, 1789);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGComponentUserParameterAtomicityTest,
	"Monolith.PCG.Component.UserParameters.Atomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGComponentUserParameterAtomicityTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor))
	{
		return false;
	}
	if (!AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}

	UPCGGraph* Graph = NewObject<UPCGGraph>(
		Component,
		UPCGGraph::StaticClass(),
		TEXT("MonolithPCGTransientParameterGraph"),
		RF_Transient);
	if (!TestNotNull(TEXT("Transient graph created"), Graph))
	{
		return false;
	}

	const FName DensityName(TEXT("Density"));
	const FName CountName(TEXT("Count"));
	const FName LargeCountName(TEXT("LargeCount"));
	TArray<FPropertyBagPropertyDesc> Descriptors;
	Descriptors.Emplace(DensityName, EPropertyBagPropertyType::Double);
	Descriptors.Emplace(CountName, EPropertyBagPropertyType::Int32);
	Descriptors.Emplace(LargeCountName, EPropertyBagPropertyType::Int64);
	if (!TestEqual(
			TEXT("Transient graph accepts the test user-parameter schema"),
			Graph->AddUserParameters(Descriptors),
			EPropertyBagAlterationResult::Success))
	{
		return false;
	}

	FInstancedPropertyBag* GraphBag = Graph->GetMutableUserParametersStruct_Unsafe();
	if (!TestNotNull(TEXT("Transient graph has a mutable parameter bag"), GraphBag))
	{
		return false;
	}
	TestEqual(TEXT("Density default is initialized"), GraphBag->SetValueDouble(DensityName, 0.25), EPropertyBagResult::Success);
	TestEqual(TEXT("Count default is initialized"), GraphBag->SetValueInt32(CountName, 7), EPropertyBagResult::Success);
	TestEqual(TEXT("LargeCount default is initialized"), GraphBag->SetValueInt64(LargeCountName, -17), EPropertyBagResult::Success);

#if WITH_EDITORONLY_DATA
	// SetGraphLocal normally schedules an editor refresh through
	// RefreshAfterGraphChanged. The user-parameter action deliberately rejects
	// non-idle components, so disable automatic editor regeneration before the
	// fixture-only graph assignment instead of waiting or manually ticking.
	Component->bRegenerateInEditor = false;
#endif
	Component->SetGraphLocal(Graph);
#if WITH_EDITOR
	if (!TestFalse(TEXT("Fixture graph assignment does not leave a refresh task"), Component->IsRefreshInProgress()))
	{
		return false;
	}
#endif
	if (!TestFalse(TEXT("Fixture graph assignment does not start generation"), Component->IsGenerating())
		|| !TestFalse(TEXT("Fixture graph assignment does not start cleanup"), Component->IsCleaningUp()))
	{
		return false;
	}
	UPCGGraphInstance* GraphInstance = nullptr;
	const FInstancedPropertyBag* InstanceBag = nullptr;
	const FPropertyBagPropertyDesc* DensityDesc = nullptr;
	const FPropertyBagPropertyDesc* CountDesc = nullptr;
	const FPropertyBagPropertyDesc* LargeCountDesc = nullptr;
	auto RefreshInstanceState = [&]() -> bool
	{
		Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
		if (!TestNotNull(TEXT("Exact live component can be re-resolved after an action"), Component))
		{
			return false;
		}
		GraphInstance = Component->GetGraphInstance();
		if (!TestNotNull(TEXT("Component owns a graph instance"), GraphInstance))
		{
			return false;
		}
		InstanceBag = GraphInstance->GetUserParametersStruct();
		if (!TestNotNull(TEXT("Graph instance exposes the inherited parameter bag"), InstanceBag))
		{
			return false;
		}
		DensityDesc = InstanceBag->FindPropertyDescByName(DensityName);
		CountDesc = InstanceBag->FindPropertyDescByName(CountName);
		LargeCountDesc = InstanceBag->FindPropertyDescByName(LargeCountName);
		if (!TestNotNull(TEXT("Density descriptor copied to instance"), DensityDesc)
			|| !TestNotNull(TEXT("Count descriptor copied to instance"), CountDesc)
			|| !TestNotNull(TEXT("LargeCount descriptor copied to instance"), LargeCountDesc))
		{
			return false;
		}
		return TestNotNull(TEXT("Density descriptor has a reflected property"), DensityDesc->CachedProperty)
			&& TestNotNull(TEXT("Count descriptor has a reflected property"), CountDesc->CachedProperty)
			&& TestNotNull(TEXT("LargeCount descriptor has a reflected property"), LargeCountDesc->CachedProperty);
	};
	if (!RefreshInstanceState())
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidValues = MakeShared<FJsonObject>();
	InvalidValues->SetNumberField(TEXT("Density"), 0.75);
	InvalidValues->SetStringField(TEXT("Count"), TEXT("not-an-int32"));
	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("component_path"), ComponentPath);
	InvalidParams->SetObjectField(TEXT("values"), InvalidValues);
	InvalidParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult InvalidResult =
		ExecuteAction(TEXT("set_component_user_parameters"), InvalidParams);
	TestFalse(TEXT("A mixed valid/invalid override batch is rejected"), InvalidResult.bSuccess);
	if (!RefreshInstanceState())
	{
		return false;
	}

	double Density = 0.0;
	int32 Count = 0;
	int64 LargeCount = 0;
	if (!ReadDoubleParameter(*this, *InstanceBag, DensityName, Density)
		|| !ReadInt32Parameter(*this, *InstanceBag, CountName, Count)
		|| !ReadInt64Parameter(*this, *InstanceBag, LargeCountName, LargeCount))
	{
		return false;
	}
	TestEqual(TEXT("Rejected batch leaves Density inherited"), Density, 0.25);
	TestEqual(TEXT("Rejected batch leaves Count inherited"), Count, 7);
	TestEqual(TEXT("Rejected batch leaves LargeCount inherited"), LargeCount, static_cast<int64>(-17));
	TestFalse(TEXT("Rejected batch does not mark Density overridden"), GraphInstance->IsPropertyOverridden(DensityDesc->CachedProperty));
	TestFalse(TEXT("Rejected batch does not mark Count overridden"), GraphInstance->IsPropertyOverridden(CountDesc->CachedProperty));
	TestFalse(TEXT("Rejected batch does not mark LargeCount overridden"), GraphInstance->IsPropertyOverridden(LargeCountDesc->CachedProperty));
	if (const TSharedPtr<FJsonObject> InvalidErrorData =
			MonolithPCGResultUtils::GetErrorDataObject(InvalidResult))
	{
		TestFalse(
			TEXT("Rejected validation path does not prepare source control"),
			InvalidErrorData->HasField(TEXT("source_control_prepare")));
	}

	for (const FString& InvalidInt64 : {FString(TEXT("01")), FString(TEXT("9223372036854775808"))})
	{
		TSharedPtr<FJsonObject> InvalidInt64Values = MakeShared<FJsonObject>();
		InvalidInt64Values->SetStringField(LargeCountName.ToString(), InvalidInt64);
		TSharedPtr<FJsonObject> InvalidInt64Params = MakeShared<FJsonObject>();
		InvalidInt64Params->SetStringField(TEXT("component_path"), ComponentPath);
		InvalidInt64Params->SetObjectField(TEXT("values"), InvalidInt64Values);
		InvalidInt64Params->SetBoolField(TEXT("save"), false);
		const FMonolithActionResult InvalidInt64Result =
			ExecuteAction(TEXT("set_component_user_parameters"), InvalidInt64Params);
		TestFalse(*FString::Printf(TEXT("Non-canonical/out-of-range int64 '%s' is rejected"), *InvalidInt64),
			InvalidInt64Result.bSuccess);
		if (!RefreshInstanceState()
			|| !ReadInt64Parameter(*this, *InstanceBag, LargeCountName, LargeCount))
		{
			return false;
		}
		TestEqual(TEXT("Rejected int64 leaves the live value unchanged"), LargeCount, static_cast<int64>(-17));
		TestFalse(TEXT("Rejected int64 does not create an override"),
			GraphInstance->IsPropertyOverridden(LargeCountDesc->CachedProperty));
	}

	TSharedPtr<FJsonObject> ValidValues = MakeShared<FJsonObject>();
	ValidValues->SetNumberField(TEXT("Density"), 0.75);
	ValidValues->SetNumberField(TEXT("Count"), 11);
	ValidValues->SetStringField(TEXT("LargeCount"), TEXT("9223372036854775807"));
	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("component_path"), ComponentPath);
	DryRunParams->SetObjectField(TEXT("values"), ValidValues);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	DryRunParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult DryRunResult =
		ExecuteAction(TEXT("set_component_user_parameters"), DryRunParams);
	if (!RequireSuccess(*this, TEXT("set_component_user_parameters dry run"), DryRunResult))
	{
		return false;
	}
	TestTrue(TEXT("Dry run reports dry_run=true"), DryRunResult.Result->GetBoolField(TEXT("dry_run")));
	TestFalse(
		TEXT("Dry run does not prepare source control"),
		DryRunResult.Result->HasField(TEXT("source_control_prepare")));

	TSharedPtr<FJsonObject> ValidParams = MakeShared<FJsonObject>();
	ValidParams->SetStringField(TEXT("component_path"), ComponentPath);
	ValidParams->SetObjectField(TEXT("values"), ValidValues);
	ValidParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult ValidResult =
		ExecuteAction(TEXT("set_component_user_parameters"), ValidParams);
	if (!RequireSuccess(*this, TEXT("set_component_user_parameters"), ValidResult))
	{
		return false;
	}
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("User-parameter changed path"), ValidResult.Result))
	{
		return false;
	}
	if (!RefreshInstanceState())
	{
		return false;
	}
	if (!ReadDoubleParameter(*this, *InstanceBag, DensityName, Density)
		|| !ReadInt32Parameter(*this, *InstanceBag, CountName, Count)
		|| !ReadInt64Parameter(*this, *InstanceBag, LargeCountName, LargeCount))
	{
		return false;
	}
	TestEqual(TEXT("Valid batch commits Density"), Density, 0.75);
	TestEqual(TEXT("Valid batch commits Count"), Count, 11);
	TestEqual(TEXT("Valid batch commits full-range LargeCount"), LargeCount, MAX_int64);
	TestTrue(TEXT("Valid batch marks Density overridden"), GraphInstance->IsPropertyOverridden(DensityDesc->CachedProperty));
	TestTrue(TEXT("Valid batch marks Count overridden"), GraphInstance->IsPropertyOverridden(CountDesc->CachedProperty));
	TestTrue(TEXT("Valid batch marks LargeCount overridden"), GraphInstance->IsPropertyOverridden(LargeCountDesc->CachedProperty));
	const TArray<TSharedPtr<FJsonValue>>* ChangeRows = nullptr;
	if (TestTrue(TEXT("Valid result reports per-parameter changes"),
		ValidResult.Result->TryGetArrayField(TEXT("changes"), ChangeRows)) && ChangeRows)
	{
		bool bFoundExactInt64 = false;
		for (const TSharedPtr<FJsonValue>& ChangeValue : *ChangeRows)
		{
			const TSharedPtr<FJsonObject> Change = ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
			if (Change.IsValid() && Change->GetStringField(TEXT("name")) == LargeCountName.ToString())
			{
				bFoundExactInt64 = Change->GetStringField(TEXT("after_serialized_value")) ==
					TEXT("9223372036854775807");
				break;
			}
		}
		TestTrue(TEXT("Valid result preserves the exact decimal int64 readback"), bFoundExactInt64);
	}

	TArray<TSharedPtr<FJsonValue>> ResetValues;
	ResetValues.Add(MakeShared<FJsonValueString>(DensityName.ToString()));
	TSharedPtr<FJsonObject> ResetParams = MakeShared<FJsonObject>();
	ResetParams->SetStringField(TEXT("component_path"), ComponentPath);
	ResetParams->SetArrayField(TEXT("reset"), ResetValues);
	ResetParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult ResetResult =
		ExecuteAction(TEXT("set_component_user_parameters"), ResetParams);
	if (!RequireSuccess(*this, TEXT("set_component_user_parameters reset"), ResetResult))
	{
		return false;
	}
	if (!RefreshInstanceState())
	{
		return false;
	}
	if (!ReadDoubleParameter(*this, *InstanceBag, DensityName, Density))
	{
		return false;
	}
	TestEqual(TEXT("Reset restores inherited Density"), Density, 0.25);
	TestFalse(TEXT("Reset removes Density override identity"), GraphInstance->IsPropertyOverridden(DensityDesc->CachedProperty));
	TestTrue(TEXT("Reset leaves unrelated Count override intact"), GraphInstance->IsPropertyOverridden(CountDesc->CachedProperty));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGComponentLocalMutationGuardTest,
	"Monolith.PCG.Component.LocalMutationGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGComponentLocalMutationGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor))
	{
		return false;
	}
	if (!AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
	Component->MarkAsLocalComponent();
	if (!TestTrue(TEXT("Fixture component is marked as an engine-owned local component"), Component->IsLocalComponent()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("component_path"), ComponentPath);
	const FMonolithActionResult GetResult = ExecuteAction(TEXT("get_component"), ReadParams);
	if (!RequireSuccess(*this, TEXT("get_component on a local component"), GetResult))
	{
		return false;
	}
	TestEqual(
		TEXT("Local component read preserves exact identity"),
		GetResult.Result->GetStringField(TEXT("component_path")),
		ComponentPath);

	const int32 OriginalSeed = Component->Seed;
	const bool bOriginalActivated = Component->bActivated;
	const bool bOriginalPartitioned = Component->IsPartitioned();
	const EPCGComponentGenerationTrigger OriginalTrigger = Component->GenerationTrigger;
	UPCGGraph* const OriginalGraph = Component->GetGraph();
	const int32 OriginalInstanceComponentCount = Fixture.Actor->GetInstanceComponents().Num();

	const TArray<TPair<FString, TSharedPtr<FJsonObject>>> MutationCases =
		BuildMutationGuardCases(ComponentPath, OriginalSeed);

	TestEqual(TEXT("Local guard covers every non-create mutation action"), MutationCases.Num(), 7);
	for (const TPair<FString, TSharedPtr<FJsonObject>>& MutationCase : MutationCases)
	{
		const FString& Action = MutationCase.Key;
		const FMonolithActionResult Result = ExecuteAction(Action, MutationCase.Value);
		TestFalse(
			*FString::Printf(TEXT("pcg.%s rejects an engine-owned local component"), *Action),
			Result.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("pcg.%s reports the local-component ownership guard"), *Action),
			Result.ErrorMessage.Contains(TEXT("local"), ESearchCase::IgnoreCase));

		Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
		if (!TestNotNull(
			*FString::Printf(TEXT("pcg.%s preserves exact component identity"), *Action),
			Component))
		{
			return false;
		}
		TestTrue(
			*FString::Printf(TEXT("pcg.%s preserves local ownership"), *Action),
			Component->IsLocalComponent());
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves seed"), *Action), Component->Seed, OriginalSeed);
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves activation"), *Action), Component->bActivated, bOriginalActivated);
		TestEqual(
			*FString::Printf(TEXT("pcg.%s preserves partition state"), *Action),
			Component->IsPartitioned(),
			bOriginalPartitioned);
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves trigger"), *Action), Component->GenerationTrigger, OriginalTrigger);
		TestTrue(
			*FString::Printf(TEXT("pcg.%s preserves graph"), *Action),
			Component->GetGraph() == OriginalGraph);
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule generation"), *Action), Component->IsGenerating());
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule cleanup"), *Action), Component->IsCleaningUp());
#if WITH_EDITOR
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule refresh"), *Action), Component->IsRefreshInProgress());
#endif
		TestEqual(
			*FString::Printf(TEXT("pcg.%s does not alter actor component membership"), *Action),
			Fixture.Actor->GetInstanceComponents().Num(),
			OriginalInstanceComponentCount);
	}

	TSharedPtr<FJsonObject> OutputParams = MakeShared<FJsonObject>();
	OutputParams->SetStringField(TEXT("component_path"), ComponentPath);
	OutputParams->SetBoolField(TEXT("include_managed_resources"), false);
	const FMonolithActionResult OutputResult =
		ExecuteAction(TEXT("get_component_output"), OutputParams);
	if (!RequireSuccess(*this, TEXT("get_component_output on a local component"), OutputResult))
	{
		return false;
	}
	TestEqual(
		TEXT("Output read preserves exact local component identity"),
		OutputResult.Result->GetStringField(TEXT("component_path")),
		ComponentPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGPartitionActorCreateGuardTest,
	"Monolith.PCG.Component.PartitionActorCreateGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGPartitionActorCreateGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(
		World,
		APCGPartitionActor::StaticClass(),
		TEXT("MonolithPCGPartitionActorCreateGuard"));
	APCGPartitionActor* PartitionActor = Cast<APCGPartitionActor>(Fixture.Actor);
	if (!TestNotNull(TEXT("Transient PCG partition actor spawned"), PartitionActor))
	{
		return false;
	}

	TArray<UPCGComponent*> BeforeComponents;
	PartitionActor->GetComponents(BeforeComponents);
	const int32 BeforeInstanceComponentCount = PartitionActor->GetInstanceComponents().Num();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), PartitionActor->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("RejectedPartitionWorkerComponent"));
	Params->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult Result = ExecuteAction(TEXT("create_component"), Params);
	TestFalse(TEXT("create_component rejects an engine-owned PCG partition actor"), Result.bSuccess);
	TestTrue(
		TEXT("Partition-actor rejection reports the ownership boundary"),
		Result.ErrorMessage.Contains(TEXT("partition"), ESearchCase::IgnoreCase));

	TArray<UPCGComponent*> AfterComponents;
	PartitionActor->GetComponents(AfterComponents);
	TestEqual(
		TEXT("Rejected create leaves the partition actor PCG-component count unchanged"),
		AfterComponents.Num(),
		BeforeComponents.Num());
	TestEqual(
		TEXT("Rejected create leaves partition actor instance-component membership unchanged"),
		PartitionActor->GetInstanceComponents().Num(),
		BeforeInstanceComponentCount);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGPartitionActorOwnedMutationGuardTest,
	"Monolith.PCG.Component.PartitionActorOwnedMutationGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGPartitionActorOwnedMutationGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(
		World,
		APCGPartitionActor::StaticClass(),
		TEXT("MonolithPCGPartitionActorMutationGuard"));
	APCGPartitionActor* PartitionActor = Cast<APCGPartitionActor>(Fixture.Actor);
	if (!TestNotNull(TEXT("Transient PCG partition actor spawned"), PartitionActor))
	{
		return false;
	}

	UPCGComponent* Component = NewObject<UPCGComponent>(
		PartitionActor,
		UPCGComponent::StaticClass(),
		MakeUniqueObjectName(
			PartitionActor,
			UPCGComponent::StaticClass(),
			FName(TEXT("MonolithPCGNonLocalPartitionWorker"))),
		RF_Transient | RF_Transactional);
	if (!TestNotNull(TEXT("Non-local partition-worker component created"), Component))
	{
		return false;
	}
	PartitionActor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	if (!TestTrue(TEXT("Partition-worker component is registered"), Component->IsRegistered())
		|| !TestFalse(TEXT("Partition-worker fixture is deliberately not marked local"), Component->IsLocalComponent()))
	{
		return false;
	}

	const FString ComponentPath = Component->GetPathName();
	const int32 OriginalSeed = Component->Seed;
	const bool bOriginalActivated = Component->bActivated;
	const bool bOriginalPartitioned = Component->IsPartitioned();
	const EPCGComponentGenerationTrigger OriginalTrigger = Component->GenerationTrigger;
	UPCGGraph* const OriginalGraph = Component->GetGraph();
	const int32 OriginalInstanceComponentCount = PartitionActor->GetInstanceComponents().Num();
	const TArray<TPair<FString, TSharedPtr<FJsonObject>>> MutationCases =
		BuildMutationGuardCases(ComponentPath, OriginalSeed);

	TestEqual(TEXT("Partition-actor ownership guard covers every non-create mutation action"), MutationCases.Num(), 7);
	for (const TPair<FString, TSharedPtr<FJsonObject>>& MutationCase : MutationCases)
	{
		const FString& Action = MutationCase.Key;
		const FMonolithActionResult Result = ExecuteAction(Action, MutationCase.Value);
		TestFalse(
			*FString::Printf(TEXT("pcg.%s rejects every partition-actor-owned component"), *Action),
			Result.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("pcg.%s reports the partition-actor ownership boundary"), *Action),
			Result.ErrorMessage.Contains(TEXT("partition"), ESearchCase::IgnoreCase));
		if (const TSharedPtr<FJsonObject> ResultErrorData =
				MonolithPCGResultUtils::GetErrorDataObject(Result))
		{
			TestTrue(
				*FString::Printf(TEXT("pcg.%s marks the owner as a partition actor"), *Action),
				ResultErrorData->GetBoolField(TEXT("partition_actor_owned")));
		}

		Component = FindComponentExactOnActor(PartitionActor, ComponentPath);
		if (!TestNotNull(
			*FString::Printf(TEXT("pcg.%s preserves exact component identity"), *Action),
			Component))
		{
			return false;
		}
		TestFalse(
			*FString::Printf(TEXT("pcg.%s does not need a local flag to enforce ownership"), *Action),
			Component->IsLocalComponent());
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves seed"), *Action), Component->Seed, OriginalSeed);
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves activation"), *Action), Component->bActivated, bOriginalActivated);
		TestEqual(
			*FString::Printf(TEXT("pcg.%s preserves partition state"), *Action),
			Component->IsPartitioned(),
			bOriginalPartitioned);
		TestEqual(*FString::Printf(TEXT("pcg.%s preserves trigger"), *Action), Component->GenerationTrigger, OriginalTrigger);
		TestTrue(
			*FString::Printf(TEXT("pcg.%s preserves graph"), *Action),
			Component->GetGraph() == OriginalGraph);
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule generation"), *Action), Component->IsGenerating());
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule cleanup"), *Action), Component->IsCleaningUp());
#if WITH_EDITOR
		TestFalse(*FString::Printf(TEXT("pcg.%s does not schedule refresh"), *Action), Component->IsRefreshInProgress());
#endif
		TestEqual(
			*FString::Printf(TEXT("pcg.%s preserves actor component membership"), *Action),
			PartitionActor->GetInstanceComponents().Num(),
			OriginalInstanceComponentCount);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGeneratedNoOpSaveGuardTest,
	"Monolith.PCG.Component.GeneratedNoOpSaveGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGeneratedNoOpSaveGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedGraphAssetFixture GraphFixture;
	if (!TestNotNull(TEXT("In-memory graph asset created"), GraphFixture.Graph))
	{
		return false;
	}
	FScopedActorFixture ActorFixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), ActorFixture.Actor)
		|| !AddValidBoundsRoot(*this, ActorFixture.Actor))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, ActorFixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	Component->bRegenerateInEditor = false;
#endif
	TSharedPtr<FJsonObject> InitialGraphParams = MakeShared<FJsonObject>();
	InitialGraphParams->SetStringField(TEXT("component_path"), ComponentPath);
	InitialGraphParams->SetStringField(TEXT("graph_asset_path"), GraphFixture.ObjectPath);
	InitialGraphParams->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult InitialGraphResult =
		ExecuteAction(TEXT("set_component_graph"), InitialGraphParams);
	if (!RequireSuccess(*this, TEXT("Initial set_component_graph"), InitialGraphResult)
		|| !RequireTransientSourceControlPrepare(
			*this, TEXT("set_component_graph changed path"), InitialGraphResult.Result))
	{
		return false;
	}
	Component = FindComponentExactOnActor(ActorFixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Graph fixture preserves the exact component"), Component)
		|| !TestTrue(TEXT("Fixture graph assignment commits"), Component->GetGraph() == GraphFixture.Graph))
	{
		return false;
	}
	Component->bGenerated = true;

	auto VerifyNoOpSaveContract = [this, Component](
		const FString& Action,
		const FString& Context,
		const TSharedPtr<FJsonObject>& Params) -> bool
	{
		Params->SetBoolField(TEXT("save"), true);
		const FMonolithActionResult SavedResult = ExecuteAction(Action, Params);
		TestFalse(*FString::Printf(TEXT("%s save=true rejects generated state"), *Context), SavedResult.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("%s save=true reports cleanup requirement"), *Context),
			SavedResult.ErrorMessage.Contains(TEXT("generated"), ESearchCase::IgnoreCase)
				&& SavedResult.ErrorMessage.Contains(TEXT("cleanup"), ESearchCase::IgnoreCase));

		Params->SetBoolField(TEXT("save"), false);
		const FMonolithActionResult UnsavedResult = ExecuteAction(Action, Params);
		if (!RequireSuccess(*this, Context + TEXT(" save=false"), UnsavedResult))
		{
			return false;
		}
		TestFalse(
			*FString::Printf(TEXT("%s save=false does not prepare source control"), *Context),
			UnsavedResult.Result->HasField(TEXT("source_control_prepare")));
		TestFalse(
			*FString::Printf(TEXT("%s save=false remains a true no-op"), *Context),
			UnsavedResult.Result->GetBoolField(TEXT("changed")));
		TestTrue(
			*FString::Printf(TEXT("%s preserves generated state"), *Context),
			Component->bGenerated);
		return true;
	};

	TSharedPtr<FJsonObject> GraphParams = MakeShared<FJsonObject>();
	GraphParams->SetStringField(TEXT("component_path"), ComponentPath);
	GraphParams->SetStringField(TEXT("graph_asset_path"), GraphFixture.ObjectPath);
	if (!VerifyNoOpSaveContract(TEXT("set_component_graph"), TEXT("Same-graph no-op"), GraphParams))
	{
		return false;
	}

	TSharedPtr<FJsonObject> SettingsParams = MakeShared<FJsonObject>();
	SettingsParams->SetStringField(TEXT("component_path"), ComponentPath);
	SettingsParams->SetNumberField(TEXT("seed"), Component->Seed);
	if (!VerifyNoOpSaveContract(TEXT("set_component_settings"), TEXT("Same-settings no-op"), SettingsParams))
	{
		return false;
	}

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("actor_path"), ActorFixture.Actor->GetPathName());
	CreateParams->SetStringField(TEXT("component_name"), Component->GetName());
	CreateParams->SetStringField(TEXT("existing_policy"), TEXT("return_existing"));
	CreateParams->SetStringField(TEXT("graph_asset_path"), GraphFixture.ObjectPath);
	CreateParams->SetNumberField(TEXT("seed"), Component->Seed);
	CreateParams->SetBoolField(TEXT("activated"), Component->bActivated);
	CreateParams->SetBoolField(TEXT("partitioned"), Component->IsPartitioned());
	CreateParams->SetStringField(TEXT("generation_trigger"), TEXT("on_demand"));
	CreateParams->SetBoolField(
		TEXT("generate_on_drop_when_on_demand"),
		Component->bGenerateOnDropWhenTriggerOnDemand);
	if (!VerifyNoOpSaveContract(TEXT("create_component"), TEXT("Return-existing no-op"), CreateParams))
	{
		return false;
	}

	Component->bGenerated = false;
	Component->SetGraphLocal(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGRollbackFailurePreservesDirtyStateTest,
	"Monolith.PCG.Component.RollbackFailurePreservesDirtyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGRollbackFailurePreservesDirtyStateTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor)
		|| !AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	Component->bRegenerateInEditor = false;
#endif
	UPackage* ComponentPackage = Component->GetPackage();
	UPackage* ActorPackage = Fixture.Actor->GetPackage();
	UPackage* LevelPackage = Fixture.Actor->GetLevel() ? Fixture.Actor->GetLevel()->GetOutermost() : nullptr;
	if (!TestNotNull(TEXT("Component package is available"), ComponentPackage)
		|| !TestNotNull(TEXT("Actor package is available"), ActorPackage)
		|| !TestNotNull(TEXT("Level package is available"), LevelPackage))
	{
		return false;
	}
	ComponentPackage->SetDirtyFlag(false);
	if (ActorPackage != ComponentPackage)
	{
		ActorPackage->SetDirtyFlag(false);
	}
	if (LevelPackage != ComponentPackage && LevelPackage != ActorPackage)
	{
		LevelPackage->SetDirtyFlag(false);
	}

	const int32 OriginalSeed = Component->Seed;
	const bool bOriginalActivated = Component->bActivated;
	int32 SeedPropertyEventCount = 0;
	const FDelegateHandle PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[Component, &SeedPropertyEventCount](UObject* Object, FPropertyChangedEvent& Event)
		{
			if (Object == Component
				&& Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UPCGComponent, Seed))
			{
				++SeedPropertyEventCount;
				// Simulate an ownership transition after the first committed field. The
				// next field and the rollback are both rejected by CanEditChange, forcing
				// the production incomplete-rollback path without a test-only hook.
				Component->MarkAsLocalComponent();
			}
		});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("component_path"), ComponentPath);
	Params->SetNumberField(TEXT("seed"), OriginalSeed + 1);
	Params->SetBoolField(TEXT("activated"), !bOriginalActivated);
	Params->SetBoolField(TEXT("save"), false);
	const FMonolithActionResult Result = ExecuteAction(TEXT("set_component_settings"), Params);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);

	TestFalse(TEXT("Injected mid-commit ownership transition fails the settings mutation"), Result.bSuccess);
	TestTrue(TEXT("Seed property event reached the injected transition"), SeedPropertyEventCount > 0);
	TestTrue(
		TEXT("Failure explicitly reports incomplete rollback"),
		Result.ErrorMessage.Contains(TEXT("rollback_complete=false"), ESearchCase::IgnoreCase));
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("Incomplete rollback path"),
			MonolithPCGResultUtils::GetErrorDataObject(Result)))
	{
		return false;
	}

	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Partially mutated component remains exactly resolvable"), Component))
	{
		return false;
	}
	TestTrue(TEXT("Injected ownership transition remains visible"), Component->IsLocalComponent());
	TestEqual(TEXT("First property remains partially committed"), Component->Seed, OriginalSeed + 1);
	TestEqual(TEXT("Second property was rejected before commit"), Component->bActivated, bOriginalActivated);
	TestTrue(TEXT("Incomplete rollback leaves the component package dirty"), ComponentPackage->IsDirty());
	TestTrue(TEXT("Incomplete rollback leaves the actor package dirty"), ActorPackage->IsDirty());
	TestTrue(TEXT("Incomplete rollback leaves the owning level package dirty"), LevelPackage->IsDirty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGComponentSaveFailureAtomicityTest,
	"Monolith.PCG.Component.SaveFailureAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGComponentSaveFailureAtomicityTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor) ||
		!AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}
	FScopedGraphAssetFixture OriginalGraphFixture;
	FScopedGraphAssetFixture ReplacementGraphFixture;
	if (!TestNotNull(TEXT("Original graph fixture exists"), OriginalGraphFixture.Graph) ||
		!TestNotNull(TEXT("Replacement graph fixture exists"), ReplacementGraphFixture.Graph))
	{
		return false;
	}

	const FName CountName(TEXT("RollbackCount"));
	TArray<FPropertyBagPropertyDesc> Descriptors;
	Descriptors.Emplace(CountName, EPropertyBagPropertyType::Int32);
	if (!TestEqual(
			TEXT("Original graph accepts the rollback parameter schema"),
			OriginalGraphFixture.Graph->AddUserParameters(Descriptors),
			EPropertyBagAlterationResult::Success))
	{
		return false;
	}
	FInstancedPropertyBag* OriginalGraphBag =
		OriginalGraphFixture.Graph->GetMutableUserParametersStruct_Unsafe();
	if (!TestNotNull(TEXT("Original graph exposes its parameter bag"), OriginalGraphBag) ||
		!TestEqual(
			TEXT("RollbackCount default is initialized"),
			OriginalGraphBag->SetValueInt32(CountName, 7),
			EPropertyBagResult::Success))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	Component->bRegenerateInEditor = false;
#endif
	Component->SetGraphLocal(OriginalGraphFixture.Graph);
	if (!TestTrue(
			TEXT("Fixture assigns the original graph"),
			Component->GetGraphInstance() &&
				Component->GetGraphInstance()->Graph.Get() == OriginalGraphFixture.Graph))
	{
		return false;
	}

	UPackage* ActorPackage = Fixture.Actor->GetPackage();
	UPackage* LevelPackage =
		Fixture.Actor->GetLevel() ? Fixture.Actor->GetLevel()->GetOutermost() : nullptr;
	if (!TestNotNull(TEXT("Actor package is available"), ActorPackage) ||
		!TestNotNull(TEXT("Level package is available"), LevelPackage))
	{
		return false;
	}
	auto MarkFixtureClean = [&]()
	{
		ActorPackage->SetDirtyFlag(false);
		if (LevelPackage != ActorPackage)
		{
			LevelPackage->SetDirtyFlag(false);
		}
	};
	auto TestFixtureClean = [&](const FString& Context)
	{
		TestFalse(*(Context + TEXT(" restores the actor package dirty state")), ActorPackage->IsDirty());
		TestFalse(*(Context + TEXT(" restores the level package dirty state")), LevelPackage->IsDirty());
	};
	auto ExecuteInjectedSaveFailure = [&](const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
#if WITH_DEV_AUTOMATION_TESTS
		UE::MonolithPCG::Private::ConfigureComponentLevelSaveTestFault(
			Fixture.Actor->GetPathName());
#endif
		return ExecuteAction(Action, Params);
	};
	ON_SCOPE_EXIT
	{
#if WITH_DEV_AUTOMATION_TESTS
		UE::MonolithPCG::Private::ResetComponentLevelSaveTestFault();
#endif
	};
	auto RequireCompleteRollback = [&](const FString& Context, const FMonolithActionResult& Result)
	{
		TestFalse(*(Context + TEXT(" reports the injected save failure")), Result.bSuccess);
		TestTrue(
			*(Context + TEXT(" reports complete rollback")),
			Result.ErrorMessage.Contains(TEXT("rollback_complete=true"), ESearchCase::CaseSensitive));
		const TSharedPtr<FJsonObject> ErrorData =
			MonolithPCGResultUtils::GetErrorDataObject(Result);
		bool bMutationAttempted = false;
		bool bRollbackComplete = false;
		bool bChanged = true;
		TestTrue(
			*(Context + TEXT(" returns structured mutation_attempted")),
			ErrorData.IsValid() &&
				ErrorData->TryGetBoolField(TEXT("mutation_attempted"), bMutationAttempted) &&
				bMutationAttempted);
		TestTrue(
			*(Context + TEXT(" returns structured rollback_complete")),
			ErrorData.IsValid() &&
				ErrorData->TryGetBoolField(TEXT("rollback_complete"), bRollbackComplete) &&
				bRollbackComplete);
		TestTrue(
			*(Context + TEXT(" reports no remaining live change")),
			ErrorData.IsValid() &&
				ErrorData->TryGetBoolField(TEXT("changed"), bChanged) &&
				!bChanged);
		return RequireTransientSourceControlPrepare(*this, Context, ErrorData);
	};

	MarkFixtureClean();
	const int32 OriginalInstanceComponentCount =
		Fixture.Actor->GetInstanceComponents().Num();
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("actor_path"), Fixture.Actor->GetPathName());
	CreateParams->SetStringField(
		TEXT("component_name"),
		TEXT("MonolithPCGSaveFailureCreatedComponent"));
	CreateParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult CreateFailure =
		ExecuteInjectedSaveFailure(TEXT("create_component"), CreateParams);
	if (!RequireCompleteRollback(TEXT("create_component save failure"), CreateFailure))
	{
		return false;
	}
	UPCGComponent* CreatedComponentRemnant = nullptr;
	for (UActorComponent* InstanceComponent : Fixture.Actor->GetInstanceComponents())
	{
		if (InstanceComponent &&
			InstanceComponent->GetFName() == TEXT("MonolithPCGSaveFailureCreatedComponent"))
		{
			CreatedComponentRemnant = Cast<UPCGComponent>(InstanceComponent);
			break;
		}
	}
	TestNull(
		TEXT("Create rollback removes the new exact actor component"),
		CreatedComponentRemnant);
	TestEqual(
		TEXT("Create rollback restores actor instance-component membership"),
		Fixture.Actor->GetInstanceComponents().Num(),
		OriginalInstanceComponentCount);
	TestFixtureClean(TEXT("create_component save failure"));

	MarkFixtureClean();
	TSharedPtr<FJsonObject> GraphParams = MakeShared<FJsonObject>();
	GraphParams->SetStringField(TEXT("component_path"), ComponentPath);
	GraphParams->SetStringField(
		TEXT("graph_asset_path"),
		ReplacementGraphFixture.ObjectPath);
	GraphParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult GraphFailure =
		ExecuteInjectedSaveFailure(TEXT("set_component_graph"), GraphParams);
	if (!RequireCompleteRollback(TEXT("set_component_graph save failure"), GraphFailure))
	{
		return false;
	}
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Graph rollback preserves exact component identity"), Component))
	{
		return false;
	}
	TestTrue(
		TEXT("Graph rollback restores the previous graph"),
		Component->GetGraphInstance() &&
			Component->GetGraphInstance()->Graph.Get() == OriginalGraphFixture.Graph);
	TestFixtureClean(TEXT("set_component_graph save failure"));

	MarkFixtureClean();
	const int32 OriginalSeed = Component->Seed;
	TSharedPtr<FJsonObject> SettingsParams = MakeShared<FJsonObject>();
	SettingsParams->SetStringField(TEXT("component_path"), ComponentPath);
	SettingsParams->SetNumberField(TEXT("seed"), OriginalSeed + 101);
	SettingsParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult SettingsFailure =
		ExecuteInjectedSaveFailure(TEXT("set_component_settings"), SettingsParams);
	if (!RequireCompleteRollback(TEXT("set_component_settings save failure"), SettingsFailure))
	{
		return false;
	}
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Settings rollback preserves exact component identity"), Component))
	{
		return false;
	}
	TestEqual(TEXT("Settings rollback restores the original seed"), Component->Seed, OriginalSeed);
	TestFixtureClean(TEXT("set_component_settings save failure"));

	MarkFixtureClean();
	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetNumberField(TEXT("RollbackCount"), 99);
	TSharedPtr<FJsonObject> UserParameterParams = MakeShared<FJsonObject>();
	UserParameterParams->SetStringField(TEXT("component_path"), ComponentPath);
	UserParameterParams->SetObjectField(TEXT("values"), Values);
	UserParameterParams->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult UserParameterFailure =
		ExecuteInjectedSaveFailure(
			TEXT("set_component_user_parameters"),
			UserParameterParams);
	if (!RequireCompleteRollback(
			TEXT("set_component_user_parameters save failure"),
			UserParameterFailure))
	{
		return false;
	}
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	UPCGGraphInstance* RestoredInstance =
		Component ? Component->GetGraphInstance() : nullptr;
	const FInstancedPropertyBag* RestoredBag =
		RestoredInstance ? RestoredInstance->GetUserParametersStruct() : nullptr;
	const FPropertyBagPropertyDesc* RestoredDesc =
		RestoredBag ? RestoredBag->FindPropertyDescByName(CountName) : nullptr;
	int32 RestoredCount = 0;
	if (!TestNotNull(TEXT("User-parameter rollback preserves the graph instance"), RestoredInstance) ||
		!TestNotNull(TEXT("User-parameter rollback preserves the parameter bag"), RestoredBag) ||
		!TestNotNull(TEXT("User-parameter rollback preserves the descriptor"), RestoredDesc) ||
		!ReadInt32Parameter(*this, *RestoredBag, CountName, RestoredCount))
	{
		return false;
	}
	TestEqual(TEXT("User-parameter rollback restores the inherited value"), RestoredCount, 7);
	TestFalse(
		TEXT("User-parameter rollback restores the inherited override flag"),
		RestoredInstance->IsPropertyOverridden(RestoredDesc->CachedProperty));
	TestFixtureClean(TEXT("set_component_user_parameters save failure"));

	Component->SetGraphLocal(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGCleanupCoalescingMetadataTest,
	"Monolith.PCG.Component.CleanupCoalescingMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGCleanupCoalescingMetadataTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor)
		|| !AddValidBoundsRoot(*this, Fixture.Actor))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
	Component->bGenerated = true;

	TSharedPtr<FJsonObject> FirstParams = MakeShared<FJsonObject>();
	FirstParams->SetStringField(TEXT("component_path"), ComponentPath);
	FirstParams->SetBoolField(TEXT("remove_components"), false);
	const FMonolithActionResult FirstResult = ExecuteAction(TEXT("cleanup_component"), FirstParams);
	if (!RequireSuccess(*this, TEXT("Initial cleanup schedule"), FirstResult))
	{
		return false;
	}
	TestTrue(TEXT("Initial cleanup is scheduled"), FirstResult.Result->GetBoolField(TEXT("scheduled")));
	TestFalse(
		TEXT("Initial cleanup reports its actual requested mode"),
		FirstResult.Result->GetBoolField(TEXT("remove_components")));
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("Scheduled cleanup path"), FirstResult.Result))
	{
		return false;
	}
	if (!TestTrue(TEXT("UE PCG owns the in-flight cleanup"), Component->IsCleaningUp()))
	{
		return false;
	}
	const FString FirstCleanupTaskId =
		FirstResult.Result->GetStringField(TEXT("scheduled_task_id"));
	TestFalse(
		TEXT("Initial cleanup reports the task id returned by CleanupLocal"),
		FirstCleanupTaskId.IsEmpty());

	TSharedPtr<FJsonObject> CoalescedParams = MakeShared<FJsonObject>();
	CoalescedParams->SetStringField(TEXT("component_path"), ComponentPath);
	CoalescedParams->SetBoolField(TEXT("remove_components"), true);
	const FMonolithActionResult CoalescedResult = ExecuteAction(TEXT("cleanup_component"), CoalescedParams);
	if (!RequireSuccess(*this, TEXT("Coalesced cleanup request"), CoalescedResult))
	{
		return false;
	}
	TestTrue(
		TEXT("Second cleanup request reports that work was already in flight"),
		CoalescedResult.Result->GetBoolField(TEXT("already_cleaning")));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TestEqual(
		TEXT("Coalesced cleanup preserves the actual in-flight task identity"),
		CoalescedResult.Result->GetStringField(TEXT("scheduled_task_id")),
		FirstCleanupTaskId);
#else
	TestFalse(
		TEXT("UE 5.7 reports in-flight cleanup task identity as unsupported"),
		CoalescedResult.Result->GetBoolField(TEXT("scheduled_task_id_supported")));
	TestFalse(
		TEXT("UE 5.7 does not fabricate an in-flight cleanup task id"),
		CoalescedResult.Result->HasField(TEXT("scheduled_task_id")));
#endif
	TestTrue(
		TEXT("Response preserves the second request mode as requested-only metadata"),
		CoalescedResult.Result->GetBoolField(TEXT("requested_remove_components")));
	TestFalse(
		TEXT("Response explicitly marks the in-flight cleanup mode as unobservable"),
		CoalescedResult.Result->GetBoolField(TEXT("inflight_remove_components_known")));
	TestEqual(
		TEXT("Response names the coalescing limitation"),
		CoalescedResult.Result->GetStringField(TEXT("coalescing_status")),
		FString(TEXT("already_cleaning_mode_not_observable")));
	TestFalse(
		TEXT("Coalesced response never relabels the requested mode as the actual in-flight mode"),
		CoalescedResult.Result->HasField(TEXT("remove_components")));
	TestFalse(
		TEXT("Coalesced cleanup does not prepare source control"),
		CoalescedResult.Result->HasField(TEXT("source_control_prepare")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGGraphAssignmentCyclePreflightTest,
	"Monolith.PCG.Component.GraphAssignmentCyclePreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGGraphAssignmentCyclePreflightTest::RunTest(const FString& Parameters)
{
	UPCGGraphInstance* InstanceA = NewObject<UPCGGraphInstance>(
		GetTransientPackage(),
		UPCGGraphInstance::StaticClass(),
		NAME_None,
		RF_Transient);
	UPCGGraphInstance* InstanceB = NewObject<UPCGGraphInstance>(
		GetTransientPackage(),
		UPCGGraphInstance::StaticClass(),
		NAME_None,
		RF_Transient);
	if (!TestNotNull(TEXT("First transient graph instance created"), InstanceA)
		|| !TestNotNull(TEXT("Second transient graph instance created"), InstanceB))
	{
		return false;
	}

	TestFalse(TEXT("A graph instance rejects itself as its parent"), InstanceA->CanGraphInterfaceBeSet(InstanceA));
	TestTrue(TEXT("An acyclic graph instance parent is accepted"), InstanceA->CanGraphInterfaceBeSet(InstanceB));
	InstanceA->SetGraph(InstanceB);
	if (!TestTrue(TEXT("Acyclic parent assignment commits"), InstanceA->Graph.Get() == InstanceB))
	{
		return false;
	}

	TestFalse(
		TEXT("Preflight rejects B -> A because A already resolves through B"),
		InstanceB->CanGraphInterfaceBeSet(InstanceA));
	TestNull(TEXT("Cycle preflight is non-mutating"), InstanceB->Graph.Get());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGDirtyGeneratedGenerateTest,
	"Monolith.PCG.Component.DirtyGeneratedSchedulesGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGDirtyGeneratedGenerateTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

#if !WITH_EDITORONLY_DATA
	AddError(TEXT("Dirty-generated regression requires WITH_EDITORONLY_DATA"));
	return false;
#else
	UWorld* World = GetEditorWorld();
	if (!TestNotNull(TEXT("Editor world is available"), World))
	{
		return false;
	}
	FScopedActorFixture Fixture(World);
	if (!TestNotNull(TEXT("Host actor spawned"), Fixture.Actor))
	{
		return false;
	}

	UBoxComponent* BoundsComponent = NewObject<UBoxComponent>(
		Fixture.Actor,
		UBoxComponent::StaticClass(),
		TEXT("MonolithPCGGenerationBounds"),
		RF_Transient);
	if (!TestNotNull(TEXT("Generation bounds component created"), BoundsComponent))
	{
		return false;
	}
	BoundsComponent->SetBoxExtent(FVector(100.0));
	Fixture.Actor->AddInstanceComponent(BoundsComponent);
	if (!TestTrue(TEXT("Generation bounds becomes the actor root"), Fixture.Actor->SetRootComponent(BoundsComponent)))
	{
		return false;
	}
	BoundsComponent->RegisterComponent();
	if (!TestTrue(
		TEXT("Host actor exposes valid PCG generation bounds"),
		Fixture.Actor->GetComponentsBoundingBox(/*bNonColliding=*/true).IsValid != 0))
	{
		return false;
	}

	FString ComponentPath;
	UPCGComponent* Component = CreateComponent(*this, Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("PCG component created"), Component))
	{
		return false;
	}
	UPCGGraph* Graph = NewObject<UPCGGraph>(
		Component,
		UPCGGraph::StaticClass(),
		TEXT("MonolithPCGDirtyGeneratedGraph"),
		RF_Transient);
	if (!TestNotNull(TEXT("Transient generation graph created"), Graph))
	{
		return false;
	}
	Component->bRegenerateInEditor = false;
	Component->SetGraphLocal(Graph);
	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Graph assignment preserves the exact component"), Component))
	{
		return false;
	}
	if (!TestFalse(TEXT("Fixture graph assignment leaves generation idle"), Component->IsGenerating())
		|| !TestFalse(TEXT("Fixture graph assignment leaves cleanup idle"), Component->IsCleaningUp())
		|| !TestFalse(TEXT("Fixture graph assignment leaves refresh idle"), Component->IsRefreshInProgress()))
	{
		return false;
	}

	Component->bGenerated = true;
	Component->bDirtyGenerated = true;
	TestTrue(TEXT("Fixture represents previously generated state"), Component->bGenerated);
	TestTrue(TEXT("Fixture marks the generated state dirty"), Component->bDirtyGenerated);

	TSharedPtr<FJsonObject> GenerateParams = MakeShared<FJsonObject>();
	GenerateParams->SetStringField(TEXT("component_path"), ComponentPath);
	GenerateParams->SetBoolField(TEXT("force"), false);
	const FMonolithActionResult GenerateResult =
		ExecuteAction(TEXT("generate_component"), GenerateParams);
	if (!RequireSuccess(*this, TEXT("generate_component for dirty generated state"), GenerateResult))
	{
		return false;
	}
	if (!RequireTransientSourceControlPrepare(
			*this, TEXT("Scheduled generation path"), GenerateResult.Result))
	{
		return false;
	}
	TestTrue(
		TEXT("Dirty generated state schedules through UE ShouldGenerate"),
		GenerateResult.Result->GetBoolField(TEXT("scheduled")));
	TestFalse(
		TEXT("Dirty generated state is not misreported as already generated"),
		GenerateResult.Result->GetBoolField(TEXT("already_generated")));
	TestTrue(
		TEXT("Scheduled dirty regeneration returns a valid task id"),
		GenerateResult.Result->GetBoolField(TEXT("scheduled_task_valid")));
	TestFalse(
		TEXT("Scheduled dirty regeneration returns a non-empty decimal task id"),
		GenerateResult.Result->GetStringField(TEXT("scheduled_task_id")).IsEmpty());

	Component = FindComponentExactOnActor(Fixture.Actor, ComponentPath);
	if (!TestNotNull(TEXT("Scheduled component remains exactly resolvable"), Component))
	{
		return false;
	}
	TestTrue(TEXT("UE PCG owns the scheduled generation task"), Component->IsGenerating());
	Component->CancelGeneration();
	TestFalse(TEXT("Fixture cleanup cancels the generation task"), Component->IsGenerating());
	TestFalse(TEXT("Fixture cleanup cancels the dependent shallow cleanup task"), Component->IsCleaningUp());
	Component->bGenerated = false;
	Component->bDirtyGenerated = false;

	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPCGComponentParamGuardTest,
	"Monolith.PCG.Component.ParamGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPCGComponentParamGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithPCGComponentActionsTests;
	RegisterActions();

	const TArray<FString> ComponentPathActions = {
		TEXT("get_component"),
		TEXT("set_component_graph"),
		TEXT("set_component_settings"),
		TEXT("generate_component"),
		TEXT("refresh_component"),
		TEXT("cancel_component"),
		TEXT("cleanup_component"),
		TEXT("get_component_output"),
		TEXT("set_component_user_parameters")};
	for (const FString& Action : ComponentPathActions)
	{
		const FMonolithActionResult Result = ExecuteAction(Action, MakeShared<FJsonObject>());
		TestFalse(*FString::Printf(TEXT("pcg.%s rejects a missing component_path"), *Action), Result.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("pcg.%s names the missing component_path"), *Action),
			Result.ErrorMessage.Contains(TEXT("component_path")));
	}

	const FMonolithActionResult CreateResult =
		ExecuteAction(TEXT("create_component"), MakeShared<FJsonObject>());
	TestFalse(TEXT("pcg.create_component rejects a missing actor_path"), CreateResult.bSuccess);
	TestTrue(
		TEXT("pcg.create_component names the missing actor_path"),
		CreateResult.ErrorMessage.Contains(TEXT("actor_path")));

	return true;
}
