#include "MonolithMeshSceneActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Selection.h"
#include "UnrealEdGlobals.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "CollisionQueryParams.h"

// ============================================================================
// Static member
// ============================================================================

bool FMonolithMeshSceneActions::bBatchTransactionActive = false;

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MonolithScene::Private
{
namespace
{
	FString GDeleteActorsFaultActorPath;
	EDeleteActorsTestFault GDeleteActorsFault = EDeleteActorsTestFault::None;
}

void ConfigureDeleteActorsTestFault(
	const FString& ExactActorPath,
	EDeleteActorsTestFault Fault)
{
	GDeleteActorsFaultActorPath = ExactActorPath;
	GDeleteActorsFault = Fault;
}

void ResetDeleteActorsTestFault()
{
	GDeleteActorsFaultActorPath.Reset();
	GDeleteActorsFault = EDeleteActorsTestFault::None;
}

bool ConsumeDeleteActorsTestFault(
	const FString& ExactActorPath,
	EDeleteActorsTestFault Fault)
{
	if (GDeleteActorsFault != Fault ||
		!GDeleteActorsFaultActorPath.Equals(ExactActorPath, ESearchCase::CaseSensitive))
	{
		return false;
	}
	GDeleteActorsFault = EDeleteActorsTestFault::None;
	return true;
}
}
#endif

// ============================================================================
// Helpers
// ============================================================================

namespace SceneActionHelpers
{
	static constexpr int32 MaxDeleteActorsPerRequest = 1000;

	struct FDeleteActorTarget
	{
		FString RequestedIdentity;
		FString ActorPath;
		FString ActorName;
		FString ActorLabel;
		FObjectKey ObjectKey;
	};

	AActor* ResolveDeleteActor(UWorld* World, const FString& Identity, FString& OutError)
	{
		if (!World || Identity.IsEmpty())
		{
			OutError = Identity.IsEmpty()
				? TEXT("Actor identity must be a non-empty string")
				: TEXT("No editor world available");
			return nullptr;
		}

		const bool bLooksLikeObjectPath = Identity.StartsWith(TEXT("/")) || Identity.Contains(TEXT(":"));
		if (bLooksLikeObjectPath)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* const Actor = *It;
				if (Actor && Actor->GetPathName().Equals(Identity, ESearchCase::CaseSensitive))
				{
					return Actor;
				}
			}
			OutError = FString::Printf(
				TEXT("Exact actor object path is not live in the current editor world: %s"),
				*Identity);
			return nullptr;
		}

		// Destructive short-name resolution must be unique across both label and
		// internal-name domains. The shared convenience resolver preserves legacy
		// label precedence and returns the first internal-name match, which is not
		// an exact enough contract when streamed levels contain duplicate FNames.
		TArray<AActor*> Matches;
		TSet<FObjectKey> MatchKeys;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* const Actor = *It;
			if (!Actor || !IsValid(Actor) || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (Actor->GetActorLabel() == Identity || Actor->GetFName().ToString() == Identity)
			{
				const FObjectKey ActorKey(Actor);
				if (!MatchKeys.Contains(ActorKey))
				{
					MatchKeys.Add(ActorKey);
					Matches.Add(Actor);
				}
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(
				TEXT("Actor name/label '%s' is ambiguous across %d live actors; use an exact object path:"),
				*Identity,
				Matches.Num());
			for (const AActor* const Match : Matches)
			{
				OutError += FString::Printf(TEXT("\n  - %s"), *Match->GetPathName());
			}
			return nullptr;
		}

		OutError = FString::Printf(TEXT("Actor not found in the current editor world: %s"), *Identity);
		return nullptr;
	}

	FMonolithActionResult DeleteActorsPreflightError(
		const FString& Message,
		const FString& MapPackage,
		int32 RequestedCount,
		int32 FailedIndex,
		const FString& RequestedIdentity,
		const FString& ActorPath = FString())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("namespace"), TEXT("scene"));
		Data->SetStringField(TEXT("action"), TEXT("delete_actors"));
		Data->SetStringField(TEXT("status"), TEXT("rejected_preflight"));
		Data->SetStringField(TEXT("phase"), TEXT("preflight"));
		Data->SetStringField(TEXT("map_package"), MapPackage);
		Data->SetNumberField(TEXT("requested_count"), RequestedCount);
		Data->SetNumberField(TEXT("failed_index"), FailedIndex);
		Data->SetStringField(TEXT("requested_identity"), RequestedIdentity);
		Data->SetNumberField(TEXT("deleted_count"), 0);
		Data->SetNumberField(TEXT("unmodified_request_count"), RequestedCount);
		Data->SetBoolField(TEXT("mutation_started"), false);
		Data->SetBoolField(TEXT("partial_failure"), false);
		Data->SetBoolField(TEXT("rollback_performed"), false);
		if (!ActorPath.IsEmpty())
		{
			Data->SetStringField(TEXT("actor_path"), ActorPath);
		}
		return FMonolithActionResult::Error(Message).WithErrorData(Data);
	}

	/** Make a JSON array from a FVector */
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Scoped undo transaction that respects bBatchTransactionActive */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction = false;

		FScopedMeshTransaction(const FText& Description)
		{
			if (!FMonolithMeshSceneActions::bBatchTransactionActive && GEditor)
			{
				bOwnsTransaction = GEditor->BeginTransaction(Description) != INDEX_NONE;
			}
		}

		~FScopedMeshTransaction()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->EndTransaction();
			}
		}

		/**
		 * Close a failed action without claiming rollback. UE's CancelTransaction
		 * discards the transaction record; it does not undo mutations that already
		 * happened. Ending instead keeps any partial mutation available to Undo.
		 */
		void EndForFailure()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->EndTransaction();
			}
			bOwnsTransaction = false; // prevent a second EndTransaction in destructor
		}

		bool HasUndoTransaction() const
		{
			return bOwnsTransaction;
		}
	};

	/** Convert a mobility enum to string */
	FString MobilityToString(EComponentMobility::Type Mobility)
	{
		switch (Mobility)
		{
		case EComponentMobility::Static:     return TEXT("Static");
		case EComponentMobility::Stationary: return TEXT("Stationary");
		case EComponentMobility::Movable:    return TEXT("Movable");
		default:                             return TEXT("Unknown");
		}
	}

	/** Parse mobility string to enum, returns false if invalid */
	bool ParseMobility(const FString& Str, EComponentMobility::Type& Out)
	{
		if (Str.Equals(TEXT("Static"), ESearchCase::IgnoreCase))         { Out = EComponentMobility::Static; return true; }
		if (Str.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))     { Out = EComponentMobility::Stationary; return true; }
		if (Str.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))        { Out = EComponentMobility::Movable; return true; }
		return false;
	}

}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshSceneActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("scene"), TEXT("get_actor_info"),
		TEXT("Get comprehensive info for an actor in the editor world (class, transform, mesh, materials, tags, components, bounds)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::GetActorInfo),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("spawn_actor"),
		TEXT("Spawn an actor in the editor world. Path starting with '/' spawns StaticMeshActor with that mesh; otherwise spawns by class name."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SpawnActor),
		FParamSchemaBuilder()
			.Required(TEXT("class_or_mesh"), TEXT("string"), TEXT("Asset path for mesh (starts with '/') or class name"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"), TEXT("[1,1,1]"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Optional label for the spawned actor"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Actor folder path in the outliner"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("move_actor"),
		TEXT("Persistently move/rotate/scale a non-transient editor actor. Set relative=true to offset from current transform."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::MoveActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("Location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"))
			.Optional(TEXT("relative"), TEXT("boolean"), TEXT("If true, add to current transform instead of replacing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("duplicate_actor"),
		TEXT("Duplicate an actor in the editor world with an optional position offset"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::DuplicateActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Label for the duplicate"))
			.Optional(TEXT("offset"), TEXT("array"), TEXT("Position offset [x, y, z]"), TEXT("[0,0,0]"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("delete_actors"),
		TEXT("Delete exact live actors from the current map package. Preflights every target and verifies every requested actor identity is absent after one undoable commit. Does NOT delete asset files."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::DeleteActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of exact object paths or unique actor names/labels to delete (1-1000, no duplicates)"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("group_actors"),
		TEXT("Move actors into an outliner folder (creates folder hierarchy automatically)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::GroupActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels"))
			.Required(TEXT("group_name"), TEXT("string"), TEXT("Folder path (e.g. 'Furniture/Kitchen')"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("set_actor_properties"),
		TEXT("Set properties on an actor (mobility, physics, collision, shadows, tags, mass)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SetActorProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("mobility"), TEXT("string"), TEXT("Static, Stationary, or Movable"))
			.Optional(TEXT("simulate_physics"), TEXT("boolean"), TEXT("Enable/disable physics simulation"))
			.Optional(TEXT("collision_preset"), TEXT("string"), TEXT("Collision profile name"))
			.Optional(TEXT("cast_shadow"), TEXT("boolean"), TEXT("Enable/disable shadow casting"))
			.Optional(TEXT("tags"), TEXT("array"), TEXT("Array of tag strings to set on the actor"))
			.Optional(TEXT("mass_kg"), TEXT("number"), TEXT("Mass override in kg"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("batch_execute"),
		TEXT("Execute up to 200 scene actions in one undo transaction. Stops at the first failure, ends the transaction for Undo, and never claims automatic rollback. No nested batch_execute allowed."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::BatchExecute),
		FParamSchemaBuilder()
			.Required(TEXT("actions"), TEXT("array"), TEXT("Array of {action, params, namespace?} objects; namespace defaults to scene"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("align_actors"),
		TEXT("Align multiple actors along an axis. Modes: min/max (snap to extremes), center (average), distribute (spread evenly between min/max)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::AlignActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to align"))
			.Required(TEXT("axis"), TEXT("string"), TEXT("Axis to align on: X, Y, or Z"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Alignment mode: min, max, center, or distribute"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("snap_to_floor"),
		TEXT("Snap actors to the floor by tracing downward. Places the bottom of each actor on the hit surface."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SnapToFloor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to snap"))
			.Optional(TEXT("trace_distance"), TEXT("number"), TEXT("Maximum downward trace distance"), TEXT("10000"))
			.Build());

	Registry.RegisterAction(TEXT("scene"), TEXT("manage_folders"),
		TEXT("Manage World Outliner folders: list, delete (move actors to root), rename, or move folders."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::ManageFolders),
		FParamSchemaBuilder()
			.Required(TEXT("sub_action"), TEXT("string"), TEXT("Action: list, delete, rename, move"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Target folder path (for delete/rename/move)"))
			.OptionalAssetPath(TEXT("new_folder"), TEXT("New folder path (for rename/move destination)"))
			.Optional(TEXT("include_subfolders"), TEXT("boolean"), TEXT("Include actors in subfolders"), TEXT("true"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("scene"), TEXT("spawn_actor"), { TEXT("place actor"), TEXT("add to level") }, { TEXT("create actor") }, { TEXT("spawn a mesh at 0,0,100") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("scene"), TEXT("move_actor"), { TEXT("set transform"), TEXT("reposition") }, { TEXT("translate") }, { TEXT("move the chair to 200,0,0") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("scene"), TEXT("delete_actors"), { TEXT("remove from level"), TEXT("destroy actor") }, { TEXT("remove actor") }, { TEXT("delete the crate actors") });
}

// ============================================================================
// 1. get_actor_info
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::GetActorInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Actor->GetFName().ToString());
	Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("label"), Actor->GetActorLabel());

	// Transform
	Result->SetObjectField(TEXT("transform"), MonolithMeshUtils::TransformToJson(Actor->GetActorTransform()));

	// Mesh path and materials (if StaticMeshActor or has StaticMeshComponent)
	UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
	if (SMC)
	{
		if (UStaticMesh* Mesh = SMC->GetStaticMesh())
		{
			Result->SetStringField(TEXT("mesh"), Mesh->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> MatArr;
		for (int32 i = 0; i < SMC->GetNumMaterials(); ++i)
		{
			UMaterialInterface* Mat = SMC->GetMaterial(i);
			MatArr.Add(MakeShared<FJsonValueString>(Mat ? Mat->GetPathName() : TEXT("None")));
		}
		Result->SetArrayField(TEXT("materials"), MatArr);
	}

	// Mobility
	USceneComponent* Root = Actor->GetRootComponent();
	if (Root)
	{
		Result->SetStringField(TEXT("mobility"), SceneActionHelpers::MobilityToString(Root->Mobility));
	}

	// Tags
	TArray<TSharedPtr<FJsonValue>> TagArr;
	for (const FName& Tag : Actor->Tags)
	{
		TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Result->SetArrayField(TEXT("tags"), TagArr);

	// Components
	TArray<TSharedPtr<FJsonValue>> CompArr;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Comp : Components)
	{
		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetFName().ToString());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
		CompArr.Add(MakeShared<FJsonValueObject>(CompObj));
	}
	Result->SetArrayField(TEXT("components"), CompArr);

	// World bounds (for actors with a mesh)
	if (SMC && SMC->GetStaticMesh())
	{
		FBoxSphereBounds WorldBounds = SMC->Bounds;
		Result->SetObjectField(TEXT("world_bounds"), MonolithMeshUtils::BoundsToJson(WorldBounds));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. spawn_actor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::SpawnActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassOrMesh;
	if (!Params->TryGetStringField(TEXT("class_or_mesh"), ClassOrMesh))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: class_or_mesh"));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FVector Scale(1, 1, 1);
	MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale);

	FString OptionalName;
	Params->TryGetStringField(TEXT("name"), OptionalName);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Validate inputs before opening a transaction
	UStaticMesh* MeshToSpawn = nullptr;
	UClass* ClassToSpawn = nullptr;

	if (ClassOrMesh.StartsWith(TEXT("/")))
	{
		MeshToSpawn = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(ClassOrMesh);
		if (!MeshToSpawn)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("StaticMesh not found: %s"), *ClassOrMesh));
		}
	}
	else
	{
		// Block ABlockingVolume
		if (ClassOrMesh.Equals(TEXT("BlockingVolume"), ESearchCase::IgnoreCase) ||
			ClassOrMesh.Equals(TEXT("ABlockingVolume"), ESearchCase::IgnoreCase))
		{
			return FMonolithActionResult::Error(TEXT("Spawning BlockingVolume is not allowed via Monolith. Use the editor directly."));
		}

		ClassToSpawn = FindFirstObject<UClass>(*ClassOrMesh, EFindFirstObjectOptions::NativeFirst);
		if (!ClassToSpawn)
		{
			FString AltName = ClassOrMesh.StartsWith(TEXT("A")) ? ClassOrMesh.Mid(1) : (TEXT("A") + ClassOrMesh);
			ClassToSpawn = FindFirstObject<UClass>(*AltName, EFindFirstObjectOptions::NativeFirst);
		}
		if (!ClassToSpawn)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Class not found: %s"), *ClassOrMesh));
		}
		if (!ClassToSpawn->IsChildOf(AActor::StaticClass()))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not an Actor class"), *ClassOrMesh));
		}
		if (ClassToSpawn->IsChildOf(ABlockingVolume::StaticClass()))
		{
			return FMonolithActionResult::Error(TEXT("Spawning BlockingVolume is not allowed via Monolith. Use the editor directly."));
		}
	}

	// All validation passed — open transaction and spawn
	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Spawn Actor")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* SpawnedActor = nullptr;
	FString SpawnedClassName;

	if (MeshToSpawn)
	{
		AStaticMeshActor* SMActor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
		if (!SMActor)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Failed to spawn StaticMeshActor"));
		}
		SMActor->GetStaticMeshComponent()->SetStaticMesh(MeshToSpawn);
		SMActor->SetActorScale3D(Scale);
		SpawnedActor = SMActor;
		SpawnedClassName = TEXT("StaticMeshActor");
	}
	else
	{
		SpawnedActor = World->SpawnActor(ClassToSpawn, &Location, &Rotation, SpawnParams);
		if (!SpawnedActor)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to spawn actor of class '%s'"), *ClassOrMesh));
		}
		SpawnedActor->SetActorScale3D(Scale);
		SpawnedClassName = ClassToSpawn->GetName();
	}

	// Set optional label
	if (!OptionalName.IsEmpty())
	{
		SpawnedActor->SetActorLabel(OptionalName);
	}

	// Set folder — default to /Spawned if none specified
	if (!Folder.IsEmpty())
	{
		SpawnedActor->SetFolderPath(FName(*Folder));
	}
	else
	{
		SpawnedActor->SetFolderPath(FName(TEXT("Spawned")));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), SpawnedActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("class"), SpawnedClassName);
	Result->SetArrayField(TEXT("location"), SceneActionHelpers::VectorToJsonArray(SpawnedActor->GetActorLocation()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. move_actor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::MoveActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	USceneComponent* const RootComponent = Actor->GetRootComponent();
	UPackage* const OwningPackage = Actor->GetPackage();
	if (!RootComponent)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no root component and cannot persist a transform."), *ActorName));
	}
	if (Actor->HasAnyFlags(RF_Transient) || RootComponent->HasAnyFlags(RF_Transient) ||
		!OwningPackage || OwningPackage == GetTransientPackage())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' is transient and cannot be edited by the persistent move_actor action."), *ActorName));
	}

	bool bRelative = false;
	Params->TryGetBoolField(TEXT("relative"), bRelative);

	FVector Location;
	const bool bHasLocation = MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location);
	FRotator Rotation;
	const bool bHasRotation = MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);
	FVector Scale;
	const bool bHasScale = MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale);
	const FVector PreviousLocation = Actor->GetActorLocation();
	const FRotator PreviousRotation = Actor->GetActorRotation();
	const FVector PreviousScale = Actor->GetActorScale3D();

	const FVector TargetLocation = bHasLocation
		? (bRelative ? PreviousLocation + Location : Location)
		: PreviousLocation;
	const FRotator TargetRotation = bHasRotation
		? (bRelative ? PreviousRotation + Rotation : Rotation)
		: PreviousRotation;
	const FVector TargetScale = bHasScale
		? (bRelative ? PreviousScale + Scale : Scale)
		: PreviousScale;

	// Mirror the component setters' world-to-relative conversion before deciding
	// whether the requested fields can produce an effective edit. This matters
	// for attached actors: a world-space delta that is below tolerance can still
	// be a meaningful relative-space delta under a scaled parent (and vice versa).
	FVector EffectiveTargetLocation = TargetLocation;
	FQuat EffectiveTargetRotationQuat = TargetRotation.Quaternion();
	FVector EffectiveTargetScale = TargetScale;
	if (USceneComponent* const AttachParent = RootComponent->GetAttachParent())
	{
		const FTransform ParentToWorld = AttachParent->GetSocketTransform(RootComponent->GetAttachSocketName());
		if (FTransform::AnyHasNegativeScale(RootComponent->GetRelativeScale3D(), ParentToWorld.GetScale3D()))
		{
			const FTransform TargetWorldTransform(
				EffectiveTargetRotationQuat,
				TargetLocation,
				RootComponent->GetRelativeScale3D() * ParentToWorld.GetScale3D());
			const FTransform TargetRelativeTransform = TargetWorldTransform.GetRelativeTransform(ParentToWorld);
			if (!RootComponent->IsUsingAbsoluteLocation())
			{
				EffectiveTargetLocation = TargetRelativeTransform.GetLocation();
			}
			if (!RootComponent->IsUsingAbsoluteRotation())
			{
				EffectiveTargetRotationQuat = TargetRelativeTransform.GetRotation();
			}
		}
		else
		{
			if (!RootComponent->IsUsingAbsoluteLocation())
			{
				EffectiveTargetLocation = ParentToWorld.InverseTransformPosition(TargetLocation);
			}
			if (!RootComponent->IsUsingAbsoluteRotation())
			{
				EffectiveTargetRotationQuat = ParentToWorld.GetRotation().Inverse() * EffectiveTargetRotationQuat;
			}
		}

		if (!RootComponent->IsUsingAbsoluteScale())
		{
			EffectiveTargetScale = TargetScale * ParentToWorld.GetSafeScaleReciprocal(ParentToWorld.GetScale3D());
		}
	}
	const FRotator EffectiveTargetRotation = EffectiveTargetRotationQuat.Rotator();

	// Test only fields the caller asked to mutate. Match each transform setter's
	// effective-change semantics so an edit it would suppress is also a clean
	// no-op here instead of needlessly dirtying the actor's owning package.
	// Location/rotation setters use tolerant Equals checks; scale is applied by
	// SetRelativeScale3D using exact vector inequality. FRotator::Equals also
	// treats angle-equivalent values such as 0 and 360 degrees as the same pose.
	const bool bLocationChanged = bHasLocation && !EffectiveTargetLocation.Equals(RootComponent->GetRelativeLocation());
	const bool bRotationChanged = bHasRotation && !EffectiveTargetRotation.Equals(RootComponent->GetRelativeRotation());
	const bool bScaleChanged = bHasScale && EffectiveTargetScale != RootComponent->GetRelativeScale3D();
	const bool bTransformChanged = bLocationChanged || bRotationChanged || bScaleChanged;

	if (bTransformChanged)
	{
		// Establish the persistence postcondition before mutating the live actor.
		// MarkPackageDirty can be suppressed by the editor (for example during
		// loading); fail closed while the transform is still untouched in that case.
		if (!Actor->MarkPackageDirty() || !OwningPackage->IsDirty())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unable to dirty owning package '%s'; actor '%s' was not moved."),
					*OwningPackage->GetName(), *ActorName));
		}

		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Move Actor")));
		Actor->Modify(/*bAlwaysMarkDirty=*/false);
		RootComponent->Modify(/*bAlwaysMarkDirty=*/false);

		if (bHasLocation)
		{
			Actor->SetActorLocation(TargetLocation);
		}

		if (bHasRotation)
		{
			Actor->SetActorRotation(TargetRotation);
		}

		if (bHasScale)
		{
			Actor->SetActorScale3D(TargetScale);
		}

		// Match a normal editor transform edit: construction/editor listeners receive
		// the completed move, Undo owns both the actor and its root component state,
		// and the owning map or external-actor package becomes persistently savable.
		Actor->PostEditMove(/*bFinished=*/true);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetObjectField(TEXT("new_transform"), MonolithMeshUtils::TransformToJson(Actor->GetActorTransform()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. duplicate_actor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::DuplicateActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* SourceActor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!SourceActor)
	{
		return FMonolithActionResult::Error(Error);
	}

	FVector Offset(0, 0, 0);
	MonolithMeshUtils::ParseVector(Params, TEXT("offset"), Offset);

	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Duplicate Actor")));

	// Select the source actor and duplicate via editor
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(SourceActor, true, false, true);

	// Use editor duplication
	GEditor->edactDuplicateSelected(World->GetCurrentLevel(), false);

	// The duplicated actor is now the only selected actor
	AActor* DupActor = nullptr;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	for (int32 i = 0; i < SelectedActors->Num(); ++i)
	{
		AActor* Selected = Cast<AActor>(SelectedActors->GetSelectedObject(i));
		if (Selected && Selected != SourceActor)
		{
			DupActor = Selected;
			break;
		}
	}

	if (!DupActor)
	{
		Transaction.EndForFailure();
		return FMonolithActionResult::Error(TEXT("Failed to duplicate actor"));
	}

	// Apply offset
	DupActor->SetActorLocation(DupActor->GetActorLocation() + Offset);

	// Set label if requested
	if (!NewName.IsEmpty())
	{
		DupActor->SetActorLabel(NewName);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("original"), SourceActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("duplicate"), DupActor->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("location"), SceneActionHelpers::VectorToJsonArray(DupActor->GetActorLocation()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. delete_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::DeleteActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names (array of strings)"));
	}
	if (NamesArr->Num() > SceneActionHelpers::MaxDeleteActorsPerRequest)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("actor_names is capped at %d exact actors per request; got %d"),
			SceneActionHelpers::MaxDeleteActorsPerRequest,
			NamesArr->Num()));
	}

	UWorld* const World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}
	ULevel* const CurrentLevel = World->GetCurrentLevel();
	UPackage* const CurrentMapPackage = CurrentLevel ? CurrentLevel->GetOutermost() : nullptr;
	if (!CurrentLevel || !CurrentMapPackage)
	{
		return FMonolithActionResult::Error(
			TEXT("The editor world has no exact current level/map package"));
	}
	const FString CurrentMapPackageName = CurrentMapPackage->GetName();
	if (!GUnrealEd || !GUnrealEd->GetSelectedActors() ||
		!GUnrealEd->GetSelectedActors()->GetElementSelectionSet())
	{
		return SceneActionHelpers::DeleteActorsPreflightError(
			TEXT("The UE 5.8 actor-deletion service or editor selection set is unavailable"),
			CurrentMapPackageName,
			NamesArr->Num(),
			INDEX_NONE,
			FString());
	}

	// Phase 1: resolve and validate the complete set before the first side effect.
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());
	TArray<SceneActionHelpers::FDeleteActorTarget> Targets;
	Targets.Reserve(NamesArr->Num());
	TSet<FObjectKey> ResolvedActorKeys;

	for (int32 Index = 0; Index < NamesArr->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*NamesArr)[Index];
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(TEXT("actor_names[%d] must be a string. No actors deleted."), Index),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				FString());
		}

		FString RequestedIdentity = Value->AsString();
		RequestedIdentity.TrimStartAndEndInline();
		FString Error;
		AActor* const Actor = SceneActionHelpers::ResolveDeleteActor(
			World, RequestedIdentity, Error);
		if (!Actor)
		{
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(TEXT("%s. No actors deleted."), *Error),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				RequestedIdentity);
		}

		if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
		{
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(TEXT("Actor target '%s' is not a stable live actor. No actors deleted."), *RequestedIdentity),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				RequestedIdentity);
		}

		const FString ActorPath = Actor->GetPathName();
		const FObjectKey ActorKey(Actor);
		if (ResolvedActorKeys.Contains(ActorKey))
		{
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(
					TEXT("Duplicate actor target '%s' resolves to exact actor '%s'. No actors deleted."),
					*RequestedIdentity,
					*ActorPath),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				RequestedIdentity,
				ActorPath);
		}
		if (Actor->GetWorld() != World || Actor->GetLevel() != CurrentLevel ||
			Actor->GetLevel()->GetOutermost() != CurrentMapPackage)
		{
			const FString ActorMapPackage = Actor->GetLevel() && Actor->GetLevel()->GetOutermost()
				? Actor->GetLevel()->GetOutermost()->GetName()
				: FString(TEXT("<none>"));
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(
					TEXT("Actor '%s' belongs to map package '%s', not the current map package '%s'. No actors deleted."),
					*ActorPath,
					*ActorMapPackage,
					*CurrentMapPackageName),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				RequestedIdentity,
				ActorPath);
		}

		FText CannotDeleteReason;
		if (!GUnrealEd->CanDeleteActor(Actor, &CannotDeleteReason))
		{
			return SceneActionHelpers::DeleteActorsPreflightError(
				FString::Printf(
					TEXT("Actor '%s' cannot be deleted: %s. No actors deleted."),
					*ActorPath,
					*CannotDeleteReason.ToString()),
				CurrentMapPackageName,
				NamesArr->Num(),
				Index,
				RequestedIdentity,
				ActorPath);
		}

		ResolvedActorKeys.Add(ActorKey);
		Actors.Add(Actor);
		SceneActionHelpers::FDeleteActorTarget& Target = Targets.AddDefaulted_GetRef();
		Target.RequestedIdentity = RequestedIdentity;
		Target.ActorPath = ActorPath;
		Target.ActorName = Actor->GetFName().ToString();
		Target.ActorLabel = Actor->GetActorLabel();
		Target.ObjectKey = ActorKey;
	}

	FText AbortReason;
	if (GUnrealEd->ShouldAbortActorDeletion(Actors, &AbortReason))
	{
		return SceneActionHelpers::DeleteActorsPreflightError(
			FString::Printf(
				TEXT("UE 5.8 rejected the complete actor-deletion set: %s. No actors deleted."),
				*AbortReason.ToString()),
			CurrentMapPackageName,
			NamesArr->Num(),
			INDEX_NONE,
			FString());
	}

	// Phase 2: issue one direct UE deletion call. This avoids selection/component
	// ambiguity and keeps the exact preflight set in one undo transaction.
	TArray<AActor*> CommitActors = Actors;
#if WITH_DEV_AUTOMATION_TESTS
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		if (UE::MonolithScene::Private::ConsumeDeleteActorsTestFault(
			Targets[Index].ActorPath,
			UE::MonolithScene::Private::EDeleteActorsTestFault::SkipExactActorDuringCommit))
		{
			CommitActors.RemoveAt(Index);
			break;
		}
	}
#endif

	bool bEngineDeleteReturned = true;
	bool bUndoTransactionOpened = false;
	{
		SceneActionHelpers::FScopedMeshTransaction Transaction(
			FText::FromString(TEXT("Monolith: Delete Exact Actors")));
		bUndoTransactionOpened = bBatchTransactionActive
			? (GEditor && GEditor->IsTransactionActive())
			: Transaction.HasUndoTransaction();
		if (!CommitActors.IsEmpty())
		{
			bEngineDeleteReturned = GUnrealEd->DeleteActors(
				CommitActors,
				World,
				GUnrealEd->GetSelectedActors()->GetElementSelectionSet(),
				/*bVerifyDeletionCanHappen=*/true,
				/*bWarnAboutReferences=*/false,
				/*bWarnAboutSoftReferences=*/false);
		}
	}

	// Phase 3: exact identity/path read-back. UUnrealEdEngine::DeleteActors can
	// return true after skipping an undeletable actor, so its bool is not proof.
	TSet<FObjectKey> LiveActorKeys;
	TSet<FString> LiveActorPaths;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* const LiveActor = *It;
		if (LiveActor && IsValid(LiveActor) && !LiveActor->IsActorBeingDestroyed())
		{
			LiveActorKeys.Add(FObjectKey(LiveActor));
			LiveActorPaths.Add(LiveActor->GetPathName());
		}
	}

	int32 DeletedCount = 0;
	int32 SurvivorCount = 0;
	TArray<TSharedPtr<FJsonValue>> LegacyDeletedActors;
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	TArray<TSharedPtr<FJsonValue>> Survivors;
	for (const SceneActionHelpers::FDeleteActorTarget& Target : Targets)
	{
		const bool bIdentitySurvives = LiveActorKeys.Contains(Target.ObjectKey);
		const bool bPathOccupied = LiveActorPaths.Contains(Target.ActorPath);
		const bool bDeleted = !bIdentitySurvives && !bPathOccupied;

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("requested_identity"), Target.RequestedIdentity);
		Row->SetStringField(TEXT("actor_path"), Target.ActorPath);
		Row->SetStringField(TEXT("actor_name"), Target.ActorName);
		Row->SetStringField(TEXT("actor_label"), Target.ActorLabel);
		Row->SetBoolField(TEXT("deleted"), bDeleted);
		Row->SetBoolField(TEXT("exact_identity_survives"), bIdentitySurvives);
		Row->SetBoolField(TEXT("actor_path_occupied"), bPathOccupied);
		Row->SetStringField(
			TEXT("status"),
			bDeleted ? TEXT("deleted") :
				(bIdentitySurvives ? TEXT("exact_identity_survives") : TEXT("actor_path_reused")));
		ActorResults.Add(MakeShared<FJsonValueObject>(Row));

		if (bDeleted)
		{
			++DeletedCount;
			LegacyDeletedActors.Add(MakeShared<FJsonValueString>(Target.ActorLabel));
		}
		else
		{
			++SurvivorCount;
			Survivors.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("scene"));
	Result->SetStringField(TEXT("action"), TEXT("delete_actors"));
	Result->SetStringField(TEXT("map_package"), CurrentMapPackageName);
	Result->SetStringField(TEXT("level_path"), CurrentLevel->GetPathName());
	Result->SetNumberField(TEXT("requested_count"), Targets.Num());
	Result->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Result->SetNumberField(TEXT("survivor_count"), SurvivorCount);
	Result->SetNumberField(TEXT("deleted"), DeletedCount);
	Result->SetArrayField(TEXT("actors"), LegacyDeletedActors);
	Result->SetArrayField(TEXT("actor_results"), ActorResults);
	Result->SetArrayField(TEXT("survivors"), Survivors);
	Result->SetBoolField(TEXT("engine_delete_returned"), bEngineDeleteReturned);
	Result->SetBoolField(TEXT("exact_deletion_verified"), SurvivorCount == 0);
	Result->SetBoolField(TEXT("partial_failure"), DeletedCount > 0 && SurvivorCount > 0);
	Result->SetBoolField(TEXT("rollback_performed"), false);
	Result->SetBoolField(TEXT("requires_manual_undo"),
		bUndoTransactionOpened && DeletedCount > 0 && (!bEngineDeleteReturned || SurvivorCount > 0));
	Result->SetBoolField(TEXT("requires_manual_recovery"),
		DeletedCount > 0 && (!bEngineDeleteReturned || SurvivorCount > 0));
	Result->SetStringField(
		TEXT("transaction_scope"),
		bBatchTransactionActive ? TEXT("batch") : TEXT("action"));
	Result->SetBoolField(TEXT("undo_available"), bUndoTransactionOpened);

	if (!bEngineDeleteReturned || SurvivorCount > 0)
	{
		Result->SetStringField(
			TEXT("status"),
			DeletedCount > 0 ? TEXT("partial_failure") : TEXT("delete_failed"));
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("delete_actors did not satisfy its exact postcondition: deleted %d/%d, survivors=%d, engine_returned=%s. No rollback was claimed; any completed deletion remains applied%s."),
			DeletedCount,
			Targets.Num(),
			SurvivorCount,
			bEngineDeleteReturned ? TEXT("true") : TEXT("false"),
			bUndoTransactionOpened ? TEXT(" and is available in the reported undo transaction") : TEXT("; no undo transaction was available")))
			.WithErrorData(Result);
	}

	Result->SetStringField(TEXT("status"), TEXT("deleted"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. group_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::GroupActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	FString GroupName;
	if (!Params->TryGetStringField(TEXT("group_name"), GroupName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: group_name"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Group Actors")));

	int32 Count = 0;
	FString FirstError;
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			if (FirstError.IsEmpty())
			{
				FirstError = Error;
			}
			continue;
		}
		Actor->SetFolderPath(FName(*GroupName));
		Count++;
	}

	if (Count == 0 && !FirstError.IsEmpty())
	{
		Transaction.EndForFailure();
		return FMonolithActionResult::Error(FirstError);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("group"), GroupName);
	Result->SetNumberField(TEXT("actors"), Count);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. set_actor_properties
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::SetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	USceneComponent* Root = Actor->GetRootComponent();
	UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Root);

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Actor Properties")));

	TArray<TSharedPtr<FJsonValue>> PropertiesSet;

	// Mobility
	FString MobilityStr;
	if (Params->TryGetStringField(TEXT("mobility"), MobilityStr))
	{
		if (!Root)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Actor has no root component to set mobility on"));
		}
		EComponentMobility::Type Mob;
		if (!SceneActionHelpers::ParseMobility(MobilityStr, Mob))
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mobility: '%s'. Use Static, Stationary, or Movable."), *MobilityStr));
		}
		Root->SetMobility(Mob);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("mobility")));
	}

	// Simulate physics (must check mobility is Movable)
	bool bSimPhysics;
	if (Params->TryGetBoolField(TEXT("simulate_physics"), bSimPhysics))
	{
		if (!Primitive)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set physics"));
		}
		if (bSimPhysics && Root->Mobility != EComponentMobility::Movable)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Mobility must be Movable before enabling SimulatePhysics. Set mobility first."));
		}
		Primitive->SetSimulatePhysics(bSimPhysics);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("simulate_physics")));
	}

	// Collision preset
	FString CollisionPreset;
	if (Params->TryGetStringField(TEXT("collision_preset"), CollisionPreset))
	{
		if (!Primitive)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set collision"));
		}
		Primitive->SetCollisionProfileName(FName(*CollisionPreset));
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("collision_preset")));
	}

	// Cast shadow
	bool bCastShadow;
	if (Params->TryGetBoolField(TEXT("cast_shadow"), bCastShadow))
	{
		if (!Primitive)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set shadow"));
		}
		Primitive->SetCastShadow(bCastShadow);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("cast_shadow")));
	}

	// Tags
	const TArray<TSharedPtr<FJsonValue>>* TagsArr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		Actor->Tags.Empty();
		for (const auto& TagVal : *TagsArr)
		{
			Actor->Tags.Add(FName(*TagVal->AsString()));
		}
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("tags")));
	}

	// Mass override
	double MassKg;
	if (Params->TryGetNumberField(TEXT("mass_kg"), MassKg))
	{
		if (!Primitive)
		{
			Transaction.EndForFailure();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set mass"));
		}
		Primitive->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("mass_kg")));
	}

	if (PropertiesSet.Num() == 0)
	{
		Transaction.EndForFailure();
		return FMonolithActionResult::Error(TEXT("No properties provided to set. Specify at least one of: mobility, simulate_physics, collision_preset, cast_shadow, tags, mass_kg"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("properties_set"), PropertiesSet);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. batch_execute
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::BatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	// Reject nested batch_execute
	if (bBatchTransactionActive)
	{
		return FMonolithActionResult::Error(TEXT("Nested batch_execute is not allowed"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ActionsArr;
	if (!Params->TryGetArrayField(TEXT("actions"), ActionsArr) || ActionsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actions"));
	}

	if (ActionsArr->Num() > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("batch_execute is capped at 200 actions, got %d"), ActionsArr->Num()));
	}

	// Begin one outer transaction. A failure stops dispatch but still ends this
	// transaction so completed mutations remain available to Undo. UE's
	// CancelTransaction only discards the record; it is not a rollback API.
	const int32 BatchTransactionIndex = GEditor
		? GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Batch Execute")))
		: INDEX_NONE;
	const bool bUndoTransactionOpen = BatchTransactionIndex != INDEX_NONE;
	bBatchTransactionActive = true;

	int32 Succeeded = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	bool bHadFailure = false;
	bool bFailingActionReportedMutation = false;

	for (int32 i = 0; i < ActionsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* ActionObj;
		if (!(*ActionsArr)[i]->TryGetObject(ActionObj))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), TEXT("unknown"));
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Invalid action entry — expected object with 'action' and 'params' fields"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		FString ActionName;
		if (!(*ActionObj)->TryGetStringField(TEXT("action"), ActionName))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), TEXT("unknown"));
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Missing 'action' field in batch item"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		// Reject nested batch_execute
		if (ActionName == TEXT("batch_execute"))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), ActionName);
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Nested batch_execute is not allowed"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		// Get params (optional, default to empty object)
		TSharedPtr<FJsonObject> ActionParams = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* ParamsObj;
		if ((*ActionObj)->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			ActionParams = *ParamsObj;
		}

		FString ActionNamespace = TEXT("scene");
		(*ActionObj)->TryGetStringField(TEXT("namespace"), ActionNamespace);

		// Dispatch via registry
		FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(ActionNamespace, ActionName, ActionParams);

		auto ItemResult = MakeShared<FJsonObject>();
		ItemResult->SetStringField(TEXT("namespace"), ActionNamespace);
		ItemResult->SetStringField(TEXT("action"), ActionName);
		ItemResult->SetBoolField(TEXT("success"), ActionResult.bSuccess);
		if (!ActionResult.bSuccess)
		{
			ItemResult->SetStringField(TEXT("error"), ActionResult.ErrorMessage);
			if (ActionResult.ErrorData.IsValid())
			{
				ItemResult->SetObjectField(TEXT("error_data"), ActionResult.ErrorData);

				bool bPartialFailure = false;
				double DeletedCount = 0.0;
				ActionResult.ErrorData->TryGetBoolField(TEXT("partial_failure"), bPartialFailure);
				ActionResult.ErrorData->TryGetNumberField(TEXT("deleted_count"), DeletedCount);
				bFailingActionReportedMutation = bPartialFailure || DeletedCount > 0.0;
			}
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break; // Stop dispatch on first failure.
		}
		Succeeded++;
		ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
	}

	// Always close the transaction. Never represent CancelTransaction as an
	// automatic rollback: completed actions and a partially-mutated failing
	// action retain their actual state and their Undo record.
	bBatchTransactionActive = false;
	if (bUndoTransactionOpen && GEditor)
	{
		GEditor->EndTransaction();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), ActionsArr->Num());
	Result->SetNumberField(TEXT("succeeded"), Succeeded);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetStringField(
		TEXT("transaction_status"),
		!bUndoTransactionOpen ? TEXT("unavailable") :
			(bHadFailure ? TEXT("ended_after_failure_for_undo") : TEXT("committed")));
	Result->SetBoolField(TEXT("rollback_performed"), false);
	Result->SetBoolField(TEXT("undo_available"), bUndoTransactionOpen);
	Result->SetBoolField(TEXT("completed_actions_retained"), bHadFailure && Succeeded > 0);
	Result->SetBoolField(TEXT("failing_action_mutation_retained"), bFailingActionReportedMutation);
	Result->SetBoolField(
		TEXT("completed_mutations_may_be_retained"),
		bHadFailure && (Succeeded > 0 || bFailingActionReportedMutation));

	if (bHadFailure)
	{
		FString ErrorMsg = TEXT("Batch execution failed on an action");
		if (ResultsArr.Num() > 0)
		{
			const TSharedPtr<FJsonObject>* LastItem;
			if (ResultsArr.Last()->TryGetObject(LastItem))
			{
				FString InnerError;
				if ((*LastItem)->TryGetStringField(TEXT("error"), InnerError))
				{
					ErrorMsg = FString::Printf(TEXT("Batch execution failed: %s"), *InnerError);
				}
			}
		}
		ErrorMsg += bUndoTransactionOpen
			? TEXT(" Dispatch stopped at the first failure; no rollback was claimed. Any completed changes remain in the closed batch transaction for Undo.")
			: TEXT(" Dispatch stopped at the first failure; no rollback was claimed. Any completed changes remain applied, and no Undo transaction was available.");
		return FMonolithActionResult::Error(ErrorMsg).WithErrorData(Result);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. align_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::AlignActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or insufficient actor_names (need at least 2 actors to align)"));
	}

	FString AxisStr;
	if (!Params->TryGetStringField(TEXT("axis"), AxisStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: axis (X, Y, or Z)"));
	}
	AxisStr = AxisStr.ToUpper();
	int32 AxisIndex = -1;
	if (AxisStr == TEXT("X")) AxisIndex = 0;
	else if (AxisStr == TEXT("Y")) AxisIndex = 1;
	else if (AxisStr == TEXT("Z")) AxisIndex = 2;
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid axis: '%s'. Use X, Y, or Z."), *AxisStr));
	}

	FString ModeStr;
	if (!Params->TryGetStringField(TEXT("mode"), ModeStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: mode (min, max, center, or distribute)"));
	}
	ModeStr = ModeStr.ToLower();
	if (ModeStr != TEXT("min") && ModeStr != TEXT("max") && ModeStr != TEXT("center") && ModeStr != TEXT("distribute"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mode: '%s'. Use min, max, center, or distribute."), *ModeStr));
	}

	// Resolve all actors first — fail if any missing
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s"), *Name));
		}
		Actors.Add(Actor);
	}

	// Lambdas for axis access
	auto GetAxisValue = [AxisIndex](const FVector& V) -> double
	{
		return (AxisIndex == 0) ? V.X : (AxisIndex == 1) ? V.Y : V.Z;
	};

	auto SetAxisValue = [AxisIndex](FVector& V, double Val)
	{
		if (AxisIndex == 0) V.X = Val;
		else if (AxisIndex == 1) V.Y = Val;
		else V.Z = Val;
	};

	// Compute stats along the target axis
	double MinVal = TNumericLimits<double>::Max();
	double MaxVal = TNumericLimits<double>::Lowest();
	double Sum = 0.0;
	for (AActor* Actor : Actors)
	{
		double Val = GetAxisValue(Actor->GetActorLocation());
		MinVal = FMath::Min(MinVal, Val);
		MaxVal = FMath::Max(MaxVal, Val);
		Sum += Val;
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Align Actors")));

	TArray<TSharedPtr<FJsonValue>> ResultArr;
	ResultArr.Reserve(Actors.Num());

	if (ModeStr == TEXT("distribute"))
	{
		// Sort actors by current position on the target axis, then spread evenly
		struct FActorSortEntry
		{
			AActor* Actor;
			double AxisVal;
		};
		TArray<FActorSortEntry> Sorted;
		Sorted.Reserve(Actors.Num());
		for (AActor* Actor : Actors)
		{
			Sorted.Add({ Actor, GetAxisValue(Actor->GetActorLocation()) });
		}
		Sorted.Sort([](const FActorSortEntry& A, const FActorSortEntry& B) { return A.AxisVal < B.AxisVal; });

		double Range = MaxVal - MinVal;
		int32 Count = Sorted.Num();

		for (int32 i = 0; i < Count; ++i)
		{
			AActor* Actor = Sorted[i].Actor;
			double NewVal = (Count > 1) ? (MinVal + (Range * i) / (Count - 1)) : MinVal;
			FVector Loc = Actor->GetActorLocation();
			SetAxisValue(Loc, NewVal);
			Actor->SetActorLocation(Loc);

			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(Loc));
			ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	else
	{
		double Target = 0.0;
		if (ModeStr == TEXT("min")) Target = MinVal;
		else if (ModeStr == TEXT("max")) Target = MaxVal;
		else /* center */ Target = Sum / Actors.Num();

		for (AActor* Actor : Actors)
		{
			FVector Loc = Actor->GetActorLocation();
			SetAxisValue(Loc, Target);
			Actor->SetActorLocation(Loc);

			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(Loc));
			ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("axis"), AxisStr);
	Result->SetStringField(TEXT("mode"), ModeStr);
	Result->SetNumberField(TEXT("aligned"), ResultArr.Num());
	Result->SetArrayField(TEXT("actors"), ResultArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. snap_to_floor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::SnapToFloor(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	double TraceDistance = 10000.0;
	Params->TryGetNumberField(TEXT("trace_distance"), TraceDistance);
	if (TraceDistance <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("trace_distance must be positive"));
	}

	// Resolve all actors first — fail if any missing
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s"), *Name));
		}
		Actors.Add(Actor);
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Snap to Floor")));

	TArray<TSharedPtr<FJsonValue>> ResultArr;
	ResultArr.Reserve(Actors.Num());
	int32 Snapped = 0;
	int32 Missed = 0;

	for (AActor* Actor : Actors)
	{
		// Get actor bounds to compute half-height (distance from center to bottom)
		FVector BoundsOrigin, BoundsExtent;
		Actor->GetActorBounds(false, BoundsOrigin, BoundsExtent);
		double ActorHalfHeight = BoundsExtent.Z; // Extent is half-size per axis

		FVector ActorLoc = Actor->GetActorLocation();

		// Trace downward from actor location
		FVector TraceStart = ActorLoc;
		FVector TraceEnd = TraceStart - FVector(0, 0, TraceDistance);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SnapToFloor), true);
		QueryParams.AddIgnoredActor(Actor);

		FHitResult Hit;
		bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());

		if (bHit)
		{
			// Place actor so its bottom sits on the hit point.
			// Account for pivot offset: the actor's pivot may not be at bounds center.
			// PivotOffset = ActorLocation.Z - BoundsOrigin.Z
			// NewZ = HitPoint.Z + ActorHalfHeight + PivotOffset
			double PivotOffset = ActorLoc.Z - BoundsOrigin.Z;
			double NewZ = Hit.ImpactPoint.Z + ActorHalfHeight + PivotOffset;
			FVector NewLoc = ActorLoc;
			NewLoc.Z = NewZ;
			Actor->SetActorLocation(NewLoc);

			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(NewLoc));
			Entry->SetNumberField(TEXT("floor_z"), Hit.ImpactPoint.Z);
			Entry->SetBoolField(TEXT("snapped"), true);
			Snapped++;
		}
		else
		{
			Entry->SetBoolField(TEXT("snapped"), false);
			Entry->SetStringField(TEXT("reason"), TEXT("No floor found within trace distance"));
			Missed++;
		}

		ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("snapped"), Snapped);
	Result->SetNumberField(TEXT("missed"), Missed);
	Result->SetNumberField(TEXT("trace_distance"), TraceDistance);
	Result->SetArrayField(TEXT("actors"), ResultArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// manage_folders
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::ManageFolders(const TSharedPtr<FJsonObject>& Params)
{
	FString SubAction;
	if (!Params->TryGetStringField(TEXT("sub_action"), SubAction))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: sub_action (list, delete, rename, move)"));
	}
	SubAction = SubAction.ToLower();

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// ---- LIST: enumerate all folder paths in use ----
	if (SubAction == TEXT("list"))
	{
		TMap<FString, int32> FolderCounts;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FName FolderPath = It->GetFolderPath();
			if (!FolderPath.IsNone())
			{
				FolderCounts.FindOrAdd(FolderPath.ToString())++;
			}
		}

		TArray<TSharedPtr<FJsonValue>> FolderArr;
		FolderArr.Reserve(FolderCounts.Num());
		FolderCounts.KeySort([](const FString& A, const FString& B) { return A < B; });
		for (const auto& Pair : FolderCounts)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("folder"), Pair.Key);
			Obj->SetNumberField(TEXT("actor_count"), Pair.Value);
			FolderArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("folders"), FolderArr);
		Result->SetNumberField(TEXT("total_folders"), FolderArr.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ---- Remaining sub-actions need a folder param ----
	FString Folder;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || Folder.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: folder (for delete/rename/move)"));
	}

	bool bIncludeSubfolders = true;
	Params->TryGetBoolField(TEXT("include_subfolders"), bIncludeSubfolders);

	// Collect actors in the target folder
	TArray<AActor*> FolderActors;
	FName FolderFName(*Folder);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FName ActorFolder = It->GetFolderPath();
		if (ActorFolder == FolderFName)
		{
			FolderActors.Add(*It);
		}
		else if (bIncludeSubfolders && !ActorFolder.IsNone())
		{
			FString ActorFolderStr = ActorFolder.ToString();
			if (ActorFolderStr.StartsWith(Folder + TEXT("/")))
			{
				FolderActors.Add(*It);
			}
		}
	}

	// ---- DELETE: move all actors to root (folder vanishes when empty) ----
	if (SubAction == TEXT("delete"))
	{
		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Delete Folder")));

		for (AActor* Actor : FolderActors)
		{
			Actor->SetFolderPath(NAME_None);
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("deleted_folder"), Folder);
		Result->SetNumberField(TEXT("actors_moved_to_root"), FolderActors.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ---- RENAME: change folder path for all actors ----
	if (SubAction == TEXT("rename"))
	{
		FString NewFolder;
		if (!Params->TryGetStringField(TEXT("new_folder"), NewFolder) || NewFolder.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: new_folder (for rename)"));
		}

		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Rename Folder")));

		int32 Renamed = 0;
		for (AActor* Actor : FolderActors)
		{
			FString ActorFolderStr = Actor->GetFolderPath().ToString();
			if (ActorFolderStr == Folder)
			{
				Actor->SetFolderPath(FName(*NewFolder));
			}
			else if (bIncludeSubfolders)
			{
				// Preserve subfolder structure: "OldFolder/Sub" -> "NewFolder/Sub"
				FString Remainder = ActorFolderStr.Mid(Folder.Len());
				Actor->SetFolderPath(FName(*(NewFolder + Remainder)));
			}
			Renamed++;
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("old_folder"), Folder);
		Result->SetStringField(TEXT("new_folder"), NewFolder);
		Result->SetNumberField(TEXT("actors_renamed"), Renamed);
		return FMonolithActionResult::Success(Result);
	}

	// ---- MOVE: move all actors into a different folder ----
	if (SubAction == TEXT("move"))
	{
		FString NewFolder;
		if (!Params->TryGetStringField(TEXT("new_folder"), NewFolder) || NewFolder.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: new_folder (for move destination)"));
		}

		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Move Folder")));

		for (AActor* Actor : FolderActors)
		{
			Actor->SetFolderPath(FName(*NewFolder));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_folder"), Folder);
		Result->SetStringField(TEXT("destination_folder"), NewFolder);
		Result->SetNumberField(TEXT("actors_moved"), FolderActors.Num());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("Unknown sub_action: '%s'. Valid: list, delete, rename, move"), *SubAction));
}
