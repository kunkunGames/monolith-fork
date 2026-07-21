#include "MonolithPCGComponentActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSourceControlUtils.h"

#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGManagedResource.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Grid/PCGPartitionActor.h"
#include "Subsystems/PCGSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "String/LexFromString.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace MonolithPCGComponent
{
static constexpr int32 MaxUserParameters = 256;
static constexpr int32 MaxOutputItems = 500;
static constexpr int32 MaxManagedResources = 500;
static constexpr int32 MaxManagedObjectsPerResource = 500;
static constexpr int32 MaxTagsPerOutput = 500;
static constexpr int32 MaxUserParameterValueChars = 4096;
static constexpr double MaxExactJsonInteger = 9007199254740991.0; // 2^53 - 1

FMonolithActionExecutionPolicy TransactionPolicy()
{
	FMonolithActionExecutionPolicy Policy;
	Policy.PolicyId = TEXT("transaction_optional");
	Policy.bDefaulted = false;
	Policy.bDirtyPackageTracking = true;
	Policy.bTransactionWrapping = true;
	Policy.bPostEditValidation = false;
	Policy.bEnforced = true;
	return Policy;
}

FMonolithActionExecutionPolicy AsyncMutationPolicy()
{
	FMonolithActionExecutionPolicy Policy;
	Policy.PolicyId = TEXT("track_dirty_packages");
	Policy.bDefaulted = false;
	Policy.bDirtyPackageTracking = true;
	// Generation, refresh, cancellation, and cleanup continue after this handler
	// returns. Wrapping only the scheduling call in an undo transaction would be
	// misleading because the async work would run outside that transaction.
	Policy.bTransactionWrapping = false;
	Policy.bPostEditValidation = false;
	Policy.bEnforced = true;
	return Policy;
}

TSharedPtr<FJsonObject> ErrorData(const FString& Field, const FString& Detail)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field"), Field);
	Data->SetStringField(TEXT("detail"), Detail);
	return Data;
}

FMonolithActionResult InvalidParam(const FString& Field, const FString& Detail)
{
	return FMonolithActionResult::Error(Detail, FMonolithJsonUtils::ErrInvalidParams)
		.WithErrorData(ErrorData(Field, Detail));
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue,
	FMonolithActionResult& OutError)
{
	OutValue.Reset();
	if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue))
	{
		OutError = InvalidParam(Field, FString::Printf(TEXT("%s must be a string"), Field));
		return false;
	}
	OutValue.TrimStartAndEndInline();
	if (OutValue.IsEmpty())
	{
		OutError = InvalidParam(Field, FString::Printf(TEXT("%s must not be empty"), Field));
		return false;
	}
	return true;
}

bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool DefaultValue, bool& OutValue,
	FMonolithActionResult& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(Field);
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean || !JsonValue->TryGetBool(OutValue))
	{
		OutError = InvalidParam(Field, FString::Printf(TEXT("%s must be a boolean"), Field));
		return false;
	}
	return true;
}

bool ReadOptionalInt32(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue, int32& OutValue,
	FMonolithActionResult& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}

	double Number = 0.0;
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(Field);
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Number) ||
		!FMath::IsFinite(Number) || FMath::TruncToDouble(Number) != Number || Number < static_cast<double>(MIN_int32) ||
		Number > static_cast<double>(MAX_int32))
	{
		OutError = InvalidParam(Field, FString::Printf(TEXT("%s must be a 32-bit integer"), Field));
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool ReadOptionalBoundedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue, int32 MinValue,
	int32 MaxValue, int32& OutValue, FMonolithActionResult& OutError)
{
	if (!ReadOptionalInt32(Params, Field, DefaultValue, OutValue, OutError))
	{
		return false;
	}
	if (OutValue < MinValue || OutValue > MaxValue)
	{
		OutError = InvalidParam(Field,
			FString::Printf(TEXT("%s must be an integer in range %d..%d"), Field, MinValue, MaxValue));
		return false;
	}
	return true;
}

UWorld* GetEditorWorld(FMonolithActionResult& OutError)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = FMonolithActionResult::Error(TEXT("No active editor world is available"));
	}
	return World;
}

AActor* ResolveActorExact(const FString& ActorPath, FMonolithActionResult& OutError)
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	AActor* Match = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (IsValid(Actor) && Actor->GetPathName().Equals(ActorPath, ESearchCase::CaseSensitive))
		{
			if (Match)
			{
				OutError = FMonolithActionResult::Error(
					FString::Printf(TEXT("actor_path resolved more than once in the active editor world: %s"), *ActorPath));
				return nullptr;
			}
			Match = Actor;
		}
	}

	if (!Match)
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("actor_path must be an exact actor object path in the active editor world: %s"), *ActorPath));
	}
	return Match;
}

UPCGComponent* ResolveComponentExact(const FString& ComponentPath, FMonolithActionResult& OutError)
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return nullptr;
	}

	UActorComponent* ExactObject = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (IsValid(Component) && Component->GetPathName().Equals(ComponentPath, ESearchCase::CaseSensitive))
			{
				if (ExactObject)
				{
					OutError = FMonolithActionResult::Error(FString::Printf(
						TEXT("component_path resolved more than once in the active editor world: %s"), *ComponentPath));
					return nullptr;
				}
				ExactObject = Component;
			}
		}
	}

	if (!ExactObject)
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("component_path must be an exact component object path in the active editor world: %s"),
			*ComponentPath));
		return nullptr;
	}

	UPCGComponent* PCGComponent = Cast<UPCGComponent>(ExactObject);
	if (!PCGComponent)
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("component_path identifies %s, not UPCGComponent: %s"), *ExactObject->GetClass()->GetPathName(),
			*ComponentPath));
		return nullptr;
	}
	if (!PCGComponent->GetOwner() || PCGComponent->GetOwner()->GetWorld() != World)
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("component_path is not owned by an actor in the active editor world: %s"), *ComponentPath));
		return nullptr;
	}
	return PCGComponent;
}

UPCGComponent* ReadAndResolveComponent(const TSharedPtr<FJsonObject>& Params, FMonolithActionResult& OutError,
	FString* OutComponentPath = nullptr)
{
	FString ComponentPath;
	if (!ReadRequiredString(Params, TEXT("component_path"), ComponentPath, OutError))
	{
		return nullptr;
	}
	if (OutComponentPath)
	{
		*OutComponentPath = ComponentPath;
	}
	return ResolveComponentExact(ComponentPath, OutError);
}

bool LoadGraphInterface(const FString& AssetPath, UPCGGraphInterface*& OutGraph, FString& OutResolvedPath,
	FMonolithActionResult& OutError)
{
	OutGraph = nullptr;
	FString LoadError;
	if (!FMonolithAssetUtils::TryLoadAssetByPath<UPCGGraphInterface>(
			AssetPath, OutGraph, OutResolvedPath, LoadError))
	{
		OutError = InvalidParam(TEXT("graph_asset_path"),
			FString::Printf(TEXT("Could not load UPCGGraphInterface '%s': %s"), *AssetPath, *LoadError));
		return false;
	}
	return true;
}

bool ResolveBlueprintPCGComponentTemplate(
	const FString& BlueprintAssetPath,
	const FString& ComponentName,
	UBlueprint*& OutBlueprint,
	UPCGComponent*& OutComponent,
	FString& OutResolvedBlueprintPath,
	FMonolithActionResult& OutError)
{
	OutBlueprint = nullptr;
	OutComponent = nullptr;
	OutResolvedBlueprintPath.Reset();
	FString LoadError;
	if (!FMonolithAssetUtils::TryLoadAssetByPath<UBlueprint>(
		BlueprintAssetPath, OutBlueprint, OutResolvedBlueprintPath, LoadError))
	{
		OutError = InvalidParam(
			TEXT("blueprint_asset_path"),
			FString::Printf(TEXT("Could not load Blueprint '%s': %s"), *BlueprintAssetPath, *LoadError));
		return false;
	}
	if (!OutBlueprint || !OutBlueprint->SimpleConstructionScript)
	{
		OutError = InvalidParam(
			TEXT("blueprint_asset_path"),
			TEXT("blueprint_asset_path must resolve to an Actor Blueprint with a SimpleConstructionScript"));
		return false;
	}
	if (!OutBlueprint->GeneratedClass || !OutBlueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		OutError = InvalidParam(
			TEXT("blueprint_asset_path"),
			TEXT("blueprint_asset_path must resolve to an Actor-derived Blueprint"));
		return false;
	}
	if (!FMonolithAssetUtils::IsProjectOwnedPackage(OutBlueprint->GetOutermost()->GetName()))
	{
		OutError = InvalidParam(
			TEXT("blueprint_asset_path"),
			TEXT("blueprint_asset_path must resolve to a project-owned Blueprint package"));
		return false;
	}

	USCS_Node* Match = nullptr;
	for (USCS_Node* Node : OutBlueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || Node->GetVariableName().ToString() != ComponentName)
		{
			continue;
		}
		if (Match)
		{
			OutError = InvalidParam(
				TEXT("component_name"),
				FString::Printf(TEXT("component_name resolves more than once in the Blueprint SCS: %s"), *ComponentName));
			return false;
		}
		Match = Node;
	}
	if (!Match)
	{
		OutError = InvalidParam(
			TEXT("component_name"),
			FString::Printf(TEXT("component_name must exactly identify one Blueprint SCS component: %s"), *ComponentName));
		return false;
	}
	OutComponent = Cast<UPCGComponent>(Match->ComponentTemplate);
	if (!OutComponent)
	{
		OutError = InvalidParam(
			TEXT("component_name"),
			FString::Printf(
				TEXT("component_name identifies %s, not UPCGComponent: %s"),
				Match->ComponentTemplate ? *Match->ComponentTemplate->GetClass()->GetPathName() : TEXT("null"),
				*ComponentName));
		return false;
	}
	if (!OutComponent->GetGraphInstance())
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Blueprint PCG component template has no graph instance: %s"), *ComponentName));
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> BuildBlueprintComponentGraphResult(
	const UBlueprint* Blueprint,
	const UPCGComponent* Component,
	const FString& ComponentName,
	const FString& ResolvedBlueprintPath,
	const FString& PreviousGraphPath,
	const FString& RequestedGraphPath,
	bool bDryRun,
	bool bWouldChange,
	bool bChanged,
	bool bSaved,
	const FString& SavedFilename = FString())
{
	const UPCGGraphInstance* GraphInstance = Component ? Component->GetGraphInstance() : nullptr;
	const UPCGGraphInterface* ReadBackGraph = GraphInstance ? GraphInstance->Graph.Get() : nullptr;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("operation"), TEXT("set_blueprint_component_graph"));
	Result->SetStringField(TEXT("blueprint_asset_path"), ResolvedBlueprintPath);
	Result->SetStringField(TEXT("blueprint_object_path"), Blueprint ? Blueprint->GetPathName() : FString());
	Result->SetStringField(TEXT("component_name"), ComponentName);
	Result->SetStringField(TEXT("component_template_path"), Component ? Component->GetPathName() : FString());
	Result->SetStringField(TEXT("graph_instance_path"), GraphInstance ? GraphInstance->GetPathName() : FString());
	Result->SetStringField(TEXT("previous_graph_asset_path"), PreviousGraphPath);
	Result->SetStringField(TEXT("requested_graph_asset_path"), RequestedGraphPath);
	Result->SetStringField(TEXT("read_back_graph_asset_path"), ReadBackGraph ? ReadBackGraph->GetPathName() : FString());
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("saved_filename"), SavedFilename);
	Result->SetNumberField(
		TEXT("compile_status_value"),
		Blueprint ? static_cast<int32>(Blueprint->Status) : -1);
	Result->SetBoolField(TEXT("compile_succeeded"), Blueprint && Blueprint->Status != BS_Error);
	return Result;
}

bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutFilename)
{
	OutFilename.Reset();
	if (!Blueprint || !Blueprint->GetOutermost())
	{
		return false;
	}
	OutFilename = FPackageName::LongPackageNameToFilename(
		Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.Error = GError;
	return UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *OutFilename, SaveArgs);
}

FString GenerationTriggerToString(EPCGComponentGenerationTrigger Trigger)
{
	switch (Trigger)
	{
	case EPCGComponentGenerationTrigger::GenerateOnLoad:
		return TEXT("on_load");
	case EPCGComponentGenerationTrigger::GenerateOnDemand:
		return TEXT("on_demand");
	case EPCGComponentGenerationTrigger::GenerateAtRuntime:
		return TEXT("at_runtime");
	default:
		return TEXT("unknown");
	}
}

bool ParseGenerationTrigger(const FString& Input, EPCGComponentGenerationTrigger& OutTrigger)
{
	FString Normalized = Input;
	Normalized.TrimStartAndEndInline();
	Normalized.ToLowerInline();
	Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
	if (Normalized == TEXT("on_load") || Normalized == TEXT("generate_on_load"))
	{
		OutTrigger = EPCGComponentGenerationTrigger::GenerateOnLoad;
		return true;
	}
	if (Normalized == TEXT("on_demand") || Normalized == TEXT("generate_on_demand"))
	{
		OutTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
		return true;
	}
	if (Normalized == TEXT("at_runtime") || Normalized == TEXT("generate_at_runtime"))
	{
		OutTrigger = EPCGComponentGenerationTrigger::GenerateAtRuntime;
		return true;
	}
	return false;
}

FString TaskIdToString(FPCGTaskId TaskId)
{
	return FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(TaskId));
}

void AddTaskFields(const TSharedPtr<FJsonObject>& Object, const TCHAR* Prefix, FPCGTaskId TaskId)
{
	const bool bValid = TaskId != InvalidPCGTaskId;
	Object->SetBoolField(FString::Printf(TEXT("%s_task_valid"), Prefix), bValid);
	Object->SetStringField(FString::Printf(TEXT("%s_task_id"), Prefix), bValid ? TaskIdToString(TaskId) : FString());
}

TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), Box.IsValid != 0);
	if (Box.IsValid)
	{
		TSharedPtr<FJsonObject> Min = MakeShared<FJsonObject>();
		Min->SetNumberField(TEXT("x"), Box.Min.X);
		Min->SetNumberField(TEXT("y"), Box.Min.Y);
		Min->SetNumberField(TEXT("z"), Box.Min.Z);
		TSharedPtr<FJsonObject> Max = MakeShared<FJsonObject>();
		Max->SetNumberField(TEXT("x"), Box.Max.X);
		Max->SetNumberField(TEXT("y"), Box.Max.Y);
		Max->SetNumberField(TEXT("z"), Box.Max.Z);
		Result->SetObjectField(TEXT("min"), Min);
		Result->SetObjectField(TEXT("max"), Max);
	}
	return Result;
}

int32 CountManagedResources(const UPCGComponent* Component)
{
	if (!Component || !Component->AreManagedResourcesAccessible())
	{
		return INDEX_NONE;
	}
	int32 Count = 0;
	Component->ForEachConstManagedResource([&Count](const UPCGManagedResource* Resource)
	{
		if (Resource)
		{
			++Count;
		}
	});
	return Count;
}

bool HasGeneratedState(const UPCGComponent* Component)
{
	if (!Component)
	{
		return false;
	}
	if (Component->bGenerated || Component->IsGenerating() || Component->IsCleaningUp()
#if WITH_EDITOR
		|| Component->IsRefreshInProgress()
#endif
	)
	{
		return true;
	}
	const int32 ResourceCount = CountManagedResources(Component);
	if (ResourceCount > 0)
	{
		return true;
	}
	return !Component->GetGeneratedGraphOutput().TaggedData.IsEmpty();
}

bool RequireIdleUngenerated(const UPCGComponent* Component, const FString& Action, FMonolithActionResult& OutError)
{
	if (!Component)
	{
		OutError = FMonolithActionResult::Error(TEXT("Internal error: null PCG component"));
		return false;
	}
	if (Component->IsGenerating() || Component->IsCleaningUp()
#if WITH_EDITOR
		|| Component->IsRefreshInProgress()
#endif
	)
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("%s requires an idle component; poll pcg.get_component or cancel/cleanup the active work first"),
			*Action));
		return false;
	}
	if (HasGeneratedState(Component))
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("%s refuses to change a generated component; call pcg.cleanup_component and poll until idle first"),
			*Action));
		return false;
	}
	return true;
}

bool RequireOriginalComponentMutation(const UPCGComponent* Component, const FString& Action,
	FMonolithActionResult& OutError)
{
	if (!Component)
	{
		OutError = FMonolithActionResult::Error(TEXT("Internal error: null PCG component"));
		return false;
	}
	const APCGPartitionActor* PartitionActor = Cast<APCGPartitionActor>(Component->GetOwner());
	if (!Component->IsLocalComponent() && !PartitionActor)
	{
		return true;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_path"), Component->GetPathName());
	Data->SetBoolField(TEXT("local_component"), Component->IsLocalComponent());
	Data->SetBoolField(TEXT("partition_actor_owned"), PartitionActor != nullptr);
	const UPCGComponent* OriginalComponent = PartitionActor ? PartitionActor->GetOriginalComponent(Component) : nullptr;
	Data->SetStringField(TEXT("original_component_path"),
		OriginalComponent ? OriginalComponent->GetPathName() : FString());
	OutError = FMonolithActionResult::Error(FString::Printf(
		TEXT("%s refuses engine-owned local or partition-actor components; invoke the action on original_component_path"),
		*Action)).WithErrorData(Data);
	return false;
}

struct FDirtySnapshot
{
	UPackage* ActorPackage = nullptr;
	UPackage* LevelPackage = nullptr;
	bool bActorDirty = false;
	bool bLevelDirty = false;

	explicit FDirtySnapshot(const AActor* Actor)
	{
		ActorPackage = Actor ? Actor->GetPackage() : nullptr;
		LevelPackage = Actor && Actor->GetLevel() ? Actor->GetLevel()->GetOutermost() : nullptr;
		bActorDirty = ActorPackage && ActorPackage->IsDirty();
		bLevelDirty = LevelPackage && LevelPackage->IsDirty();
	}

	void Restore() const
	{
		if (ActorPackage)
		{
			ActorPackage->SetDirtyFlag(bActorDirty);
		}
		if (LevelPackage && LevelPackage != ActorPackage)
		{
			LevelPackage->SetDirtyFlag(bLevelDirty);
		}
	}

	void PreserveDirty() const
	{
		if (ActorPackage)
		{
			ActorPackage->SetDirtyFlag(true);
		}
		if (LevelPackage)
		{
			LevelPackage->SetDirtyFlag(true);
		}
	}
};

void FinalizeRollbackDirtyState(bool bRollbackComplete, AActor* Actor, UPCGComponent* Component,
	const FDirtySnapshot& DirtySnapshot)
{
	if (bRollbackComplete)
	{
		DirtySnapshot.Restore();
		return;
	}

	// A failed rollback must remain visible to Save All, source-control reconciliation,
	// and the operator. Restoring a previously clean dirty bit here would hide a
	// partially mutated component after reporting an error.
	if (IsValid(Component))
	{
		Component->MarkPackageDirty();
	}
	if (IsValid(Actor))
	{
		Actor->MarkPackageDirty();
	}
	DirtySnapshot.PreserveDirty();
}

bool IsTransientMutationTarget(const UObject* Target)
{
	for (const UObject* Outer = Target; Outer && !Outer->IsA<UPackage>(); Outer = Outer->GetOuter())
	{
		if (Outer->HasAnyFlags(RF_Transient))
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> BuildHandlerOwnedSourceControlPrepare(
	const FString& Status,
	const TSharedPtr<FJsonObject>& BeforeAction)
{
	TSharedPtr<FJsonObject> SafeBeforeAction = BeforeAction;
	FString SafeStatus = Status;
	if (!SafeBeforeAction.IsValid())
	{
		SafeBeforeAction = MakeShared<FJsonObject>();
		SafeBeforeAction->SetBoolField(TEXT("ok"), false);
		SafeBeforeAction->SetStringField(TEXT("status"), TEXT("missing_before_action"));
		SafeBeforeAction->SetStringField(
			TEXT("message"), TEXT("Handler-owned source-control preparation produced no utility result"));
		SafeStatus = TEXT("failed");
	}
	TSharedPtr<FJsonObject> Prepare = MakeShared<FJsonObject>();
	Prepare->SetStringField(TEXT("mode"), TEXT("handler_owned_pre_mutation"));
	Prepare->SetStringField(TEXT("status"), SafeStatus);
	Prepare->SetObjectField(TEXT("before_action"), SafeBeforeAction);
	return Prepare;
}

bool PrepareSourceControlBeforeMutation(
	UObject* Target,
	const FString& Action,
	TSharedPtr<FJsonObject>& OutPrepare,
	FMonolithActionResult& OutError)
{
	OutPrepare.Reset();
	if (!IsValid(Target))
	{
		TSharedPtr<FJsonObject> BeforeAction = MakeShared<FJsonObject>();
		BeforeAction->SetStringField(TEXT("operation"), TEXT("checkout_or_add"));
		BeforeAction->SetBoolField(TEXT("ok"), false);
		BeforeAction->SetStringField(TEXT("status"), TEXT("invalid_target"));
		BeforeAction->SetStringField(TEXT("message"), TEXT("Exact live mutation target is unavailable"));
		OutPrepare = BuildHandlerOwnedSourceControlPrepare(TEXT("failed"), BeforeAction);
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("source_control_prepare"), OutPrepare);
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("%s aborted before mutation because the exact live target is unavailable"),
			*Action)).WithErrorData(ErrorData);
		return false;
	}
	UPackage* Package = Target ? Target->GetOutermost() : nullptr;
	const FString PackageName = Package ? Package->GetName() : FString();
	const bool bProjectPackage = Package && !Package->HasAnyFlags(RF_Transient) &&
		Package != GetTransientPackage() &&
		FMonolithAssetUtils::IsProjectOwnedPackage(PackageName);
	if (!bProjectPackage || IsTransientMutationTarget(Target))
	{
		TSharedPtr<FJsonObject> BeforeAction = MakeShared<FJsonObject>();
		BeforeAction->SetStringField(TEXT("operation"), TEXT("checkout_or_add"));
		BeforeAction->SetBoolField(TEXT("ok"), true);
		BeforeAction->SetBoolField(TEXT("skipped"), true);
		BeforeAction->SetStringField(TEXT("status"), TEXT("skipped_non_project_package"));
		BeforeAction->SetStringField(TEXT("package_name"), PackageName);
		BeforeAction->SetStringField(
			TEXT("message"),
			TEXT("Transient and non-project packages are outside handler-owned source-control preparation"));
		OutPrepare = BuildHandlerOwnedSourceControlPrepare(
			TEXT("skipped_non_project_package"), BeforeAction);
		return true;
	}

	FMonolithSourceControlPrepareOptions Options;
	Options.bUnavailableIsSuccess = true;
	TSharedPtr<FJsonObject> BeforeAction =
		FMonolithSourceControlUtils::CheckoutOrAddPackage(Package, Options);
	bool bOk = false;
	bool bAvailable = false;
	if (!BeforeAction.IsValid() || !BeforeAction->TryGetBoolField(TEXT("ok"), bOk))
	{
		bOk = false;
	}
	if (BeforeAction.IsValid())
	{
		BeforeAction->TryGetBoolField(TEXT("available"), bAvailable);
	}
	if (!BeforeAction.IsValid())
	{
		BeforeAction = MakeShared<FJsonObject>();
		BeforeAction->SetBoolField(TEXT("ok"), false);
		BeforeAction->SetStringField(TEXT("message"), TEXT("Source-control utility returned no result"));
	}

	const FString Status = !bOk
		? TEXT("failed")
		: (bAvailable ? TEXT("prepared") : TEXT("skipped_provider_unavailable"));
	OutPrepare = BuildHandlerOwnedSourceControlPrepare(Status, BeforeAction);
	if (bOk)
	{
		return true;
	}

	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetObjectField(TEXT("source_control_prepare"), OutPrepare);
	OutError = FMonolithActionResult::Error(FString::Printf(
		TEXT("%s aborted before mutation because source-control preparation failed for %s"),
		*Action, *PackageName)).WithErrorData(ErrorData);
	return false;
}

FMonolithActionResult AttachSourceControlPrepare(
	FMonolithActionResult Result,
	const TSharedPtr<FJsonObject>& Prepare)
{
	if (!Prepare.IsValid())
	{
		return Result;
	}
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetObjectField(TEXT("source_control_prepare"), Prepare);
	}
	else
	{
		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}
		Result.ErrorData->SetObjectField(TEXT("source_control_prepare"), Prepare);
	}
	return Result;
}

bool PreflightLevelSave(const AActor* Actor, FString& OutError)
{
	if (!Actor || !Actor->GetLevel())
	{
		OutError = TEXT("The target actor has no owning level");
		return false;
	}
	if (Actor->HasAnyFlags(RF_Transient) || Actor->GetLevel()->HasAnyFlags(RF_Transient))
	{
		OutError = TEXT("save=true is not valid for a transient actor or level; use save=false for temporary test actors");
		return false;
	}
	const FString PackageName = Actor->GetLevel()->GetOutermost()->GetName();
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FString::Printf(TEXT("The owning level has no valid package name: %s"), *PackageName);
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	if (!FPaths::FileExists(Filename))
	{
		OutError = FString::Printf(
			TEXT("The owning level is unsaved or has no on-disk map; save it explicitly before using save=true: %s"),
			*PackageName);
		return false;
	}
	return true;
}

bool SaveOwningLevel(AActor* Actor, FString& OutFilename, FString& OutError)
{
	OutFilename.Reset();
	if (!Actor || !Actor->GetLevel())
	{
		OutError = TEXT("The target actor has no owning level");
		return false;
	}
	if (!FEditorFileUtils::SaveLevel(Actor->GetLevel(), FString(), &OutFilename))
	{
		OutError = FString::Printf(
			TEXT("FEditorFileUtils::SaveLevel failed for %s; the verified live mutation remains dirty for an explicit retry"),
			*Actor->GetLevel()->GetOutermost()->GetName());
		return false;
	}
	return true;
}

FString UserParameterTypeToString(EPropertyBagPropertyType Type)
{
	switch (Type)
	{
	case EPropertyBagPropertyType::Bool: return TEXT("bool");
	case EPropertyBagPropertyType::Byte: return TEXT("byte");
	case EPropertyBagPropertyType::Int32: return TEXT("int32");
	case EPropertyBagPropertyType::Int64: return TEXT("int64");
	case EPropertyBagPropertyType::Float: return TEXT("float");
	case EPropertyBagPropertyType::Double: return TEXT("double");
	case EPropertyBagPropertyType::Name: return TEXT("name");
	case EPropertyBagPropertyType::String: return TEXT("string");
	case EPropertyBagPropertyType::Text: return TEXT("text");
	case EPropertyBagPropertyType::Enum: return TEXT("enum");
	case EPropertyBagPropertyType::Struct: return TEXT("struct");
	case EPropertyBagPropertyType::Object: return TEXT("object");
	case EPropertyBagPropertyType::SoftObject: return TEXT("soft_object");
	case EPropertyBagPropertyType::Class: return TEXT("class");
	case EPropertyBagPropertyType::SoftClass: return TEXT("soft_class");
	case EPropertyBagPropertyType::Int8: return TEXT("int8");
	case EPropertyBagPropertyType::Int16: return TEXT("int16");
	case EPropertyBagPropertyType::UInt16: return TEXT("uint16");
	case EPropertyBagPropertyType::UInt32: return TEXT("uint32");
	case EPropertyBagPropertyType::UInt64: return TEXT("uint64");
	default: return TEXT("unsupported");
	}
}

TArray<TSharedPtr<FJsonValue>> ContainerTypesToJson(const FPropertyBagContainerTypes& Types)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (EPropertyBagContainerType Type : Types)
	{
		FString Name;
		switch (Type)
		{
		case EPropertyBagContainerType::Array: Name = TEXT("array"); break;
		case EPropertyBagContainerType::Set: Name = TEXT("set"); break;
		case EPropertyBagContainerType::Map: Name = TEXT("map"); break;
		default: Name = TEXT("none"); break;
		}
		Values.Add(MakeShared<FJsonValueString>(Name));
	}
	return Values;
}

bool IsSupportedUserParameter(const FPropertyBagPropertyDesc& Desc, FString& OutReason)
{
	if (!Desc.ContainerTypes.IsEmpty())
	{
		OutReason = TEXT("container overrides are not supported by this scalar action");
		return false;
	}
	switch (Desc.ValueType)
	{
	case EPropertyBagPropertyType::Bool:
	case EPropertyBagPropertyType::Byte:
	case EPropertyBagPropertyType::Int32:
	case EPropertyBagPropertyType::Int64:
	case EPropertyBagPropertyType::Float:
	case EPropertyBagPropertyType::Double:
	case EPropertyBagPropertyType::Name:
	case EPropertyBagPropertyType::String:
	case EPropertyBagPropertyType::Enum:
	case EPropertyBagPropertyType::Object:
	case EPropertyBagPropertyType::SoftObject:
	case EPropertyBagPropertyType::Class:
	case EPropertyBagPropertyType::SoftClass:
		return true;
	default:
		OutReason = FString::Printf(TEXT("property-bag type '%s' is read-only in this action"),
			*UserParameterTypeToString(Desc.ValueType));
		return false;
	}
}

bool JsonValueToSerializedInput(const FPropertyBagPropertyDesc& Desc, const TSharedPtr<FJsonValue>& JsonValue,
	FString& OutSerialized, FString& OutError)
{
	if (!JsonValue.IsValid() || JsonValue->IsNull())
	{
		if (Desc.ValueType == EPropertyBagPropertyType::Object ||
			Desc.ValueType == EPropertyBagPropertyType::SoftObject ||
			Desc.ValueType == EPropertyBagPropertyType::Class ||
			Desc.ValueType == EPropertyBagPropertyType::SoftClass)
		{
			OutSerialized = TEXT("None");
			return true;
		}
		OutError = TEXT("null is only valid for object/class reference parameters");
		return false;
	}

	switch (Desc.ValueType)
	{
	case EPropertyBagPropertyType::Bool:
	{
		bool Value = false;
		if (JsonValue->Type != EJson::Boolean || !JsonValue->TryGetBool(Value))
		{
			OutError = TEXT("expected a JSON boolean");
			return false;
		}
		OutSerialized = Value ? TEXT("True") : TEXT("False");
		return true;
	}
	case EPropertyBagPropertyType::Int64:
	{
		if (JsonValue->Type == EJson::String)
		{
			FString Decimal;
			if (!JsonValue->TryGetString(Decimal))
			{
				OutError = TEXT("expected a canonical decimal int64 string");
				return false;
			}

			int64 Parsed = 0;
			LexFromString(Parsed, FStringView(Decimal));
			if (LexToString(Parsed) != Decimal)
			{
				OutError = TEXT("expected a canonical decimal int64 string in signed 64-bit range");
				return false;
			}

			OutSerialized = MoveTemp(Decimal);
			return true;
		}

		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value) ||
			FMath::TruncToDouble(Value) != Value)
		{
			OutError = TEXT("expected an integral JSON number or canonical decimal int64 string");
			return false;
		}
		if (FMath::Abs(Value) > MaxExactJsonInteger)
		{
			OutError = TEXT("int64 JSON number exceeds the exact 53-bit range; use a canonical decimal string");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.0f"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Byte:
	case EPropertyBagPropertyType::Int32:
	{
		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value) ||
			FMath::TruncToDouble(Value) != Value)
		{
			OutError = TEXT("expected an integral JSON number");
			return false;
		}
		if (Desc.ValueType == EPropertyBagPropertyType::Byte && (Value < 0.0 || Value > 255.0))
		{
			OutError = TEXT("byte value must be in range 0..255");
			return false;
		}
		if (Desc.ValueType == EPropertyBagPropertyType::Int32 &&
			(Value < static_cast<double>(MIN_int32) || Value > static_cast<double>(MAX_int32)))
		{
			OutError = TEXT("int32 value is out of range");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.0f"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Float:
	case EPropertyBagPropertyType::Double:
	{
		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value))
		{
			OutError = TEXT("expected a finite JSON number");
			return false;
		}
		if (Desc.ValueType == EPropertyBagPropertyType::Float &&
			FMath::Abs(Value) > static_cast<double>(TNumericLimits<float>::Max()))
		{
			OutError = TEXT("float value is out of finite 32-bit range");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.17g"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Name:
	case EPropertyBagPropertyType::String:
	case EPropertyBagPropertyType::Enum:
	case EPropertyBagPropertyType::Object:
	case EPropertyBagPropertyType::SoftObject:
	case EPropertyBagPropertyType::Class:
	case EPropertyBagPropertyType::SoftClass:
	{
		if (JsonValue->Type != EJson::String || !JsonValue->TryGetString(OutSerialized))
		{
			OutError = TEXT("expected a JSON string");
			return false;
		}
		if (OutSerialized.Len() > MaxUserParameterValueChars)
		{
			OutError = FString::Printf(TEXT("string value exceeds the %d-character action limit"),
				MaxUserParameterValueChars);
			return false;
		}
		return true;
	}
	default:
		OutError = TEXT("unsupported parameter type");
		return false;
	}
}

void SetBoundedStringField(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FString& Value)
{
	const bool bTruncated = Value.Len() > MaxUserParameterValueChars;
	Object->SetStringField(Field, bTruncated ? Value.Left(MaxUserParameterValueChars) : Value);
	Object->SetNumberField(Field + TEXT("_char_count"), Value.Len());
	Object->SetBoolField(Field + TEXT("_truncated"), bTruncated);
}

TSharedPtr<FJsonObject> BuildUserParameterRow(const UPCGGraphInstance* Instance,
	const FInstancedPropertyBag& Values, const FPropertyBagPropertyDesc& Desc)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("name"), Desc.Name.ToString());
	Row->SetStringField(TEXT("id"), Desc.ID.ToString(EGuidFormats::DigitsWithHyphensLower));
	Row->SetStringField(TEXT("value_type"), UserParameterTypeToString(Desc.ValueType));
	Row->SetArrayField(TEXT("container_types"), ContainerTypesToJson(Desc.ContainerTypes));
	Row->SetStringField(TEXT("key_type"), UserParameterTypeToString(Desc.KeyType));
	Row->SetStringField(TEXT("value_type_object_path"), Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString());
	Row->SetStringField(TEXT("key_type_object_path"), Desc.KeyTypeObject ? Desc.KeyTypeObject->GetPathName() : FString());
	Row->SetStringField(TEXT("property_flags_hex"), FString::Printf(TEXT("0x%016llx"),
		static_cast<unsigned long long>(Desc.PropertyFlags)));

	FString UnsupportedReason;
	const bool bSupported = IsSupportedUserParameter(Desc, UnsupportedReason);
	const bool bCanInspectValue = bSupported && Desc.CachedProperty && Values.GetValue().IsValid();
	if (bCanInspectValue)
	{
		const void* ValuePtr = Desc.CachedProperty->ContainerPtrToValuePtr<void>(Values.GetValue().GetMemory());
		FString SerializedValue;
		bool bSerializedValueAvailable = false;
		if (Desc.ValueType == EPropertyBagPropertyType::String)
		{
			const FString& DirectStringValue = *static_cast<const FString*>(ValuePtr);
			SetBoundedStringField(Row, TEXT("value"), DirectStringValue);
			Row->SetStringField(TEXT("value_encoding"), TEXT("string"));
			if (DirectStringValue.Len() <= MaxUserParameterValueChars)
			{
				const TValueOrError<FString, EPropertyBagResult> Serialized =
					Values.GetValueSerializedString(Desc.Name);
				if (Serialized.IsValid())
				{
					SerializedValue = Serialized.GetValue();
					bSerializedValueAvailable = true;
				}
			}
			else
			{
				Row->SetStringField(TEXT("serialized_value_omitted_reason"),
					TEXT("string_exceeds_serialization_bound"));
			}
		}
		else
		{
			const TValueOrError<FString, EPropertyBagResult> Serialized =
				Values.GetValueSerializedString(Desc.Name);
			if (Serialized.IsValid())
			{
				SerializedValue = Serialized.GetValue();
				bSerializedValueAvailable = true;
			}
			else
			{
				Row->SetStringField(TEXT("serialized_value_omitted_reason"),
					TEXT("property_bag_serialization_failed"));
			}
		}
		Row->SetBoolField(TEXT("serialized_value_authoritative"), bSerializedValueAvailable);
		if (bSerializedValueAvailable)
		{
			SetBoundedStringField(Row, TEXT("serialized_value"), SerializedValue);
		}

		if (Desc.ValueType == EPropertyBagPropertyType::Int64 && bSerializedValueAvailable)
		{
			int64 IntegerValue = 0;
			if (LexTryParseString(IntegerValue, *SerializedValue))
			{
				if (IntegerValue >= -static_cast<int64>(MaxExactJsonInteger) &&
					IntegerValue <= static_cast<int64>(MaxExactJsonInteger))
				{
					Row->SetNumberField(TEXT("value"), static_cast<double>(IntegerValue));
					Row->SetStringField(TEXT("value_encoding"), TEXT("json_number"));
				}
				else
				{
					Row->SetStringField(TEXT("value"), SerializedValue);
					Row->SetStringField(TEXT("value_encoding"), TEXT("decimal_string"));
				}
			}
		}
		else if (bSerializedValueAvailable && (Desc.ValueType == EPropertyBagPropertyType::Object ||
			Desc.ValueType == EPropertyBagPropertyType::SoftObject ||
			Desc.ValueType == EPropertyBagPropertyType::Class ||
			Desc.ValueType == EPropertyBagPropertyType::SoftClass ||
			Desc.ValueType == EPropertyBagPropertyType::Enum))
		{
			SetBoundedStringField(Row, TEXT("value"), SerializedValue);
			Row->SetStringField(TEXT("value_encoding"), TEXT("unreal_export_text"));
		}
		else if (Desc.ValueType != EPropertyBagPropertyType::String)
		{
			TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
				const_cast<FProperty*>(Desc.CachedProperty), ValuePtr, 0, 0);
			if (JsonValue.IsValid())
			{
				Row->SetField(TEXT("value"), JsonValue);
				Row->SetStringField(TEXT("value_encoding"), TEXT("json_scalar"));
			}
		}
	}
	else
	{
		Row->SetBoolField(TEXT("serialized_value_authoritative"), false);
		Row->SetStringField(TEXT("value_omitted_reason"), bSupported
			? TEXT("property_value_unavailable") : TEXT("non_scalar_or_unsupported_type"));
	}

	const bool bOverridden = Instance && Desc.CachedProperty && Instance->IsPropertyOverridden(Desc.CachedProperty);
	Row->SetBoolField(TEXT("overridden"), bOverridden);
	Row->SetBoolField(TEXT("different_from_default"), Instance && Desc.CachedProperty &&
		Instance->IsPropertyOverriddenAndNotDefault(Desc.CachedProperty));

	if (Instance && Instance->Graph)
	{
		const FInstancedPropertyBag* ParentValues = Instance->Graph->GetUserParametersStruct();
		const FPropertyBagPropertyDesc* ParentDesc = ParentValues ? ParentValues->FindPropertyDescByID(Desc.ID) : nullptr;
		if (ParentValues && ParentDesc)
		{
			FString ParentSerializedValue;
			bool bParentSerializedAvailable = false;
			if (ParentDesc->ValueType == EPropertyBagPropertyType::String && ParentDesc->CachedProperty &&
				ParentValues->GetValue().IsValid())
			{
				const void* ParentValuePtr = ParentDesc->CachedProperty->ContainerPtrToValuePtr<void>(
					ParentValues->GetValue().GetMemory());
				const FString& ParentStringValue = *static_cast<const FString*>(ParentValuePtr);
				if (ParentStringValue.Len() <= MaxUserParameterValueChars)
				{
					const TValueOrError<FString, EPropertyBagResult> ParentSerialized =
						ParentValues->GetValueSerializedString(ParentDesc->Name);
					if (ParentSerialized.IsValid())
					{
						ParentSerializedValue = ParentSerialized.GetValue();
						bParentSerializedAvailable = true;
					}
				}
			}
			else if (bSupported)
			{
				const TValueOrError<FString, EPropertyBagResult> ParentSerialized =
					ParentValues->GetValueSerializedString(ParentDesc->Name);
				if (ParentSerialized.IsValid())
				{
					ParentSerializedValue = ParentSerialized.GetValue();
					bParentSerializedAvailable = true;
				}
			}
			Row->SetBoolField(TEXT("default_serialized_value_authoritative"), bParentSerializedAvailable);
			if (bParentSerializedAvailable)
			{
				SetBoundedStringField(Row, TEXT("default_serialized_value"), ParentSerializedValue);
			}
		}
	}

	Row->SetBoolField(TEXT("set_override_supported"), bSupported);
	if (!bSupported)
	{
		Row->SetStringField(TEXT("unsupported_reason"), UnsupportedReason);
	}
	return Row;
}

TArray<TSharedPtr<FJsonValue>> BuildUserParameterRows(const UPCGGraphInstance* Instance, int32 Limit,
	int32& OutTotalCount, bool& bOutTruncated)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	OutTotalCount = 0;
	bOutTruncated = false;
	if (!Instance)
	{
		return Rows;
	}
	const FInstancedPropertyBag* Values = Instance->GetUserParametersStruct();
	const UPropertyBag* BagStruct = Values ? Values->GetPropertyBagStruct() : nullptr;
	if (!Values || !BagStruct)
	{
		return Rows;
	}

	TArray<const FPropertyBagPropertyDesc*> Descs;
	for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
	{
		Descs.Add(&Desc);
	}
	Descs.Sort([](const FPropertyBagPropertyDesc& A, const FPropertyBagPropertyDesc& B)
	{
		return A.Name.LexicalLess(B.Name);
	});
	OutTotalCount = Descs.Num();
	Rows.Reserve(FMath::Min(Limit, OutTotalCount));
	for (int32 Index = 0; Index < Descs.Num() && Rows.Num() < Limit; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(BuildUserParameterRow(Instance, *Values, *Descs[Index])));
	}
	bOutTruncated = Rows.Num() < OutTotalCount;
	return Rows;
}

TSharedPtr<FJsonObject> BuildComponentJson(const UPCGComponent* Component, bool bIncludeUserParameters = true,
	int32 UserParameterLimit = MaxUserParameters, bool bIncludeManagedResourceCount = false)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("component_lifecycle"));
	if (!Component)
	{
		Result->SetStringField(TEXT("status"), TEXT("invalid"));
		return Result;
	}

	const AActor* Actor = Component->GetOwner();
	const UPCGGraphInstance* GraphInstance = Component->GetGraphInstance();
	const UPCGGraphInterface* AssignedGraph = GraphInstance ? GraphInstance->Graph.Get() : nullptr;
	Result->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
	Result->SetStringField(TEXT("actor_name"), Actor ? Actor->GetName() : FString());
	Result->SetStringField(TEXT("actor_path"), Actor ? Actor->GetPathName() : FString());
	Result->SetStringField(TEXT("component_name"), Component->GetName());
	Result->SetStringField(TEXT("component_path"), Component->GetPathName());
	Result->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
	Result->SetStringField(TEXT("component_class_path"), Component->GetClass()->GetPathName());
	Result->SetBoolField(TEXT("local_component"), Component->IsLocalComponent());
	const APCGPartitionActor* PartitionActor = Cast<APCGPartitionActor>(Actor);
	const UPCGComponent* OriginalComponent = Component->IsLocalComponent() && PartitionActor
		? PartitionActor->GetOriginalComponent(Component) : nullptr;
	Result->SetStringField(TEXT("original_component_path"),
		OriginalComponent ? OriginalComponent->GetPathName() : FString());
	Result->SetBoolField(TEXT("registered"), Component->IsRegistered());
	Result->SetStringField(TEXT("creation_method"), StaticEnum<EComponentCreationMethod>()->GetNameStringByValue(
		static_cast<int64>(Component->CreationMethod)));
	Result->SetStringField(TEXT("world"), Component->GetWorld() ? Component->GetWorld()->GetPathName() : FString());
	Result->SetStringField(TEXT("assigned_graph_path"), AssignedGraph ? AssignedGraph->GetPathName() : FString());
	Result->SetStringField(TEXT("graph_path"), Component->GetGraph() ? Component->GetGraph()->GetPathName() : FString());
	Result->SetStringField(TEXT("graph_instance_path"), GraphInstance ? GraphInstance->GetPathName() : FString());
	Result->SetNumberField(TEXT("seed"), Component->Seed);
	Result->SetBoolField(TEXT("activated"), Component->bActivated);
	Result->SetBoolField(TEXT("partitioned"), Component->IsPartitioned());
	Result->SetStringField(TEXT("generation_trigger"), GenerationTriggerToString(Component->GenerationTrigger));
	Result->SetBoolField(TEXT("generate_on_drop_when_on_demand"), Component->bGenerateOnDropWhenTriggerOnDemand);
	Result->SetBoolField(TEXT("generated"), Component->bGenerated);
	Result->SetBoolField(TEXT("generating"), Component->IsGenerating());
	Result->SetBoolField(TEXT("cleaning_up"), Component->IsCleaningUp());
#if WITH_EDITOR
	Result->SetBoolField(TEXT("refresh_in_progress"), Component->IsRefreshInProgress());
	Result->SetBoolField(TEXT("generated_this_session"), Component->WasGeneratedThisSession());
#else
	Result->SetBoolField(TEXT("refresh_in_progress"), false);
	Result->SetBoolField(TEXT("generated_this_session"), false);
#endif
	Result->SetBoolField(TEXT("generated_offline"), Component->IsGeneratedOffline());
	AddTaskFields(Result, TEXT("generation"), Component->GetGenerationTaskId());
	AddTaskFields(Result, TEXT("cleanup"), Component->GetCleanupTaskId());

	const bool bOutputAccessible = !Component->IsGenerating() && !Component->IsCleaningUp()
#if WITH_EDITOR
		&& !Component->IsRefreshInProgress()
#endif
		;
	Result->SetBoolField(TEXT("output_accessible"), bOutputAccessible);
	Result->SetNumberField(TEXT("output_item_count"),
		bOutputAccessible ? Component->GetGeneratedGraphOutput().TaggedData.Num() : INDEX_NONE);
	const bool bResourcesAccessible = Component->AreManagedResourcesAccessible();
	Result->SetBoolField(TEXT("managed_resources_accessible"), bResourcesAccessible);
	Result->SetBoolField(TEXT("managed_resource_count_requested"), bIncludeManagedResourceCount);
	Result->SetNumberField(TEXT("managed_resource_count"),
		bResourcesAccessible && bIncludeManagedResourceCount ? CountManagedResources(Component) : INDEX_NONE);
	Result->SetObjectField(TEXT("last_generated_bounds"), BoxToJson(Component->GetLastGeneratedBounds()));

	FString Status;
	if (Component->IsCleaningUp())
	{
		Status = TEXT("cleaning_up");
	}
	else if (Component->IsGenerating())
	{
		Status = TEXT("generating");
	}
#if WITH_EDITOR
	else if (Component->IsRefreshInProgress())
	{
		Status = TEXT("refreshing");
	}
#endif
	else if (!Component->bActivated)
	{
		Status = TEXT("inactive");
	}
	else if (!Component->GetGraph())
	{
		Status = TEXT("unconfigured");
	}
	else if (Component->bGenerated)
	{
		Status = TEXT("generated");
	}
	else
	{
		Status = TEXT("ready");
	}
	Result->SetStringField(TEXT("status"), Status);

	if (bIncludeUserParameters)
	{
		int32 TotalCount = 0;
		bool bTruncated = false;
		Result->SetArrayField(TEXT("user_parameters"),
			BuildUserParameterRows(GraphInstance, UserParameterLimit, TotalCount, bTruncated));
		Result->SetNumberField(TEXT("user_parameter_count"), TotalCount);
		Result->SetNumberField(TEXT("user_parameter_limit"), UserParameterLimit);
		Result->SetBoolField(TEXT("user_parameters_truncated"), bTruncated);
	}
	return Result;
}

TSharedPtr<FJsonObject> CrcToJson(const FPCGCrc& Crc)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), Crc.IsValid());
	if (Crc.IsValid())
	{
		const uint32 Value = Crc.GetValue();
		Result->SetStringField(TEXT("value"), FString::Printf(TEXT("%u"), Value));
		Result->SetStringField(TEXT("hex"), FString::Printf(TEXT("0x%08x"), Value));
	}
	return Result;
}

TSharedPtr<FJsonObject> BuildLoadedObjectJson(const UObject* Object, const FString& Kind, const FString& SoftPath)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetStringField(TEXT("soft_object_path"), SoftPath);
	Result->SetBoolField(TEXT("loaded"), Object != nullptr);
	if (Object)
	{
		Result->SetStringField(TEXT("object_path"), Object->GetPathName());
		Result->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		}
		if (const UActorComponent* ActorComponent = Cast<UActorComponent>(Object))
		{
			Result->SetBoolField(TEXT("registered"), ActorComponent->IsRegistered());
			Result->SetStringField(TEXT("owner_path"),
				ActorComponent->GetOwner() ? ActorComponent->GetOwner()->GetPathName() : FString());
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> BuildManagedResourceJson(const UPCGManagedResource* Resource, int32 Index,
	int32 ObjectLimit)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("index"), Index);
	if (!Resource)
	{
		Result->SetStringField(TEXT("kind"), TEXT("null"));
		return Result;
	}

	Result->SetStringField(TEXT("resource_path"), Resource->GetPathName());
	Result->SetStringField(TEXT("resource_class"), Resource->GetClass()->GetName());
	Result->SetStringField(TEXT("resource_class_path"), Resource->GetClass()->GetPathName());
	Result->SetBoolField(TEXT("can_be_used"), Resource->CanBeUsed());
	Result->SetBoolField(TEXT("release_on_teardown"), Resource->ReleaseOnTeardown());
	Result->SetBoolField(TEXT("marked_unused"), Resource->IsMarkedUnused());
#if WITH_EDITOR
	Result->SetBoolField(TEXT("preview"), Resource->IsPreview());
	Result->SetBoolField(TEXT("transient_on_load"), Resource->IsMarkedTransientOnLoad());
#endif
	Result->SetObjectField(TEXT("crc"), CrcToJson(Resource->GetCrc()));

	TArray<TSharedPtr<FJsonValue>> Objects;
	int32 TotalObjects = 0;
	if (const UPCGManagedActors* Actors = Cast<UPCGManagedActors>(Resource))
	{
		Result->SetStringField(TEXT("kind"), TEXT("actors"));
		const TArray<TSoftObjectPtr<AActor>>& GeneratedActors = Actors->GetConstGeneratedActors();
		TotalObjects = GeneratedActors.Num();
		for (int32 ObjectIndex = 0; ObjectIndex < TotalObjects && ObjectIndex < ObjectLimit; ++ObjectIndex)
		{
			const TSoftObjectPtr<AActor>& SoftActor = GeneratedActors[ObjectIndex];
			Objects.Add(MakeShared<FJsonValueObject>(BuildLoadedObjectJson(
				SoftActor.Get(), TEXT("actor"), SoftActor.ToSoftObjectPath().ToString())));
		}
	}
	else if (const UPCGManagedComponentList* ComponentList = Cast<UPCGManagedComponentList>(Resource))
	{
		Result->SetStringField(TEXT("kind"), TEXT("component_list"));
		TotalObjects = ComponentList->GeneratedComponents.Num();
		for (int32 ObjectIndex = 0; ObjectIndex < TotalObjects && ObjectIndex < ObjectLimit; ++ObjectIndex)
		{
			const TSoftObjectPtr<UActorComponent>& SoftComponent = ComponentList->GeneratedComponents[ObjectIndex];
			Objects.Add(MakeShared<FJsonValueObject>(BuildLoadedObjectJson(
				SoftComponent.Get(), TEXT("component"), SoftComponent.ToSoftObjectPath().ToString())));
		}
	}
	else if (const UPCGManagedComponent* ManagedComponent = Cast<UPCGManagedComponent>(Resource))
	{
		Result->SetStringField(TEXT("kind"), TEXT("component"));
		const TSoftObjectPtr<UActorComponent>& SoftComponent = ManagedComponent->GeneratedComponent;
		if (!SoftComponent.IsNull())
		{
			TotalObjects = 1;
			if (ObjectLimit > 0)
			{
				Objects.Add(MakeShared<FJsonValueObject>(BuildLoadedObjectJson(
					SoftComponent.Get(), TEXT("component"), SoftComponent.ToSoftObjectPath().ToString())));
			}
		}
	}
	else
	{
		Result->SetStringField(TEXT("kind"), TEXT("other"));
	}
	Result->SetNumberField(TEXT("managed_object_count"), TotalObjects);
	Result->SetBoolField(TEXT("managed_objects_truncated"), Objects.Num() < TotalObjects);
	Result->SetArrayField(TEXT("managed_objects"), Objects);
	return Result;
}

TSharedPtr<FJsonObject> BuildOutputItemJson(const FPCGTaggedData& TaggedData, int32 Index,
	const FPCGCrc* CachedCrc, int32 TagLimit)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("index"), Index);
	Row->SetStringField(TEXT("pin"), TaggedData.Pin.ToString());
	Row->SetBoolField(TEXT("pinless"), TaggedData.bPinlessData);
	Row->SetBoolField(TEXT("used_multiple_times"), TaggedData.bIsUsedMultipleTimes);
	Row->SetNumberField(TEXT("original_index"), TaggedData.OriginalIndex);

	// Keep only the lexicographically smallest bounded prefix. Copying and sorting the
	// entire tag set would let one pathological output row defeat tag_limit.
	TArray<FString> SortedTags;
	SortedTags.Reserve(FMath::Min(TaggedData.Tags.Num(), TagLimit));
	for (const FString& CandidateTag : TaggedData.Tags)
	{
		if (SortedTags.Num() < TagLimit)
		{
			SortedTags.Add(CandidateTag);
			continue;
		}

		int32 LargestIndex = 0;
		for (int32 ExistingIndex = 1; ExistingIndex < SortedTags.Num(); ++ExistingIndex)
		{
			if (SortedTags[LargestIndex] < SortedTags[ExistingIndex])
			{
				LargestIndex = ExistingIndex;
			}
		}
		if (CandidateTag < SortedTags[LargestIndex])
		{
			SortedTags[LargestIndex] = CandidateTag;
		}
	}
	SortedTags.Sort();
	TArray<TSharedPtr<FJsonValue>> Tags;
	Tags.Reserve(SortedTags.Num());
	for (const FString& Tag : SortedTags)
	{
		Tags.Add(MakeShared<FJsonValueString>(Tag));
	}
	Row->SetArrayField(TEXT("tags"), Tags);
	Row->SetNumberField(TEXT("tag_count"), TaggedData.Tags.Num());
	Row->SetNumberField(TEXT("tag_returned"), Tags.Num());
	Row->SetNumberField(TEXT("tag_limit"), TagLimit);
	Row->SetBoolField(TEXT("tags_truncated"), Tags.Num() < TaggedData.Tags.Num());
	if (CachedCrc)
	{
		Row->SetObjectField(TEXT("cached_crc"), CrcToJson(*CachedCrc));
	}

	const UPCGData* Data = TaggedData.Data.Get();
	TSharedPtr<FJsonObject> DataJson = MakeShared<FJsonObject>();
	DataJson->SetBoolField(TEXT("valid"), Data != nullptr);
	if (Data)
	{
		DataJson->SetStringField(TEXT("object_path"), Data->GetPathName());
		DataJson->SetStringField(TEXT("class_path"), Data->GetClass()->GetPathName());
		DataJson->SetStringField(TEXT("uid"), FString::Printf(TEXT("%llu"),
			static_cast<unsigned long long>(Data->UID)));
		DataJson->SetStringField(TEXT("data_type_id"), Data->GetDataTypeId().ToString());
		DataJson->SetStringField(TEXT("underlying_data_type_id"), Data->GetUnderlyingDataTypeId().ToString());
		if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data))
		{
			DataJson->SetNumberField(TEXT("point_count"), PointData->GetNumPoints());
			DataJson->SetObjectField(TEXT("bounds"), BoxToJson(PointData->GetBounds()));
		}
		else if (const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Data))
		{
			DataJson->SetNumberField(TEXT("dimension"), SpatialData->GetDimension());
			DataJson->SetBoolField(TEXT("bounded"), SpatialData->IsBounded());
			DataJson->SetObjectField(TEXT("bounds"), BoxToJson(SpatialData->GetBounds()));
		}
	}
	Row->SetObjectField(TEXT("data"), DataJson);
	return Row;
}

bool RollbackNewComponent(AActor* Actor, UPCGComponent* Component, const FDirtySnapshot& DirtySnapshot)
{
	if (Component && IsValid(Component))
	{
		if (Component->IsRegistered())
		{
			Component->UnregisterComponent();
		}
		Component->DestroyComponent();
	}
	const bool bRollbackComplete = !Actor || !Component || !Actor->GetInstanceComponents().Contains(Component);
	FinalizeRollbackDirtyState(bRollbackComplete, Actor, Component, DirtySnapshot);
	return bRollbackComplete;
}

bool ApplyComponentPropertyEvent(UPCGComponent*& InOutComponent, const FString& ComponentPath,
	const FName PropertyName, TFunctionRef<void(UPCGComponent*)> ApplyValue, FString& OutError)
{
	UPCGComponent* Component = InOutComponent;
	if (!IsValid(Component))
	{
		OutError = TEXT("The PCG component became invalid before applying an editor property event");
		return false;
	}
	FProperty* Property = FindFProperty<FProperty>(UPCGComponent::StaticClass(), PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("UPCGComponent property '%s' is unavailable in this engine build"),
			*PropertyName.ToString());
		return false;
	}
#if WITH_EDITOR
	if (!Component->CanEditChange(Property))
	{
		OutError = FString::Printf(TEXT("UPCGComponent property '%s' is not editable for %s"),
			*PropertyName.ToString(), *ComponentPath);
		return false;
	}
#endif
	// PostEditChangeProperty may reconstruct a Blueprint-owned component. Capture
	// every freshly resolved instance in the transaction before its next edit.
	Component->SetFlags(RF_Transactional);
	Component->Modify();
	// UPCGComponent narrows the access of its editor notification overrides in UE 5.8.
	// Invoke the public UObject contract while retaining virtual dispatch to the component.
	UObject* EditableObject = Component;
	EditableObject->PreEditChange(Property);
	ApplyValue(Component);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	EditableObject->PostEditChangeProperty(Event);

	FMonolithActionResult ResolveError;
	InOutComponent = ResolveComponentExact(ComponentPath, ResolveError);
	if (!InOutComponent)
	{
		OutError = FString::Printf(TEXT("UPCGComponent reconstruction after '%s' could not be resolved at %s: %s"),
			*PropertyName.ToString(), *ComponentPath, *ResolveError.ErrorMessage);
		return false;
	}
	return true;
}
} // namespace MonolithPCGComponent

void FMonolithPCGComponentActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithPCGComponent;

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("create_component"),
		TEXT("Create a persistent editor-instance UPCGComponent on an exact actor path, optionally assign a graph, and optionally save the owning level."),
		FMonolithActionHandler::CreateStatic(&CreateComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Exact actor object path from the active editor world"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Requested component object name"), TEXT("PCGComponent"))
			.Optional(TEXT("existing_policy"), TEXT("string"), TEXT("fail or return_existing"), TEXT("fail"))
			.Optional(TEXT("graph_asset_path"), TEXT("string"), TEXT("Optional UPCGGraphInterface asset path"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Initial 32-bit seed"), TEXT("42"))
			.Optional(TEXT("activated"), TEXT("bool"), TEXT("Initial activation state"), TEXT("true"))
			.Optional(TEXT("partitioned"), TEXT("bool"), TEXT("Initial partitioned state"), TEXT("false"))
			.Optional(TEXT("generation_trigger"), TEXT("string"), TEXT("on_load, on_demand, or at_runtime"), TEXT("on_demand"))
			.Optional(TEXT("generate_on_drop_when_on_demand"), TEXT("bool"), TEXT("Generate on actor drop when trigger is on_demand"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the owning level after verified creation"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), TransactionPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("get_component"),
		TEXT("Inspect one typed UPCGComponent by exact component path, including lifecycle state, graph assignment, task IDs, and user-parameter overrides."),
		FMonolithActionHandler::CreateStatic(&GetComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path from pcg.list_components"))
			.Optional(TEXT("include_user_parameters"), TEXT("bool"), TEXT("Include bounded graph-instance user parameters"), TEXT("true"))
			.Optional(TEXT("user_parameter_limit"), TEXT("integer"), TEXT("Maximum user-parameter rows (1-256)"), TEXT("256"))
			.Optional(TEXT("include_managed_resource_count"), TEXT("bool"), TEXT("Explicitly enumerate managed resources for an exact count"), TEXT("false"))
			.Build(),
		TEXT("Component Lifecycle"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_component_graph"),
		TEXT("Assign a UPCGGraphInterface asset to an idle, ungenerated component by exact path and optionally save the owning level."),
		FMonolithActionHandler::CreateStatic(&SetComponentGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.RequiredAssetPath(TEXT("graph_asset_path"), TEXT("UPCGGraphInterface asset path"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the owning level after verified assignment"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), TransactionPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_blueprint_component_graph"),
		TEXT("Assign a UPCGGraphInterface to one exact UPCGComponent template in a project-owned Actor Blueprint. Dry-run defaults true; commit requires confirm=true."),
		FMonolithActionHandler::CreateStatic(&SetBlueprintComponentGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("blueprint_asset_path"), TEXT("Project-owned Actor Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Exact SCS variable name of one UPCGComponent template"))
			.RequiredAssetPath(TEXT("graph_asset_path"), TEXT("UPCGGraphInterface asset path"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without mutation"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for a mutating commit"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the Blueprint package after verified assignment"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), TransactionPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_component_settings"),
		TEXT("Atomically validate and edit explicit UPCGComponent settings on an idle, ungenerated component, then optionally save its level."),
		FMonolithActionHandler::CreateStatic(&SetComponentSettings),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("32-bit component seed"))
			.Optional(TEXT("activated"), TEXT("bool"), TEXT("Component activation state"))
			.Optional(TEXT("partitioned"), TEXT("bool"), TEXT("Partition component generation"))
			.Optional(TEXT("generation_trigger"), TEXT("string"), TEXT("on_load, on_demand, or at_runtime"))
			.Optional(TEXT("generate_on_drop_when_on_demand"), TEXT("bool"), TEXT("Generate on actor drop when trigger is on_demand"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the owning level after verified edits"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), TransactionPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("generate_component"),
		TEXT("Schedule non-blocking on-demand generation for one exact component and return its uint64 task ID as a decimal string."),
		FMonolithActionHandler::CreateStatic(&GenerateComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("Force regeneration even when already generated"), TEXT("false"))
			.Build(),
		TEXT("Component Lifecycle"), AsyncMutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("refresh_component"),
		TEXT("Schedule or coalesce a non-blocking editor refresh for a previously generated exact component; poll get_component for completion."),
		FMonolithActionHandler::CreateStatic(&RefreshComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Build(),
		TEXT("Component Lifecycle"), AsyncMutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("cancel_component"),
		TEXT("Cancel in-progress generation for one exact component without blocking the editor thread."),
		FMonolithActionHandler::CreateStatic(&CancelComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Build(),
		TEXT("Component Lifecycle"), AsyncMutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("cleanup_component"),
		TEXT("Schedule non-blocking cleanup of generated resources for one exact component and return the uint64 task ID as a decimal string."),
		FMonolithActionHandler::CreateStatic(&CleanupComponent),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Optional(TEXT("remove_components"), TEXT("bool"), TEXT("Remove generated components instead of retaining them for reuse"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), AsyncMutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("get_component_output"),
		TEXT("Inspect bounded generated data and managed resources for an idle exact component without computing new CRCs or loading soft references."),
		FMonolithActionHandler::CreateStatic(&GetComponentOutput),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Optional(TEXT("output_limit"), TEXT("integer"), TEXT("Maximum tagged output rows (1-500)"), TEXT("100"))
			.Optional(TEXT("tag_limit"), TEXT("integer"), TEXT("Maximum sorted tags returned per output row (1-500)"), TEXT("100"))
			.Optional(TEXT("include_managed_resources"), TEXT("bool"), TEXT("Include bounded managed-resource rows"), TEXT("true"))
			.Optional(TEXT("resource_limit"), TEXT("integer"), TEXT("Maximum managed-resource rows (1-500)"), TEXT("100"))
			.Optional(TEXT("managed_object_limit"), TEXT("integer"), TEXT("Maximum managed objects per resource (1-500)"), TEXT("100"))
			.Build(),
		TEXT("Component Lifecycle"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_component_user_parameters"),
		TEXT("Atomically stage, validate, apply, reset, and read back scalar graph-instance user-parameter overrides on an idle exact component."),
		FMonolithActionHandler::CreateStatic(&SetComponentUserParameters),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("component_path"), TEXT("string"), TEXT("Exact UPCGComponent object path"))
			.Optional(TEXT("values"), TEXT("object"), TEXT("Map of parameter names to strict JSON scalar values"))
			.Optional(TEXT("reset"), TEXT("array"), TEXT("Parameter names whose overrides should be removed"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without mutation"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the owning level after verified mutation"), TEXT("true"))
			.Build(),
		TEXT("Component Lifecycle"), TransactionPolicy());

	Registry.SetActionSearchMetadata(TEXT("pcg"), TEXT("create_component"),
		{TEXT("PCG component creation"), TEXT("attach graph to actor"), TEXT("persistent instance component")},
		{TEXT("add PCG component"), TEXT("create procedural component")},
		{TEXT("create an on-demand PCG component on an actor and assign a graph")});
	Registry.SetActionPlanningMetadata(TEXT("pcg"), TEXT("generate_component"), TEXT("unreal-pcg"),
		{TEXT("Component is registered, active, idle, has a graph, and is not runtime-scheduler managed")},
		{TEXT("Scheduled task ID string followed by bounded get_component polling")},
		{TEXT("pcg.get_component"), TEXT("pcg.get_component_output"), TEXT("pcg.cleanup_component")});
	Registry.SetActionPlanningMetadata(TEXT("pcg"), TEXT("set_component_user_parameters"), TEXT("unreal-pcg"),
		{TEXT("Assign a graph first and cleanup generated state before changing overrides")},
		{TEXT("Atomic staged validation, canonical readback, and override flags")},
		{TEXT("pcg.get_component"), TEXT("pcg.generate_component")});
}

namespace MonolithPCGComponent
{
bool ReadOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FString& DefaultValue,
	FString& OutValue, FMonolithActionResult& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(Field);
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::String || !JsonValue->TryGetString(OutValue))
	{
		OutError = InvalidParam(Field, FString::Printf(TEXT("%s must be a string"), Field));
		return false;
	}
	OutValue.TrimStartAndEndInline();
	return true;
}

void MarkComponentMutationDirty(UPCGComponent* Component)
{
	if (!Component)
	{
		return;
	}
	Component->MarkPackageDirty();
	if (AActor* Actor = Component->GetOwner())
	{
		Actor->MarkPackageDirty();
	}
}

TSharedPtr<FJsonObject> BuildMutationResult(UPCGComponent* Component, const FString& Operation, bool bChanged,
	bool bSaved = false, const FString& SavedFilename = FString())
{
	TSharedPtr<FJsonObject> Result = BuildComponentJson(Component, false);
	Result->SetStringField(TEXT("operation"), Operation);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("saved_filename"), SavedFilename);
#if WITH_EDITOR
	Result->SetBoolField(TEXT("refresh_pending"), Component && Component->IsRefreshInProgress());
#else
	Result->SetBoolField(TEXT("refresh_pending"), false);
#endif
	return Result;
}

bool PreflightSaveIfRequested(const AActor* Actor, bool bSave, FMonolithActionResult& OutError)
{
	if (!bSave)
	{
		return true;
	}
	FString SaveError;
	if (!PreflightLevelSave(Actor, SaveError))
	{
		OutError = InvalidParam(TEXT("save"), SaveError);
		return false;
	}
	return true;
}

FMonolithActionResult SaveComponentMutation(UPCGComponent* Component, const FString& Operation, bool bChanged,
	bool bSave)
{
	if (!bSave)
	{
		return FMonolithActionResult::Success(BuildMutationResult(Component, Operation, bChanged));
	}

	FString SavedFilename;
	FString SaveError;
	if (!Component || !SaveOwningLevel(Component->GetOwner(), SavedFilename, SaveError))
	{
		return FMonolithActionResult::Error(SaveError.IsEmpty()
			? TEXT("The PCG component became invalid before its owning level could be saved")
			: SaveError)
			.WithErrorData(BuildMutationResult(Component, Operation, bChanged));
	}
	return FMonolithActionResult::Success(BuildMutationResult(Component, Operation, bChanged, true, SavedFilename));
}

struct FComponentSettingsSnapshot
{
	int32 Seed = 42;
	bool bActivated = true;
	bool bPartitioned = false;
	EPCGComponentGenerationTrigger GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	bool bGenerateOnDropWhenOnDemand = false;

	explicit FComponentSettingsSnapshot(const UPCGComponent* Component)
	{
		if (Component)
		{
			Seed = Component->Seed;
			bActivated = Component->bActivated;
			bPartitioned = Component->bIsComponentPartitioned;
			GenerationTrigger = Component->GenerationTrigger;
			bGenerateOnDropWhenOnDemand = Component->bGenerateOnDropWhenTriggerOnDemand;
		}
	}

	bool Matches(const UPCGComponent* Component) const
	{
		return Component && Component->Seed == Seed && Component->bActivated == bActivated &&
			Component->bIsComponentPartitioned == bPartitioned && Component->GenerationTrigger == GenerationTrigger &&
			Component->bGenerateOnDropWhenTriggerOnDemand == bGenerateOnDropWhenOnDemand;
	}
};

bool ApplySettingsSnapshot(UPCGComponent*& Component, const FString& ComponentPath,
	const FComponentSettingsSnapshot& Target, FString& OutError)
{
	if (Component->Seed != Target.Seed &&
		!ApplyComponentPropertyEvent(Component, ComponentPath, GET_MEMBER_NAME_CHECKED(UPCGComponent, Seed),
			[&Target](UPCGComponent* Current) { Current->Seed = Target.Seed; }, OutError))
	{
		return false;
	}
	if (Component->bActivated != Target.bActivated &&
		!ApplyComponentPropertyEvent(Component, ComponentPath, GET_MEMBER_NAME_CHECKED(UPCGComponent, bActivated),
			[&Target](UPCGComponent* Current) { Current->bActivated = Target.bActivated; }, OutError))
	{
		return false;
	}
	if (Component->bIsComponentPartitioned != Target.bPartitioned &&
		!ApplyComponentPropertyEvent(Component, ComponentPath,
			GET_MEMBER_NAME_CHECKED(UPCGComponent, bIsComponentPartitioned),
			[&Target](UPCGComponent* Current) { Current->bIsComponentPartitioned = Target.bPartitioned; }, OutError))
	{
		return false;
	}
	if (Component->GenerationTrigger != Target.GenerationTrigger &&
		!ApplyComponentPropertyEvent(Component, ComponentPath, GET_MEMBER_NAME_CHECKED(UPCGComponent, GenerationTrigger),
			[&Target](UPCGComponent* Current) { Current->GenerationTrigger = Target.GenerationTrigger; }, OutError))
	{
		return false;
	}
	if (Component->bGenerateOnDropWhenTriggerOnDemand != Target.bGenerateOnDropWhenOnDemand &&
		!ApplyComponentPropertyEvent(Component, ComponentPath,
			GET_MEMBER_NAME_CHECKED(UPCGComponent, bGenerateOnDropWhenTriggerOnDemand),
			[&Target](UPCGComponent* Current)
			{
				Current->bGenerateOnDropWhenTriggerOnDemand = Target.bGenerateOnDropWhenOnDemand;
			}, OutError))
	{
		return false;
	}
	return Target.Matches(Component);
}
} // namespace MonolithPCGComponent

FMonolithActionResult FMonolithPCGComponentActions::CreateComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	FString ActorPath;
	if (!ReadRequiredString(Params, TEXT("actor_path"), ActorPath, Error))
	{
		return Error;
	}
	AActor* Actor = ResolveActorExact(ActorPath, Error);
	if (!Actor)
	{
		return Error;
	}
	if (Actor->IsA<APCGPartitionActor>())
	{
		return InvalidParam(TEXT("actor_path"),
			TEXT("APCGPartitionActor is engine-owned; create PCG components on an original user-authored actor"));
	}
#if WITH_EDITOR
	if (!Actor->IsEditable() || (Actor->IsInLevelInstance() && !Actor->IsInEditLevelInstance()))
	{
		return InvalidParam(TEXT("actor_path"),
			TEXT("actor_path is not editable in the current editor or Level Instance context"));
	}
#endif

	FString ComponentName;
	FString ExistingPolicy;
	FString TriggerString;
	int32 Seed = 42;
	bool bActivated = true;
	bool bPartitioned = false;
	bool bGenerateOnDrop = false;
	bool bSave = true;
	if (!ReadOptionalString(Params, TEXT("component_name"), TEXT("PCGComponent"), ComponentName, Error) ||
		!ReadOptionalString(Params, TEXT("existing_policy"), TEXT("fail"), ExistingPolicy, Error) ||
		!ReadOptionalString(Params, TEXT("generation_trigger"), TEXT("on_demand"), TriggerString, Error) ||
		!ReadOptionalInt32(Params, TEXT("seed"), 42, Seed, Error) ||
		!ReadOptionalBool(Params, TEXT("activated"), true, bActivated, Error) ||
		!ReadOptionalBool(Params, TEXT("partitioned"), false, bPartitioned, Error) ||
		!ReadOptionalBool(Params, TEXT("generate_on_drop_when_on_demand"), false, bGenerateOnDrop, Error) ||
		!ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return Error;
	}

	const FName RequestedName(*ComponentName);
	if (ComponentName.IsEmpty() || RequestedName.IsNone() || !RequestedName.IsValidXName())
	{
		return InvalidParam(TEXT("component_name"), FString::Printf(
			TEXT("component_name must be a non-empty valid UObject name, not '%s'"), *ComponentName));
	}
	ExistingPolicy.ToLowerInline();
	if (ExistingPolicy != TEXT("fail") && ExistingPolicy != TEXT("return_existing"))
	{
		return InvalidParam(TEXT("existing_policy"), TEXT("existing_policy must be 'fail' or 'return_existing'"));
	}
	EPCGComponentGenerationTrigger Trigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	if (!ParseGenerationTrigger(TriggerString, Trigger))
	{
		return InvalidParam(TEXT("generation_trigger"),
			TEXT("generation_trigger must be on_load, on_demand, or at_runtime"));
	}
	if (bGenerateOnDrop && Trigger != EPCGComponentGenerationTrigger::GenerateOnDemand)
	{
		return InvalidParam(TEXT("generate_on_drop_when_on_demand"),
			TEXT("generate_on_drop_when_on_demand=true requires generation_trigger=on_demand"));
	}

	UPCGGraphInterface* RequestedGraph = nullptr;
	FString ResolvedGraphPath;
	if (Params.IsValid() && Params->HasField(TEXT("graph_asset_path")))
	{
		FString GraphAssetPath;
		if (!ReadRequiredString(Params, TEXT("graph_asset_path"), GraphAssetPath, Error) ||
			!LoadGraphInterface(GraphAssetPath, RequestedGraph, ResolvedGraphPath, Error))
		{
			return Error;
		}
		if (!RequestedGraph || !RequestedGraph->GetGraph())
		{
			return InvalidParam(TEXT("graph_asset_path"),
				TEXT("graph_asset_path resolved to a PCG graph interface without an underlying graph"));
		}
	}

	if (UObject* Collision = StaticFindObjectFast(UObject::StaticClass(), Actor, RequestedName))
	{
		UPCGComponent* Existing = Cast<UPCGComponent>(Collision);
		if (!Existing)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("component_name collides with existing non-PCG object %s (%s)"),
				*Collision->GetPathName(), *Collision->GetClass()->GetPathName()));
		}
		if (ExistingPolicy == TEXT("fail"))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("A UPCGComponent already exists with the requested exact name: %s"), *Existing->GetPathName()))
				.WithErrorData(BuildComponentJson(Existing, false));
		}

		const UPCGGraphInstance* ExistingInstance = Existing->GetGraphInstance();
		const UPCGGraphInterface* ExistingGraph = ExistingInstance ? ExistingInstance->Graph.Get() : nullptr;
		const bool bMatches = Existing->GetOwner() == Actor && Existing->CreationMethod == EComponentCreationMethod::Instance &&
			Actor->GetInstanceComponents().Contains(Existing) && Existing->IsRegistered() && ExistingGraph == RequestedGraph &&
			Existing->Seed == Seed && Existing->bActivated == bActivated &&
			Existing->IsPartitioned() == bPartitioned && Existing->GenerationTrigger == Trigger &&
			Existing->bGenerateOnDropWhenTriggerOnDemand == bGenerateOnDrop;
		if (!bMatches)
		{
			return FMonolithActionResult::Error(
				TEXT("existing_policy=return_existing refused a component whose persisted configuration does not exactly match the request"))
				.WithErrorData(BuildComponentJson(Existing, false));
		}
		if (bSave && !RequireIdleUngenerated(Existing, TEXT("pcg.create_component"), Error))
		{
			return Error;
		}
		if (!PreflightSaveIfRequested(Actor, bSave, Error))
		{
			return Error;
		}
		TSharedPtr<FJsonObject> SourceControlPrepare;
		if (bSave && !PrepareSourceControlBeforeMutation(
				Existing, TEXT("pcg.create_component"), SourceControlPrepare, Error))
		{
			return Error;
		}
		FMonolithActionResult ExistingResult = SaveComponentMutation(
			Existing, TEXT("create_component"), false, bSave);
		if (!ExistingResult.bSuccess || !ExistingResult.Result.IsValid())
		{
			return AttachSourceControlPrepare(MoveTemp(ExistingResult), SourceControlPrepare);
		}
		TSharedPtr<FJsonObject> Result = ExistingResult.Result;
		Result->SetBoolField(TEXT("created"), false);
		Result->SetBoolField(TEXT("returned_existing"), true);
		Result->SetStringField(TEXT("resolved_graph_asset_path"), ResolvedGraphPath);
		return AttachSourceControlPrepare(MoveTemp(ExistingResult), SourceControlPrepare);
	}

	if (!PreflightSaveIfRequested(Actor, bSave, Error))
	{
		return Error;
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Actor, TEXT("pcg.create_component"), SourceControlPrepare, Error))
	{
		return Error;
	}

	const FDirtySnapshot DirtySnapshot(Actor);
	Actor->Modify();
	UPCGComponent* Component = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), RequestedName, RF_Transactional);
	if (!Component)
	{
		FinalizeRollbackDirtyState(/*bRollbackComplete=*/true, Actor, nullptr, DirtySnapshot);
		return AttachSourceControlPrepare(
			FMonolithActionResult::Error(TEXT("NewObject failed to create UPCGComponent")),
			SourceControlPrepare);
	}
	Component->Modify();
	Component->Seed = Seed;
	Component->bActivated = bActivated;
	Component->GenerationTrigger = Trigger;
	Component->bGenerateOnDropWhenTriggerOnDemand = bGenerateOnDrop;
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	if (bPartitioned)
	{
		if (!Component->CanPartition())
		{
			const bool bRollbackComplete = RollbackNewComponent(Actor, Component, DirtySnapshot);
			return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
				TEXT("The target actor cannot host a partitioned PCG component; rollback_complete=%s"),
				bRollbackComplete ? TEXT("true") : TEXT("false"))), SourceControlPrepare);
		}
		Component->SetIsPartitioned(true);
		if (!Component->IsPartitioned())
		{
			const bool bRollbackComplete = RollbackNewComponent(Actor, Component, DirtySnapshot);
			return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
				TEXT("UE PCG refused the requested partitioned state; rollback_complete=%s"),
				bRollbackComplete ? TEXT("true") : TEXT("false"))), SourceControlPrepare);
		}
	}
	if (RequestedGraph)
	{
		UPCGGraphInstance* NewGraphInstance = Component->GetGraphInstance();
		if (!NewGraphInstance || !NewGraphInstance->CanGraphInterfaceBeSet(RequestedGraph))
		{
			const bool bRollbackComplete = RollbackNewComponent(Actor, Component, DirtySnapshot);
			return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
				TEXT("graph_asset_path would create an invalid graph-instance cycle; rollback_complete=%s"),
				bRollbackComplete ? TEXT("true") : TEXT("false"))), SourceControlPrepare);
		}
		Component->SetGraphLocal(RequestedGraph);
	}

	const UPCGGraphInstance* GraphInstance = Component->GetGraphInstance();
	const bool bPostcondition = IsValid(Component) && Component->GetOwner() == Actor &&
		Component->CreationMethod == EComponentCreationMethod::Instance &&
		Actor->GetInstanceComponents().Contains(Component) && Component->IsRegistered() &&
		Component->Seed == Seed && Component->bActivated == bActivated &&
		Component->IsPartitioned() == bPartitioned && Component->GenerationTrigger == Trigger &&
		Component->bGenerateOnDropWhenTriggerOnDemand == bGenerateOnDrop && GraphInstance &&
		GraphInstance->Graph.Get() == RequestedGraph;
	if (!bPostcondition)
	{
		const bool bRollbackComplete = RollbackNewComponent(Actor, Component, DirtySnapshot);
		return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
			TEXT("PCG component creation failed postcondition validation; rollback_complete=%s"),
			bRollbackComplete ? TEXT("true") : TEXT("false"))), SourceControlPrepare);
	}

	MarkComponentMutationDirty(Component);
	FMonolithActionResult Result = SaveComponentMutation(Component, TEXT("create_component"), true, bSave);
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetBoolField(TEXT("created"), true);
		Result.Result->SetBoolField(TEXT("returned_existing"), false);
		Result.Result->SetStringField(TEXT("resolved_graph_asset_path"), ResolvedGraphPath);
	}
	return AttachSourceControlPrepare(MoveTemp(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::GetComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	bool bIncludeUserParameters = true;
	bool bIncludeManagedResourceCount = false;
	int32 UserParameterLimit = MaxUserParameters;
	if (!ReadOptionalBool(Params, TEXT("include_user_parameters"), true, bIncludeUserParameters, Error) ||
		!ReadOptionalBoundedInt(Params, TEXT("user_parameter_limit"), MaxUserParameters, 1,
			MaxUserParameters, UserParameterLimit, Error) ||
		!ReadOptionalBool(Params, TEXT("include_managed_resource_count"), false,
			bIncludeManagedResourceCount, Error))
	{
		return Error;
	}
	return FMonolithActionResult::Success(
		BuildComponentJson(Component, bIncludeUserParameters, UserParameterLimit, bIncludeManagedResourceCount));
}

FMonolithActionResult FMonolithPCGComponentActions::SetComponentGraph(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	FString ComponentPath;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error, &ComponentPath);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.set_component_graph"), Error))
	{
		return Error;
	}
	FString GraphAssetPath;
	bool bSave = true;
	if (!ReadRequiredString(Params, TEXT("graph_asset_path"), GraphAssetPath, Error) ||
		!ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return Error;
	}
	UPCGGraphInterface* NewGraph = nullptr;
	FString ResolvedGraphPath;
	if (!LoadGraphInterface(GraphAssetPath, NewGraph, ResolvedGraphPath, Error))
	{
		return Error;
	}
	if (!NewGraph || !NewGraph->GetGraph())
	{
		return InvalidParam(TEXT("graph_asset_path"),
			TEXT("graph_asset_path resolved to a PCG graph interface without an underlying graph"));
	}

	UPCGGraphInstance* GraphInstance = Component->GetGraphInstance();
	if (!GraphInstance)
	{
		return FMonolithActionResult::Error(TEXT("The target PCG component has no graph instance"));
	}
	UPCGGraphInterface* PreviousGraph = GraphInstance->Graph.Get();
	if (PreviousGraph == NewGraph)
	{
		if (bSave && !RequireIdleUngenerated(Component, TEXT("pcg.set_component_graph"), Error))
		{
			return Error;
		}
		if (!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
		{
			return Error;
		}
		TSharedPtr<FJsonObject> SourceControlPrepare;
		if (bSave && !PrepareSourceControlBeforeMutation(
				Component, TEXT("pcg.set_component_graph"), SourceControlPrepare, Error))
		{
			return Error;
		}
		FMonolithActionResult NoChangeResult = SaveComponentMutation(
			Component, TEXT("set_component_graph"), false, bSave);
		if (!NoChangeResult.bSuccess || !NoChangeResult.Result.IsValid())
		{
			return AttachSourceControlPrepare(MoveTemp(NoChangeResult), SourceControlPrepare);
		}
		TSharedPtr<FJsonObject> Result = NoChangeResult.Result;
		Result->SetStringField(TEXT("resolved_graph_asset_path"), ResolvedGraphPath);
		return AttachSourceControlPrepare(MoveTemp(NoChangeResult), SourceControlPrepare);
	}
	if (!GraphInstance->CanGraphInterfaceBeSet(NewGraph))
	{
		return InvalidParam(TEXT("graph_asset_path"),
			TEXT("graph_asset_path would create a recursive UPCGGraphInstance chain"));
	}
#if WITH_EDITOR
	FProperty* GraphProperty = FindFProperty<FProperty>(UPCGGraphInstance::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UPCGGraphInstance, Graph));
	if (!GraphProperty || !GraphInstance->CanEditChange(GraphProperty))
	{
		return FMonolithActionResult::Error(
			TEXT("The graph assignment is not editable for this component (local or locked Level Instance)"));
	}
#endif
	if (!RequireIdleUngenerated(Component, TEXT("pcg.set_component_graph"), Error) ||
		!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
	{
		return Error;
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.set_component_graph"), SourceControlPrepare, Error))
	{
		return Error;
	}

	AActor* Actor = Component->GetOwner();
	const FDirtySnapshot DirtySnapshot(Actor);
	Actor->Modify();
	Component->Modify();
	GraphInstance->Modify();
	Component->SetGraphLocal(NewGraph);

	FMonolithActionResult ResolveError;
	Component = ResolveComponentExact(ComponentPath, ResolveError);
	const bool bVerified = Component && Component->GetGraphInstance() &&
		Component->GetGraphInstance()->Graph.Get() == NewGraph;
	if (!bVerified)
	{
		UPCGComponent* RollbackComponent = Component;
		if (!RollbackComponent)
		{
			RollbackComponent = ResolveComponentExact(ComponentPath, ResolveError);
		}
		bool bRollbackComplete = false;
		if (RollbackComponent && RollbackComponent->GetGraphInstance() &&
			RollbackComponent->GetGraphInstance()->CanGraphInterfaceBeSet(PreviousGraph))
		{
			RollbackComponent->SetGraphLocal(PreviousGraph);
			FMonolithActionResult RollbackResolveError;
			RollbackComponent = ResolveComponentExact(ComponentPath, RollbackResolveError);
			bRollbackComplete = RollbackComponent && RollbackComponent->GetGraphInstance() &&
				RollbackComponent->GetGraphInstance()->Graph.Get() == PreviousGraph;
		}
		FinalizeRollbackDirtyState(bRollbackComplete, Actor, RollbackComponent, DirtySnapshot);
		return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
			TEXT("PCG graph assignment failed read-back validation; rollback_complete=%s"),
			bRollbackComplete ? TEXT("true") : TEXT("false")))
			.WithErrorData(BuildComponentJson(RollbackComponent, false)), SourceControlPrepare);
	}

	MarkComponentMutationDirty(Component);
	FMonolithActionResult Result = SaveComponentMutation(Component, TEXT("set_component_graph"), true, bSave);
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetStringField(TEXT("previous_graph_asset_path"), PreviousGraph ? PreviousGraph->GetPathName() : FString());
		Result.Result->SetStringField(TEXT("resolved_graph_asset_path"), ResolvedGraphPath);
	}
	return AttachSourceControlPrepare(MoveTemp(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::SetBlueprintComponentGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	FString BlueprintAssetPath;
	FString ComponentName;
	FString GraphAssetPath;
	bool bDryRun = true;
	bool bConfirm = false;
	bool bSave = true;
	if (!ReadRequiredString(Params, TEXT("blueprint_asset_path"), BlueprintAssetPath, Error) ||
		!ReadRequiredString(Params, TEXT("component_name"), ComponentName, Error) ||
		!ReadRequiredString(Params, TEXT("graph_asset_path"), GraphAssetPath, Error) ||
		!ReadOptionalBool(Params, TEXT("dry_run"), true, bDryRun, Error) ||
		!ReadOptionalBool(Params, TEXT("confirm"), false, bConfirm, Error) ||
		!ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return Error;
	}

	UBlueprint* Blueprint = nullptr;
	UPCGComponent* Component = nullptr;
	FString ResolvedBlueprintPath;
	if (!ResolveBlueprintPCGComponentTemplate(
		BlueprintAssetPath, ComponentName, Blueprint, Component, ResolvedBlueprintPath, Error))
	{
		return Error;
	}
	const FString CanonicalBlueprintPath = ResolvedBlueprintPath;
	UPCGGraphInterface* RequestedGraph = nullptr;
	FString ResolvedGraphPath;
	if (!LoadGraphInterface(GraphAssetPath, RequestedGraph, ResolvedGraphPath, Error))
	{
		return Error;
	}
	if (!RequestedGraph || !RequestedGraph->GetGraph())
	{
		return InvalidParam(
			TEXT("graph_asset_path"),
			TEXT("graph_asset_path resolved to a PCG graph interface without an underlying graph"));
	}

	UPCGGraphInstance* GraphInstance = Component->GetGraphInstance();
	UPCGGraphInterface* PreviousGraph = GraphInstance ? GraphInstance->Graph.Get() : nullptr;
	const FString PreviousGraphPath = PreviousGraph ? PreviousGraph->GetPathName() : FString();
	const bool bWouldChange = PreviousGraph != RequestedGraph;
	if (bWouldChange && !GraphInstance->CanGraphInterfaceBeSet(RequestedGraph))
	{
		return InvalidParam(
			TEXT("graph_asset_path"),
			TEXT("graph_asset_path would create a recursive UPCGGraphInstance chain"));
	}

	if (bDryRun || !bWouldChange)
	{
		return FMonolithActionResult::Success(BuildBlueprintComponentGraphResult(
			Blueprint,
			Component,
			ComponentName,
			CanonicalBlueprintPath,
			PreviousGraphPath,
			ResolvedGraphPath,
			bDryRun,
			bWouldChange,
			false,
			false));
	}
	if (!bConfirm)
	{
		return InvalidParam(
			TEXT("confirm"),
			TEXT("A mutating set_blueprint_component_graph call requires confirm=true"));
	}

	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
		Blueprint, TEXT("pcg.set_blueprint_component_graph"), SourceControlPrepare, Error))
	{
		return Error;
	}
	UPackage* const OriginalPackage = Blueprint->GetOutermost();
	const bool bPackageWasDirty = OriginalPackage && OriginalPackage->IsDirty();

	auto Rollback = [&]() -> bool
	{
		UBlueprint* RollbackBlueprint = nullptr;
		UPCGComponent* RollbackComponent = nullptr;
		FString RollbackResolvedPath;
		FMonolithActionResult RollbackError;
		if (!ResolveBlueprintPCGComponentTemplate(
			CanonicalBlueprintPath,
			ComponentName,
			RollbackBlueprint,
			RollbackComponent,
			RollbackResolvedPath,
			RollbackError) ||
			!RollbackComponent || !RollbackComponent->GetGraphInstance() ||
			!RollbackComponent->GetGraphInstance()->CanGraphInterfaceBeSet(PreviousGraph))
		{
			return false;
		}
		RollbackBlueprint->Modify();
		RollbackComponent->Modify();
		RollbackComponent->GetGraphInstance()->Modify();
		RollbackComponent->SetGraphLocal(PreviousGraph);
		FBlueprintEditorUtils::MarkBlueprintAsModified(RollbackBlueprint);
		FKismetEditorUtilities::CompileBlueprint(RollbackBlueprint);

		UBlueprint* VerifiedBlueprint = nullptr;
		UPCGComponent* VerifiedComponent = nullptr;
		FString VerifiedPath;
		FMonolithActionResult VerifiedError;
		const bool bRollbackComplete = ResolveBlueprintPCGComponentTemplate(
			CanonicalBlueprintPath,
			ComponentName,
			VerifiedBlueprint,
			VerifiedComponent,
			VerifiedPath,
			VerifiedError) &&
			VerifiedComponent && VerifiedComponent->GetGraphInstance() &&
			VerifiedComponent->GetGraphInstance()->Graph.Get() == PreviousGraph &&
			VerifiedBlueprint && VerifiedBlueprint->Status != BS_Error;
		if (bRollbackComplete && VerifiedBlueprint && VerifiedBlueprint->GetOutermost())
		{
			VerifiedBlueprint->GetOutermost()->SetDirtyFlag(bPackageWasDirty);
		}
		else if (VerifiedBlueprint)
		{
			VerifiedBlueprint->MarkPackageDirty();
		}
		return bRollbackComplete;
	};

	Blueprint->Modify();
	Component->Modify();
	GraphInstance->Modify();
	Component->SetGraphLocal(RequestedGraph);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	if (!ResolveBlueprintPCGComponentTemplate(
		CanonicalBlueprintPath, ComponentName, Blueprint, Component, ResolvedBlueprintPath, Error) ||
		!Blueprint || Blueprint->Status == BS_Error || !Component || !Component->GetGraphInstance() ||
		Component->GetGraphInstance()->Graph.Get() != RequestedGraph)
	{
		const bool bRollbackComplete = Rollback();
		return AttachSourceControlPrepare(
			FMonolithActionResult::Error(FString::Printf(
				TEXT("Blueprint PCG graph assignment failed compile/read-back validation; rollback_complete=%s"),
				bRollbackComplete ? TEXT("true") : TEXT("false"))),
			SourceControlPrepare);
	}

	FString SavedFilename;
	if (bSave && !SaveBlueprintPackage(Blueprint, SavedFilename))
	{
		const bool bRollbackComplete = Rollback();
		return AttachSourceControlPrepare(
			FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to save Blueprint package after graph assignment; rollback_complete=%s"),
				bRollbackComplete ? TEXT("true") : TEXT("false"))),
			SourceControlPrepare);
	}

	FMonolithActionResult Result = FMonolithActionResult::Success(BuildBlueprintComponentGraphResult(
		Blueprint,
		Component,
		ComponentName,
		CanonicalBlueprintPath,
		PreviousGraphPath,
		ResolvedGraphPath,
		false,
		true,
		true,
		bSave,
		SavedFilename));
	return AttachSourceControlPrepare(MoveTemp(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::SetComponentSettings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	FString ComponentPath;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error, &ComponentPath);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.set_component_settings"), Error))
	{
		return Error;
	}

	const bool bHasSeed = Params->HasField(TEXT("seed"));
	const bool bHasActivated = Params->HasField(TEXT("activated"));
	const bool bHasPartitioned = Params->HasField(TEXT("partitioned"));
	const bool bHasTrigger = Params->HasField(TEXT("generation_trigger"));
	const bool bHasGenerateOnDrop = Params->HasField(TEXT("generate_on_drop_when_on_demand"));
	if (!bHasSeed && !bHasActivated && !bHasPartitioned && !bHasTrigger && !bHasGenerateOnDrop)
	{
		return InvalidParam(TEXT("params"), TEXT("At least one explicit component setting must be supplied"));
	}

	FComponentSettingsSnapshot Target(Component);
	bool bSave = true;
	FString TriggerString = GenerationTriggerToString(Target.GenerationTrigger);
	if (!ReadOptionalInt32(Params, TEXT("seed"), Target.Seed, Target.Seed, Error) ||
		!ReadOptionalBool(Params, TEXT("activated"), Target.bActivated, Target.bActivated, Error) ||
		!ReadOptionalBool(Params, TEXT("partitioned"), Target.bPartitioned, Target.bPartitioned, Error) ||
		!ReadOptionalString(Params, TEXT("generation_trigger"), TriggerString, TriggerString, Error) ||
		!ReadOptionalBool(Params, TEXT("generate_on_drop_when_on_demand"), Target.bGenerateOnDropWhenOnDemand,
			Target.bGenerateOnDropWhenOnDemand, Error) ||
		!ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return Error;
	}
	if (!ParseGenerationTrigger(TriggerString, Target.GenerationTrigger))
	{
		return InvalidParam(TEXT("generation_trigger"),
			TEXT("generation_trigger must be on_load, on_demand, or at_runtime"));
	}
	if (Target.bGenerateOnDropWhenOnDemand &&
		Target.GenerationTrigger != EPCGComponentGenerationTrigger::GenerateOnDemand)
	{
		return InvalidParam(TEXT("generate_on_drop_when_on_demand"),
			TEXT("generate_on_drop_when_on_demand=true requires generation_trigger=on_demand"));
	}
	if (Target.bPartitioned && !Component->CanPartition())
	{
		return InvalidParam(TEXT("partitioned"), TEXT("The target actor cannot host a partitioned PCG component"));
	}

	const FComponentSettingsSnapshot Original(Component);
	if (Target.Matches(Component))
	{
		if (bSave && !RequireIdleUngenerated(Component, TEXT("pcg.set_component_settings"), Error))
		{
			return Error;
		}
		if (!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
		{
			return Error;
		}
		TSharedPtr<FJsonObject> SourceControlPrepare;
		if (bSave && !PrepareSourceControlBeforeMutation(
				Component, TEXT("pcg.set_component_settings"), SourceControlPrepare, Error))
		{
			return Error;
		}
		return AttachSourceControlPrepare(
			SaveComponentMutation(Component, TEXT("set_component_settings"), false, bSave),
			SourceControlPrepare);
	}
	if (!RequireIdleUngenerated(Component, TEXT("pcg.set_component_settings"), Error) ||
		!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
	{
		return Error;
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.set_component_settings"), SourceControlPrepare, Error))
	{
		return Error;
	}

	AActor* Actor = Component->GetOwner();
	const FDirtySnapshot DirtySnapshot(Actor);
	Actor->Modify();
	Component->Modify();
	FString ApplyError;
	if (!ApplySettingsSnapshot(Component, ComponentPath, Target, ApplyError) || !Target.Matches(Component))
	{
		UPCGComponent* RollbackComponent = Component;
		if (!RollbackComponent)
		{
			FMonolithActionResult ResolveError;
			RollbackComponent = ResolveComponentExact(ComponentPath, ResolveError);
		}
		FString RollbackError;
		const bool bRollbackComplete = RollbackComponent &&
			ApplySettingsSnapshot(RollbackComponent, ComponentPath, Original, RollbackError) &&
			Original.Matches(RollbackComponent);
		FinalizeRollbackDirtyState(bRollbackComplete, Actor, RollbackComponent, DirtySnapshot);
		return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
			TEXT("PCG component settings failed read-back validation: %s; rollback_complete=%s%s%s"),
			*ApplyError, bRollbackComplete ? TEXT("true") : TEXT("false"),
			RollbackError.IsEmpty() ? TEXT("") : TEXT("; rollback_error="), *RollbackError))
			.WithErrorData(BuildComponentJson(RollbackComponent, false)), SourceControlPrepare);
	}

	MarkComponentMutationDirty(Component);
	return AttachSourceControlPrepare(
		SaveComponentMutation(Component, TEXT("set_component_settings"), true, bSave),
		SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::GenerateComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.generate_component"), Error))
	{
		return Error;
	}
	bool bForce = false;
	if (!ReadOptionalBool(Params, TEXT("force"), false, bForce, Error))
	{
		return Error;
	}
	if (!Component->IsRegistered())
	{
		return FMonolithActionResult::Error(TEXT("pcg.generate_component requires a registered component"));
	}
	if (!Component->bActivated)
	{
		return FMonolithActionResult::Error(TEXT("pcg.generate_component requires activated=true"));
	}
	if (!Component->GetGraph())
	{
		return FMonolithActionResult::Error(TEXT("pcg.generate_component requires an assigned graph"));
	}
	if (!Component->GetSubsystem())
	{
		return FMonolithActionResult::Error(TEXT("pcg.generate_component requires an available UPCGSubsystem"));
	}
	if (Component->IsManagedByRuntimeGenSystem())
	{
		return FMonolithActionResult::Error(
			TEXT("Runtime-scheduler-managed components must be generated by the UE PCG runtime scheduler, not this action"));
	}
	if (Component->IsCleaningUp())
	{
		return FMonolithActionResult::Error(TEXT("The component is cleaning up; poll pcg.get_component before generating"));
	}
#if WITH_EDITOR
	if (Component->IsRefreshInProgress())
	{
		return FMonolithActionResult::Error(TEXT("The component is refreshing; poll pcg.get_component before generating"));
	}
#endif
	if (Component->IsGenerating())
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("generate_component"), false);
		Result->SetBoolField(TEXT("scheduled"), false);
		Result->SetBoolField(TEXT("already_generating"), true);
		AddTaskFields(Result, TEXT("scheduled"), Component->GetGenerationTaskId());
		return FMonolithActionResult::Success(Result);
	}
	const bool bCanonicalUpToDate = Component->bGenerated && !bForce &&
		!Component->IsPartitioned() && !Component->AreProceduralInstancesInUse()
#if WITH_EDITORONLY_DATA
		&& !Component->bDirtyGenerated
#endif
		;
	if (bCanonicalUpToDate)
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("generate_component"), false);
		Result->SetBoolField(TEXT("scheduled"), false);
		Result->SetBoolField(TEXT("already_generated"), true);
		Result->SetBoolField(TEXT("up_to_date"), true);
		Result->SetBoolField(TEXT("engine_declined_generation"), false);
		Result->SetBoolField(TEXT("preflight_up_to_date"), true);
		AddTaskFields(Result, TEXT("scheduled"), InvalidPCGTaskId);
		return FMonolithActionResult::Success(Result);
	}

	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.generate_component"), SourceControlPrepare, Error))
	{
		return Error;
	}
	const FPCGTaskId TaskId = Component->GenerateLocalGetTaskId(bForce);
	if (TaskId == InvalidPCGTaskId)
	{
		// Canonical public no-op state was classified before source-control
		// preparation. Any remaining invalid task ID is a genuine scheduling failure.
		return AttachSourceControlPrepare(FMonolithActionResult::Error(
			TEXT("UE PCG rejected generation after all public preconditions passed; inspect editor/output logs for ShouldGenerate or subsystem errors"))
			.WithErrorData(BuildComponentJson(Component, false)), SourceControlPrepare);
	}
	TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("generate_component"), true);
	Result->SetBoolField(TEXT("scheduled"), true);
	Result->SetBoolField(TEXT("already_generating"), false);
	Result->SetBoolField(TEXT("already_generated"), false);
	Result->SetBoolField(TEXT("up_to_date"), false);
	Result->SetBoolField(TEXT("force"), bForce);
	AddTaskFields(Result, TEXT("scheduled"), TaskId);
	return AttachSourceControlPrepare(
		FMonolithActionResult::Success(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::RefreshComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.refresh_component"), Error))
	{
		return Error;
	}
	if (!Component->IsRegistered() || !Component->bActivated || !Component->GetGraph())
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.refresh_component requires a registered, active component with an assigned graph"));
	}
	if (Component->IsManagedByRuntimeGenSystem())
	{
		return FMonolithActionResult::Error(
			TEXT("Runtime-scheduler-managed components must be refreshed by the UE PCG runtime scheduler"));
	}
	if (Component->IsGenerating() || Component->IsCleaningUp())
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.refresh_component requires generation and cleanup to be idle"));
	}
	if (!HasGeneratedState(Component))
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.refresh_component requires previously generated state; call pcg.generate_component first"));
	}
#if WITH_EDITOR
	if (Component->GetOwner() && Component->GetOwner()->bIsEditorPreviewActor)
	{
		return FMonolithActionResult::Error(TEXT("UE PCG disables automatic refresh on editor preview actors"));
	}
#if WITH_EDITORONLY_DATA
	if (!Component->bRegenerateInEditor && Component->bActivated)
	{
		return FMonolithActionResult::Error(
			TEXT("The component has bRegenerateInEditor=false, so UE PCG would intentionally ignore this refresh"));
	}
#endif
	if (Component->IsRefreshInProgress())
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("refresh_component"), false);
		Result->SetBoolField(TEXT("scheduled"), false);
		Result->SetBoolField(TEXT("already_refreshing"), true);
		Result->SetBoolField(TEXT("coalesced"), true);
		return FMonolithActionResult::Success(Result);
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.refresh_component"), SourceControlPrepare, Error))
	{
		return Error;
	}
	Component->Refresh(EPCGChangeType::None, false);
	if (!Component->IsRefreshInProgress())
	{
		return AttachSourceControlPrepare(FMonolithActionResult::Error(
			TEXT("UE PCG did not schedule the requested refresh; inspect PCG refresh CVars and subsystem availability"))
			.WithErrorData(BuildComponentJson(Component, false)), SourceControlPrepare);
	}
	TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("refresh_component"), true);
	Result->SetBoolField(TEXT("scheduled"), true);
	Result->SetBoolField(TEXT("already_refreshing"), false);
	Result->SetBoolField(TEXT("coalesced"), false);
	return AttachSourceControlPrepare(
		FMonolithActionResult::Success(Result), SourceControlPrepare);
#else
	return FMonolithActionResult::Error(TEXT("pcg.refresh_component is only available in editor builds"));
#endif
}

FMonolithActionResult FMonolithPCGComponentActions::CancelComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.cancel_component"), Error))
	{
		return Error;
	}
	if (Component->IsManagedByRuntimeGenSystem())
	{
		return FMonolithActionResult::Error(
			TEXT("Runtime-scheduler-managed generation is owned by the UE PCG runtime scheduler"));
	}
	if (Component->IsCleaningUp())
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.cancel_component cancels generation only; cleanup is already in progress"));
	}
#if WITH_EDITOR
	if (Component->IsRefreshInProgress())
	{
		return FMonolithActionResult::Error(
			TEXT("A refresh is active and UE 5.8 exposes no reliable public refresh-task cancellation; poll pcg.get_component until idle"));
	}
#endif
	if (!Component->IsGenerating())
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("cancel_component"), false);
		Result->SetBoolField(TEXT("cancellation_requested"), false);
		Result->SetBoolField(TEXT("was_generating"), false);
		AddTaskFields(Result, TEXT("cancelled_generation"), InvalidPCGTaskId);
		return FMonolithActionResult::Success(Result);
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.cancel_component"), SourceControlPrepare, Error))
	{
		return Error;
	}
	const FPCGTaskId CancelledTaskId = Component->GetGenerationTaskId();
	Component->CancelGeneration();
	TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("cancel_component"), true);
	Result->SetBoolField(TEXT("cancellation_requested"), true);
	Result->SetBoolField(TEXT("was_generating"), true);
	AddTaskFields(Result, TEXT("cancelled_generation"), CancelledTaskId);
	return AttachSourceControlPrepare(
		FMonolithActionResult::Success(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::CleanupComponent(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.cleanup_component"), Error))
	{
		return Error;
	}
	bool bRemoveComponents = true;
	if (!ReadOptionalBool(Params, TEXT("remove_components"), true, bRemoveComponents, Error))
	{
		return Error;
	}
	if (Component->IsManagedByRuntimeGenSystem())
	{
		return FMonolithActionResult::Error(
			TEXT("Runtime-scheduler-managed components must be cleaned by the UE PCG runtime scheduler"));
	}
	if (Component->IsGenerating())
	{
		return FMonolithActionResult::Error(
			TEXT("Generation is active; call pcg.cancel_component and poll until idle before cleanup"));
	}
#if WITH_EDITOR
	if (Component->IsRefreshInProgress())
	{
		return FMonolithActionResult::Error(
			TEXT("Refresh is active and cannot be cancelled through the UE 5.8 public PCG API; poll pcg.get_component until idle before cleanup"));
	}
#endif
	if (Component->IsCleaningUp())
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("cleanup_component"), false);
		Result->SetBoolField(TEXT("scheduled"), false);
		Result->SetBoolField(TEXT("already_cleaning"), true);
		Result->SetBoolField(TEXT("requested_remove_components"), bRemoveComponents);
		Result->SetBoolField(TEXT("inflight_remove_components_known"), false);
		Result->SetStringField(TEXT("coalescing_status"), TEXT("already_cleaning_mode_not_observable"));
		AddTaskFields(Result, TEXT("scheduled"), Component->GetCleanupTaskId());
		return FMonolithActionResult::Success(Result);
	}
	UPCGSubsystem* Subsystem = Component->GetSubsystem();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.cleanup_component requires an available UPCGSubsystem"));
	}
	const bool bHasPartitionMappings =
		!Subsystem->GetPCGComponentPartitionActorMappings(Component).IsEmpty();
	if (!HasGeneratedState(Component) && !bHasPartitionMappings)
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("cleanup_component"), false);
		Result->SetBoolField(TEXT("scheduled"), false);
		Result->SetBoolField(TEXT("already_clean"), true);
		Result->SetBoolField(TEXT("remove_components"), bRemoveComponents);
		Result->SetBoolField(TEXT("engine_declined_cleanup"), false);
		Result->SetBoolField(TEXT("preflight_already_clean"), true);
		AddTaskFields(Result, TEXT("scheduled"), InvalidPCGTaskId);
		return FMonolithActionResult::Success(Result);
	}

	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.cleanup_component"), SourceControlPrepare, Error))
	{
		return Error;
	}
	const FPCGTaskId TaskId = Component->CleanupLocal(bRemoveComponents, TArray<FPCGTaskId>());
	if (TaskId == InvalidPCGTaskId)
	{
		return AttachSourceControlPrepare(FMonolithActionResult::Error(
			TEXT("UE PCG rejected cleanup after generated/resource state was detected; inspect subsystem and partition mappings"))
			.WithErrorData(BuildComponentJson(Component, false)), SourceControlPrepare);
	}
	TSharedPtr<FJsonObject> Result = BuildMutationResult(Component, TEXT("cleanup_component"), true);
	Result->SetBoolField(TEXT("scheduled"), true);
	Result->SetBoolField(TEXT("already_cleaning"), false);
	Result->SetBoolField(TEXT("already_clean"), false);
	Result->SetBoolField(TEXT("remove_components"), bRemoveComponents);
	AddTaskFields(Result, TEXT("scheduled"), TaskId);
	return AttachSourceControlPrepare(
		FMonolithActionResult::Success(Result), SourceControlPrepare);
}

FMonolithActionResult FMonolithPCGComponentActions::GetComponentOutput(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error);
	if (!Component)
	{
		return Error;
	}
	int32 OutputLimit = 100;
	int32 TagLimit = 100;
	int32 ResourceLimit = 100;
	int32 ManagedObjectLimit = 100;
	bool bIncludeManagedResources = true;
	if (!ReadOptionalBoundedInt(Params, TEXT("output_limit"), 100, 1, MaxOutputItems, OutputLimit, Error) ||
		!ReadOptionalBoundedInt(Params, TEXT("tag_limit"), 100, 1, MaxTagsPerOutput, TagLimit, Error) ||
		!ReadOptionalBool(Params, TEXT("include_managed_resources"), true, bIncludeManagedResources, Error) ||
		!ReadOptionalBoundedInt(Params, TEXT("resource_limit"), 100, 1, MaxManagedResources, ResourceLimit, Error) ||
		!ReadOptionalBoundedInt(Params, TEXT("managed_object_limit"), 100, 1,
			MaxManagedObjectsPerResource, ManagedObjectLimit, Error))
	{
		return Error;
	}
	if (Component->IsGenerating() || Component->IsCleaningUp()
#if WITH_EDITOR
		|| Component->IsRefreshInProgress()
#endif
		)
	{
		return FMonolithActionResult::Error(
			TEXT("Generated output is lock-free and may only be inspected while generation, cleanup, and refresh are idle"))
			.WithErrorData(BuildComponentJson(Component, false));
	}

	const FPCGDataCollection& Output = Component->GetGeneratedGraphOutput();
	const bool bCrcAbsent = Output.DataCrcs.IsEmpty();
	const bool bCrcAligned = !bCrcAbsent && Output.DataCrcs.Num() == Output.TaggedData.Num();
	TArray<TSharedPtr<FJsonValue>> OutputRows;
	OutputRows.Reserve(FMath::Min(OutputLimit, Output.TaggedData.Num()));
	for (int32 Index = 0; Index < Output.TaggedData.Num() && Index < OutputLimit; ++Index)
	{
		const FPCGCrc* CachedCrc = bCrcAligned ? &Output.DataCrcs[Index] : nullptr;
		OutputRows.Add(MakeShared<FJsonValueObject>(
			BuildOutputItemJson(Output.TaggedData[Index], Index, CachedCrc, TagLimit)));
	}

	TArray<TSharedPtr<FJsonValue>> ResourceRows;
	int32 TotalResources = 0;
	const bool bResourcesAccessible = Component->AreManagedResourcesAccessible();
	if (bIncludeManagedResources && bResourcesAccessible)
	{
		Component->ForEachConstManagedResource(
			[&ResourceRows, &TotalResources, ResourceLimit, ManagedObjectLimit](const UPCGManagedResource* Resource)
			{
				if (!Resource)
				{
					return;
				}
				const int32 ResourceIndex = TotalResources++;
				if (ResourceRows.Num() < ResourceLimit)
				{
					ResourceRows.Add(MakeShared<FJsonValueObject>(
						BuildManagedResourceJson(Resource, ResourceIndex, ManagedObjectLimit)));
				}
			});
	}

	TSharedPtr<FJsonObject> Result = BuildComponentJson(Component, false);
	Result->SetStringField(TEXT("operation"), TEXT("get_component_output"));
	Result->SetArrayField(TEXT("output_items"), OutputRows);
	Result->SetNumberField(TEXT("output_count"), Output.TaggedData.Num());
	Result->SetNumberField(TEXT("output_returned"), OutputRows.Num());
	Result->SetNumberField(TEXT("output_limit"), OutputLimit);
	Result->SetNumberField(TEXT("tag_limit"), TagLimit);
	Result->SetBoolField(TEXT("output_truncated"), OutputRows.Num() < Output.TaggedData.Num());
	Result->SetStringField(TEXT("crc_status"), bCrcAbsent ? TEXT("absent") : (bCrcAligned ? TEXT("aligned") : TEXT("mismatch")));
	Result->SetNumberField(TEXT("crc_count"), Output.DataCrcs.Num());
	Result->SetBoolField(TEXT("include_managed_resources"), bIncludeManagedResources);
	Result->SetBoolField(TEXT("managed_resources_accessible"), bResourcesAccessible);
	Result->SetArrayField(TEXT("managed_resources"), ResourceRows);
	Result->SetNumberField(TEXT("managed_resource_count"),
		bIncludeManagedResources && bResourcesAccessible ? TotalResources : INDEX_NONE);
	Result->SetStringField(TEXT("managed_resource_count_status"), !bIncludeManagedResources
		? TEXT("not_requested") : (bResourcesAccessible ? TEXT("exact") : TEXT("unavailable")));
	Result->SetNumberField(TEXT("managed_resource_returned"), ResourceRows.Num());
	Result->SetNumberField(TEXT("resource_limit"), ResourceLimit);
	Result->SetNumberField(TEXT("managed_object_limit"), ManagedObjectLimit);
	Result->SetBoolField(TEXT("managed_resources_truncated"),
		bIncludeManagedResources && bResourcesAccessible && ResourceRows.Num() < TotalResources);
	FString OutputStatus;
	if (!Output.TaggedData.IsEmpty() || (bIncludeManagedResources && TotalResources > 0))
	{
		OutputStatus = TEXT("available");
	}
	else if (!bIncludeManagedResources)
	{
		OutputStatus = Component->bGenerated
			? TEXT("output_empty_resources_not_requested")
			: TEXT("not_generated_resources_not_requested");
	}
	else
	{
		OutputStatus = Component->bGenerated ? TEXT("empty") : TEXT("not_generated");
	}
	Result->SetStringField(TEXT("output_status"), OutputStatus);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGComponentActions::SetComponentUserParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithPCGComponent;
	FMonolithActionResult Error;
	FString ComponentPath;
	UPCGComponent* Component = ReadAndResolveComponent(Params, Error, &ComponentPath);
	if (!Component)
	{
		return Error;
	}
	if (!RequireOriginalComponentMutation(Component, TEXT("pcg.set_component_user_parameters"), Error))
	{
		return Error;
	}
	bool bDryRun = false;
	bool bSave = true;
	if (!ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, Error) ||
		!ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return Error;
	}
	if (!RequireIdleUngenerated(Component, TEXT("pcg.set_component_user_parameters"), Error))
	{
		return Error;
	}

	UPCGGraphInstance* Instance = Component->GetGraphInstance();
	if (!Instance || !Instance->Graph || !Instance->GetGraph())
	{
		return FMonolithActionResult::Error(
			TEXT("pcg.set_component_user_parameters requires an assigned graph and valid graph instance"));
	}
	const FInstancedPropertyBag* LiveValuesConst = Instance->GetUserParametersStruct();
	if (!LiveValuesConst || !LiveValuesConst->IsValid() || !LiveValuesConst->GetPropertyBagStruct())
	{
		return FMonolithActionResult::Error(
			TEXT("The assigned graph exposes no valid user-parameter property bag"));
	}

	const TSharedPtr<FJsonObject>* ValuesObjectPtr = nullptr;
	TSharedPtr<FJsonObject> ValuesObject;
	if (Params->HasField(TEXT("values")))
	{
		if (!Params->TryGetObjectField(TEXT("values"), ValuesObjectPtr) || !ValuesObjectPtr || !ValuesObjectPtr->IsValid())
		{
			return InvalidParam(TEXT("values"), TEXT("values must be a JSON object"));
		}
		ValuesObject = *ValuesObjectPtr;
	}
	const TArray<TSharedPtr<FJsonValue>>* ResetArray = nullptr;
	if (Params->HasField(TEXT("reset")) &&
		(!Params->TryGetArrayField(TEXT("reset"), ResetArray) || !ResetArray))
	{
		return InvalidParam(TEXT("reset"), TEXT("reset must be an array of parameter-name strings"));
	}
	const int32 ValueCount = ValuesObject.IsValid() ? ValuesObject->Values.Num() : 0;
	const int32 ResetCount = ResetArray ? ResetArray->Num() : 0;
	if (ValueCount + ResetCount == 0)
	{
		return InvalidParam(TEXT("params"), TEXT("At least one values entry or reset name is required"));
	}
	if (ValueCount + ResetCount > MaxUserParameters)
	{
		return InvalidParam(TEXT("params"), FString::Printf(
			TEXT("At most %d user-parameter operations are allowed per call"), MaxUserParameters));
	}

	TArray<FName> ValueNames;
	TMap<FName, TSharedPtr<FJsonValue>> JsonValuesByName;
	TSet<FName> SeenNames;
	if (ValuesObject.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ValuesObject->Values)
		{
			FString TrimmedName = Pair.Key;
			TrimmedName.TrimStartAndEndInline();
			const FName Name(*TrimmedName);
			if (TrimmedName.IsEmpty() || Name.IsNone())
			{
				return InvalidParam(TEXT("values"), TEXT("values contains an empty parameter name"));
			}
			if (SeenNames.Contains(Name))
			{
				return InvalidParam(TEXT("values"), FString::Printf(
					TEXT("values contains duplicate case-insensitive parameter name '%s'"), *TrimmedName));
			}
			SeenNames.Add(Name);
			ValueNames.Add(Name);
			JsonValuesByName.Add(Name, Pair.Value);
		}
	}
	ValueNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });

	TArray<FName> ResetNames;
	if (ResetArray)
	{
		for (const TSharedPtr<FJsonValue>& ResetValue : *ResetArray)
		{
			FString ResetNameString;
			if (!ResetValue.IsValid() || ResetValue->Type != EJson::String ||
				!ResetValue->TryGetString(ResetNameString))
			{
				return InvalidParam(TEXT("reset"), TEXT("Every reset entry must be a string"));
			}
			ResetNameString.TrimStartAndEndInline();
			const FName ResetName(*ResetNameString);
			if (ResetNameString.IsEmpty() || ResetName.IsNone())
			{
				return InvalidParam(TEXT("reset"), TEXT("reset contains an empty parameter name"));
			}
			if (SeenNames.Contains(ResetName))
			{
				return InvalidParam(TEXT("reset"), FString::Printf(
					TEXT("Parameter '%s' is duplicated or appears in both values and reset"), *ResetNameString));
			}
			SeenNames.Add(ResetName);
			ResetNames.Add(ResetName);
		}
	}
	ResetNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });

	FInstancedPropertyBag StagedValues = *LiveValuesConst;
	TMap<FName, FString> BeforeSerialized;
	TMap<FName, FString> AfterSerialized;
	TMap<FName, bool> BeforeOverridden;
	TMap<FName, const FPropertyBagPropertyDesc*> DescsByName;
	auto ValidateDescriptor = [&](const FName Name, const TCHAR* Field) -> bool
	{
		const FPropertyBagPropertyDesc* Desc = LiveValuesConst->FindPropertyDescByName(Name);
		if (!Desc || !Desc->CachedProperty)
		{
			Error = InvalidParam(Field, FString::Printf(
				TEXT("Unknown user parameter '%s' on the assigned graph instance"), *Name.ToString()));
			return false;
		}
		FString UnsupportedReason;
		if (!IsSupportedUserParameter(*Desc, UnsupportedReason))
		{
			Error = InvalidParam(Field, FString::Printf(TEXT("User parameter '%s' is not writable: %s"),
				*Desc->Name.ToString(), *UnsupportedReason));
			return false;
		}
#if WITH_EDITOR
		if (!Instance->CanEditChange(Desc->CachedProperty))
		{
			Error = FMonolithActionResult::Error(FString::Printf(
				TEXT("User parameter '%s' is not editable in the current Level Instance context"),
				*Desc->Name.ToString()));
			return false;
		}
#endif
		if (Desc->ValueType == EPropertyBagPropertyType::String && LiveValuesConst->GetValue().IsValid())
		{
			const void* ValuePtr = Desc->CachedProperty->ContainerPtrToValuePtr<void>(
				LiveValuesConst->GetValue().GetMemory());
			if (static_cast<const FString*>(ValuePtr)->Len() > MaxUserParameterValueChars)
			{
				Error = FMonolithActionResult::Error(FString::Printf(
					TEXT("Current string value for '%s' exceeds the %d-character mutation bound"),
					*Desc->Name.ToString(), MaxUserParameterValueChars));
				return false;
			}
		}
		const TValueOrError<FString, EPropertyBagResult> Before =
			LiveValuesConst->GetValueSerializedString(Desc->Name);
		if (!Before.IsValid())
		{
			Error = FMonolithActionResult::Error(FString::Printf(
				TEXT("Could not serialize current value of user parameter '%s'"), *Desc->Name.ToString()));
			return false;
		}
		DescsByName.Add(Name, Desc);
		BeforeSerialized.Add(Name, Before.GetValue());
		BeforeOverridden.Add(Name, Instance->IsPropertyOverridden(Desc->CachedProperty));
		return true;
	};

	for (const FName Name : ValueNames)
	{
		if (!ValidateDescriptor(Name, TEXT("values")))
		{
			return Error;
		}
		const FPropertyBagPropertyDesc* Desc = DescsByName.FindChecked(Name);
		FString SerializedInput;
		FString ConversionError;
		if (!JsonValueToSerializedInput(*Desc, JsonValuesByName.FindChecked(Name), SerializedInput, ConversionError))
		{
			return InvalidParam(TEXT("values"), FString::Printf(TEXT("Invalid value for '%s': %s"),
				*Desc->Name.ToString(), *ConversionError));
		}
		const EPropertyBagResult SetResult = StagedValues.SetValueSerializedString(Desc->Name, SerializedInput);
		if (SetResult != EPropertyBagResult::Success)
		{
			return InvalidParam(TEXT("values"), FString::Printf(
				TEXT("UE property-bag parsing rejected '%s' for parameter '%s' (result=%d)"),
				*SerializedInput, *Desc->Name.ToString(), static_cast<int32>(SetResult)));
		}
		const TValueOrError<FString, EPropertyBagResult> Canonical =
			StagedValues.GetValueSerializedString(Desc->Name);
		if (!Canonical.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Could not read back staged value for user parameter '%s'"), *Desc->Name.ToString()));
		}
		if (Desc->ValueType == EPropertyBagPropertyType::Float ||
			Desc->ValueType == EPropertyBagPropertyType::Double)
		{
			const FPropertyBagPropertyDesc* StagedDesc = StagedValues.FindPropertyDescByID(Desc->ID);
			const FNumericProperty* NumericProperty = StagedDesc
				? CastField<FNumericProperty>(StagedDesc->CachedProperty) : nullptr;
			const FConstStructView StagedView = StagedValues.GetValue();
			if (!StagedDesc || !NumericProperty || !StagedView.IsValid())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Could not inspect staged floating-point value for '%s'"), *Desc->Name.ToString()));
			}
			const void* StagedValuePtr = StagedDesc->CachedProperty->ContainerPtrToValuePtr<void>(
				StagedView.GetMemory());
			if (!FMath::IsFinite(NumericProperty->GetFloatingPointPropertyValue(StagedValuePtr)))
			{
				return InvalidParam(TEXT("values"), FString::Printf(
					TEXT("Staged floating-point value for '%s' is not finite"), *Desc->Name.ToString()));
			}
		}
		AfterSerialized.Add(Name, Canonical.GetValue());
	}
	for (const FName Name : ResetNames)
	{
		if (!ValidateDescriptor(Name, TEXT("reset")))
		{
			return Error;
		}
		const FPropertyBagPropertyDesc* Desc = DescsByName.FindChecked(Name);
		const FInstancedPropertyBag* ParentValues = Instance->Graph->GetUserParametersStruct();
		const FPropertyBagPropertyDesc* ParentDesc = ParentValues ? ParentValues->FindPropertyDescByID(Desc->ID) : nullptr;
		if (ParentValues && ParentDesc && ParentDesc->ValueType == EPropertyBagPropertyType::String &&
			ParentDesc->CachedProperty && ParentValues->GetValue().IsValid())
		{
			const void* ParentValuePtr = ParentDesc->CachedProperty->ContainerPtrToValuePtr<void>(
				ParentValues->GetValue().GetMemory());
			if (static_cast<const FString*>(ParentValuePtr)->Len() > MaxUserParameterValueChars)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Parent default for '%s' exceeds the %d-character mutation bound"),
					*Desc->Name.ToString(), MaxUserParameterValueChars));
			}
		}
		const TValueOrError<FString, EPropertyBagResult> ParentSerialized =
			ParentValues && ParentDesc ? ParentValues->GetValueSerializedString(ParentDesc->Name)
				: MakeError(EPropertyBagResult::PropertyNotFound);
		if (!ParentSerialized.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Could not resolve parent default for reset parameter '%s'"), *Desc->Name.ToString()));
		}
		AfterSerialized.Add(Name, ParentSerialized.GetValue());
	}

	bool bChanged = false;
	for (const FName Name : ValueNames)
	{
		bChanged |= !BeforeOverridden.FindChecked(Name) ||
			BeforeSerialized.FindChecked(Name) != AfterSerialized.FindChecked(Name);
	}
	for (const FName Name : ResetNames)
	{
		bChanged |= BeforeOverridden.FindChecked(Name);
	}

	auto BuildChanges = [&](bool bApplied) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(ValueNames.Num() + ResetNames.Num());
		auto AddRow = [&](const FName Name, const FString& ChangeKind, bool bAfterOverridden)
		{
			const FPropertyBagPropertyDesc* Desc = DescsByName.FindChecked(Name);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Desc->Name.ToString());
			Row->SetStringField(TEXT("change"), ChangeKind);
			Row->SetStringField(TEXT("value_type"), UserParameterTypeToString(Desc->ValueType));
			SetBoundedStringField(Row, TEXT("before_serialized_value"), BeforeSerialized.FindChecked(Name));
			SetBoundedStringField(Row, TEXT("after_serialized_value"), AfterSerialized.FindChecked(Name));
			Row->SetBoolField(TEXT("before_overridden"), BeforeOverridden.FindChecked(Name));
			Row->SetBoolField(TEXT("after_overridden"), bAfterOverridden);
			Row->SetBoolField(TEXT("applied"), bApplied);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		};
		for (const FName Name : ValueNames)
		{
			AddRow(Name, TEXT("set"), true);
		}
		for (const FName Name : ResetNames)
		{
			AddRow(Name, TEXT("reset"), false);
		}
		return Rows;
	};

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Result = BuildMutationResult(
			Component, TEXT("set_component_user_parameters"), false);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("validated"), true);
		Result->SetNumberField(TEXT("operation_count"), ValueNames.Num() + ResetNames.Num());
		Result->SetArrayField(TEXT("changes"), BuildChanges(false));
		return FMonolithActionResult::Success(Result);
	}
	if (!bChanged)
	{
		if (!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
		{
			return Error;
		}
		TSharedPtr<FJsonObject> SourceControlPrepare;
		if (bSave && !PrepareSourceControlBeforeMutation(
				Component, TEXT("pcg.set_component_user_parameters"), SourceControlPrepare, Error))
		{
			return Error;
		}
		FMonolithActionResult NoChangeResult = SaveComponentMutation(
			Component, TEXT("set_component_user_parameters"), false, bSave);
		if (NoChangeResult.bSuccess && NoChangeResult.Result.IsValid())
		{
			NoChangeResult.Result->SetBoolField(TEXT("dry_run"), false);
			NoChangeResult.Result->SetBoolField(TEXT("validated"), true);
			NoChangeResult.Result->SetNumberField(TEXT("operation_count"), ValueNames.Num() + ResetNames.Num());
			NoChangeResult.Result->SetArrayField(TEXT("changes"), BuildChanges(false));
		}
		return AttachSourceControlPrepare(MoveTemp(NoChangeResult), SourceControlPrepare);
	}
	if (!PreflightSaveIfRequested(Component->GetOwner(), bSave, Error))
	{
		return Error;
	}
	TSharedPtr<FJsonObject> SourceControlPrepare;
	if (!PrepareSourceControlBeforeMutation(
			Component, TEXT("pcg.set_component_user_parameters"), SourceControlPrepare, Error))
	{
		return Error;
	}

	AActor* Actor = Component->GetOwner();
	const FDirtySnapshot DirtySnapshot(Actor);
	const FPCGOverrideInstancedPropertyBag OriginalOverrides = Instance->ParametersOverrides;
	Actor->Modify();
	Component->Modify();
	Instance->Modify();
	FInstancedPropertyBag& LiveValues = Instance->ParametersOverrides.Parameters;
	const FStructView LiveView = LiveValues.GetMutableValue();
	const FConstStructView StagedView = StagedValues.GetValue();
	FString CommitError;
	if (!LiveView.IsValid() || !StagedView.IsValid())
	{
		CommitError = TEXT("The live or staged property-bag value view is invalid");
	}
	for (const FName Name : ValueNames)
	{
		if (!CommitError.IsEmpty())
		{
			break;
		}
		const FPropertyBagPropertyDesc* LiveDesc = LiveValues.FindPropertyDescByName(Name);
		const FPropertyBagPropertyDesc* StagedDesc = StagedValues.FindPropertyDescByID(LiveDesc ? LiveDesc->ID : FGuid());
		if (!LiveDesc || !StagedDesc || !LiveDesc->CachedProperty || !StagedDesc->CachedProperty)
		{
			CommitError = FString::Printf(TEXT("Parameter layout changed while committing '%s'"), *Name.ToString());
			break;
		}
		Instance->UpdatePropertyOverride(LiveDesc->CachedProperty, true);
		void* Destination = LiveDesc->CachedProperty->ContainerPtrToValuePtr<void>(LiveView.GetMemory());
		const void* Source = StagedDesc->CachedProperty->ContainerPtrToValuePtr<void>(StagedView.GetMemory());
		LiveDesc->CachedProperty->CopyCompleteValue(Destination, Source);
		Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::ValueModifiedLocally, LiveDesc->Name);
	}
	for (const FName Name : ResetNames)
	{
		if (!CommitError.IsEmpty())
		{
			break;
		}
		const FPropertyBagPropertyDesc* LiveDesc = LiveValues.FindPropertyDescByName(Name);
		if (!LiveDesc || !LiveDesc->CachedProperty)
		{
			CommitError = FString::Printf(TEXT("Parameter layout changed while resetting '%s'"), *Name.ToString());
			break;
		}
		const bool bResetChangesValue = BeforeSerialized.FindChecked(Name) != AfterSerialized.FindChecked(Name);
		Instance->UpdatePropertyOverride(LiveDesc->CachedProperty, false);
		// UpdatePropertyOverride already notifies when it copies a different parent
		// value. Emit the explicit notification only for an override-flag-only reset.
		if (!bResetChangesValue)
		{
			Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::ValueModifiedLocally, LiveDesc->Name);
		}
	}

	FMonolithActionResult ResolveError;
	UPCGComponent* VerifiedComponent = ResolveComponentExact(ComponentPath, ResolveError);
	UPCGGraphInstance* VerifiedInstance = VerifiedComponent ? VerifiedComponent->GetGraphInstance() : nullptr;
	const FInstancedPropertyBag* VerifiedValues = VerifiedInstance ? VerifiedInstance->GetUserParametersStruct() : nullptr;
	if (CommitError.IsEmpty() && (!VerifiedInstance || !VerifiedValues))
	{
		CommitError = TEXT("The component or graph instance could not be re-resolved after parameter notifications");
	}
	for (const FName Name : ValueNames)
	{
		if (!CommitError.IsEmpty())
		{
			break;
		}
		const FPropertyBagPropertyDesc* Desc = VerifiedValues->FindPropertyDescByName(Name);
		const TValueOrError<FString, EPropertyBagResult> Readback =
			Desc ? VerifiedValues->GetValueSerializedString(Desc->Name)
				: MakeError(EPropertyBagResult::PropertyNotFound);
		if (!Desc || !Desc->CachedProperty || !Readback.IsValid() ||
			Readback.GetValue() != AfterSerialized.FindChecked(Name) ||
			!VerifiedInstance->IsPropertyOverridden(Desc->CachedProperty))
		{
			CommitError = FString::Printf(TEXT("Read-back validation failed for set parameter '%s'"), *Name.ToString());
		}
	}
	for (const FName Name : ResetNames)
	{
		if (!CommitError.IsEmpty())
		{
			break;
		}
		const FPropertyBagPropertyDesc* Desc = VerifiedValues->FindPropertyDescByName(Name);
		const TValueOrError<FString, EPropertyBagResult> Readback =
			Desc ? VerifiedValues->GetValueSerializedString(Desc->Name)
				: MakeError(EPropertyBagResult::PropertyNotFound);
		if (!Desc || !Desc->CachedProperty || !Readback.IsValid() ||
			Readback.GetValue() != AfterSerialized.FindChecked(Name) ||
			VerifiedInstance->IsPropertyOverridden(Desc->CachedProperty))
		{
			CommitError = FString::Printf(TEXT("Read-back validation failed for reset parameter '%s'"), *Name.ToString());
		}
	}

	if (!CommitError.IsEmpty())
	{
		UPCGGraphInstance* RollbackInstance = VerifiedInstance ? VerifiedInstance : Instance;
		UPCGComponent* RollbackComponent = VerifiedComponent;
		bool bRollbackComplete = false;
		if (RollbackInstance && IsValid(RollbackInstance))
		{
			RollbackInstance->ParametersOverrides = OriginalOverrides;
			for (const FName Name : ValueNames)
			{
				RollbackInstance->OnGraphParametersChanged(EPCGGraphParameterEvent::UndoRedo, Name);
			}
			for (const FName Name : ResetNames)
			{
				RollbackInstance->OnGraphParametersChanged(EPCGGraphParameterEvent::UndoRedo, Name);
			}

			FMonolithActionResult RollbackResolveError;
			RollbackComponent = ResolveComponentExact(ComponentPath, RollbackResolveError);
			RollbackInstance = RollbackComponent ? RollbackComponent->GetGraphInstance() : nullptr;
		}
		if (RollbackInstance && IsValid(RollbackInstance))
		{
			const TSet<FGuid>& RestoredOverrideIds =
				RollbackInstance->ParametersOverrides.PropertiesIDsOverridden;
			const TSet<FGuid>& OriginalOverrideIds = OriginalOverrides.PropertiesIDsOverridden;
			bRollbackComplete = RestoredOverrideIds.Num() == OriginalOverrideIds.Num();
			for (const FGuid& OriginalOverrideId : OriginalOverrideIds)
			{
				bRollbackComplete = bRollbackComplete && RestoredOverrideIds.Contains(OriginalOverrideId);
			}

			const FInstancedPropertyBag* RestoredValues = RollbackInstance->GetUserParametersStruct();
			auto VerifyRestoredParameter = [&](const FName Name)
			{
				const FPropertyBagPropertyDesc* RestoredDesc =
					RestoredValues ? RestoredValues->FindPropertyDescByName(Name) : nullptr;
				const TValueOrError<FString, EPropertyBagResult> RestoredSerialized = RestoredDesc
					? RestoredValues->GetValueSerializedString(RestoredDesc->Name)
					: MakeError(EPropertyBagResult::PropertyNotFound);
				bRollbackComplete = bRollbackComplete && RestoredDesc && RestoredDesc->CachedProperty &&
					RestoredSerialized.IsValid() &&
					RestoredSerialized.GetValue() == BeforeSerialized.FindChecked(Name) &&
					RollbackInstance->IsPropertyOverridden(RestoredDesc->CachedProperty) ==
						BeforeOverridden.FindChecked(Name);
			};
			for (const FName Name : ValueNames)
			{
				VerifyRestoredParameter(Name);
			}
			for (const FName Name : ResetNames)
			{
				VerifyRestoredParameter(Name);
			}
		}
		FinalizeRollbackDirtyState(bRollbackComplete, Actor, RollbackComponent, DirtySnapshot);
		return AttachSourceControlPrepare(FMonolithActionResult::Error(FString::Printf(
			TEXT("Atomic user-parameter commit failed: %s; rollback_complete=%s"),
			*CommitError, bRollbackComplete ? TEXT("true") : TEXT("false")))
			.WithErrorData(BuildComponentJson(RollbackComponent, false)), SourceControlPrepare);
	}

	Component = VerifiedComponent;
	MarkComponentMutationDirty(Component);
	FMonolithActionResult Result = SaveComponentMutation(
		Component, TEXT("set_component_user_parameters"), true, bSave);
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetBoolField(TEXT("dry_run"), false);
		Result.Result->SetBoolField(TEXT("validated"), true);
		Result.Result->SetNumberField(TEXT("operation_count"), ValueNames.Num() + ResetNames.Num());
		Result.Result->SetArrayField(TEXT("changes"), BuildChanges(true));
	}
	return AttachSourceControlPrepare(MoveTemp(Result), SourceControlPrepare);
}
