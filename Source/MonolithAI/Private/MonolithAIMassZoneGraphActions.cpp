#include "MonolithAIMassZoneGraphActions.h"

#include "MonolithAsyncJobRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Modules/ModuleManager.h"

#if WITH_ZONEGRAPH
// P1b (PRD Spec 10): broadcasting OnZoneGraphRequestRebuild is the only public,
// supported editor entry point to drive a full ZoneGraph rebuild — the
// UZoneGraphSubsystem::RebuildGraph / FZoneGraphBuilder::RequestRebuild paths are
// protected. The subsystem subscribes to this delegate and runs the complete
// orchestrated rebuild (stale-data cleanup, registration, missing-data spawn,
// Builder.BuildAll) synchronously on the broadcast.
#include "ZoneGraphDelegates.h"
#endif

namespace
{
	int32 ClampMassLimit(double Value)
	{
		return FMath::Clamp(static_cast<int32>(Value), 1, 500);
	}

	bool ClassContains(const UObject* Object, const TCHAR* Needle)
	{
		if (!Object || !Object->GetClass())
		{
			return false;
		}
		return Object->GetClass()->GetName().Contains(Needle, ESearchCase::IgnoreCase)
			|| Object->GetClass()->GetClassPathName().ToString().Contains(Needle, ESearchCase::IgnoreCase);
	}

	bool IsMassSpawnerLike(const AActor* Actor)
	{
		return ClassContains(Actor, TEXT("MassSpawner"));
	}

	bool IsZoneShapeLike(const AActor* Actor)
	{
		return ClassContains(Actor, TEXT("ZoneShape")) || ClassContains(Actor, TEXT("ZoneGraph"));
	}

	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Value.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Arr;
	}

	bool TryReadLocationObject(const TSharedPtr<FJsonObject>& Params, FVector& OutLocation, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* LocationPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("location"), LocationPtr) || !LocationPtr || !LocationPtr->IsValid())
		{
			OutError = TEXT("Missing required param 'location' (object with x, y, z)");
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*LocationPtr)->TryGetNumberField(TEXT("x"), X)
			|| !(*LocationPtr)->TryGetNumberField(TEXT("y"), Y)
			|| !(*LocationPtr)->TryGetNumberField(TEXT("z"), Z))
		{
			OutError = TEXT("'location' must contain numeric x, y, and z fields");
			return false;
		}

		OutLocation = FVector(X, Y, Z);
		return true;
	}

	bool TryParseTupleVector(const FString& Text, FVector& OutVector)
	{
		FString Clean = Text;
		Clean.ReplaceInline(TEXT("("), TEXT(""));
		Clean.ReplaceInline(TEXT(")"), TEXT(""));
		Clean.ReplaceInline(TEXT(" "), TEXT(""));

		TArray<FString> Parts;
		Clean.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 3)
		{
			return false;
		}

		OutVector = FVector(
			FCString::Atod(*Parts[0]),
			FCString::Atod(*Parts[1]),
			FCString::Atod(*Parts[2]));
		return true;
	}

	double DistanceSquaredToSegment(const FVector& Point, const FVector& SegmentStart, const FVector& SegmentEnd)
	{
		const FVector Segment = SegmentEnd - SegmentStart;
		const double SegmentLengthSquared = Segment.SizeSquared();
		if (SegmentLengthSquared <= SMALL_NUMBER)
		{
			return FVector::DistSquared(Point, SegmentStart);
		}

		const double Alpha = FMath::Clamp(FVector::DotProduct(Point - SegmentStart, Segment) / SegmentLengthSquared, 0.0, 1.0);
		const FVector ClosestPoint = SegmentStart + Segment * Alpha;
		return FVector::DistSquared(Point, ClosestPoint);
	}

	TSharedPtr<FJsonObject> CloneJsonObjectFields(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (!Source.IsValid())
		{
			return Clone;
		}

		for (const auto& Pair : FMonolithJsonUtils::GetFields(Source))
		{
			Clone->SetField(Pair.Key, Pair.Value);
		}
		return Clone;
	}

	TSharedPtr<FJsonObject> BuildModuleStatus(const TCHAR* ModuleName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("module"), ModuleName);
		Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
		Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(ModuleName));
		return Row;
	}

	UWorld* FindWorld(const TSharedPtr<FJsonObject>& Params, FString& OutContext, FString& OutError)
	{
		FString Requested = TEXT("editor");
		Params->TryGetStringField(TEXT("world_context"), Requested);

		if (Requested.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
		{
			if (GEngine)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE && Context.World())
					{
						OutContext = TEXT("pie");
						return Context.World();
					}
				}
			}
			OutError = TEXT("No PIE world available");
			return nullptr;
		}

		if (GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				OutContext = TEXT("editor");
				return World;
			}
		}

		OutError = TEXT("No editor world available");
		return nullptr;
	}

	TSharedPtr<FJsonObject> MakeActorRow(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Row;
		}

		Row->SetStringField(TEXT("name"), Actor->GetFName().ToString());
		Row->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Row->SetStringField(TEXT("path"), Actor->GetPathName());
		Row->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : TEXT(""));
		Row->SetStringField(TEXT("class_path"), Actor->GetClass() ? Actor->GetClass()->GetClassPathName().ToString() : TEXT(""));
		Row->SetArrayField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
		Row->SetArrayField(TEXT("scale"), VectorToJson(Actor->GetActorScale3D()));

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		TArray<TSharedPtr<FJsonValue>> ComponentRows;
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			if (ClassContains(Component, TEXT("Mass")) || ClassContains(Component, TEXT("Zone")))
			{
				TSharedPtr<FJsonObject> ComponentRow = MakeShared<FJsonObject>();
				ComponentRow->SetStringField(TEXT("name"), Component->GetName());
				ComponentRow->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				ComponentRow->SetStringField(TEXT("class_path"), Component->GetClass()->GetClassPathName().ToString());
				ComponentRows.Add(MakeShared<FJsonValueObject>(ComponentRow));
			}
		}
		Row->SetArrayField(TEXT("mass_zone_components"), ComponentRows);
		return Row;
	}

	AActor* ResolveActor(const TSharedPtr<FJsonObject>& Params, bool (*Predicate)(const AActor*), FString& OutError)
	{
		FString Query;
		Params->TryGetStringField(TEXT("actor"), Query);
		if (Query.IsEmpty())
		{
			Params->TryGetStringField(TEXT("actor_name"), Query);
		}
		if (Query.IsEmpty())
		{
			Params->TryGetStringField(TEXT("actor_path"), Query);
		}
		if (Query.IsEmpty())
		{
			OutError = TEXT("Missing required param: actor, actor_name, or actor_path");
			return nullptr;
		}

		FString Context;
		UWorld* World = FindWorld(Params, Context, OutError);
		if (!World)
		{
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !Predicate(Actor))
			{
				continue;
			}

			if (Actor->GetPathName().Equals(Query, ESearchCase::IgnoreCase)
				|| Actor->GetFName().ToString().Equals(Query, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(Query, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		OutError = FString::Printf(TEXT("Actor not found: %s"), *Query);
		return nullptr;
	}

	FMonolithActionResult ListActorsByPredicate(const TSharedPtr<FJsonObject>& Params, bool (*Predicate)(const AActor*), const FString& FieldName)
	{
		double LimitValue = 100.0;
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		const int32 Limit = ClampMassLimit(LimitValue);

		FString WorldContext;
		FString Error;
		UWorld* World = FindWorld(Params, WorldContext, Error);
		if (!World)
		{
			return FMonolithActionResult::Error(Error);
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 MatchedCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Predicate(Actor))
			{
				continue;
			}
			++MatchedCount;
			if (Rows.Num() < Limit)
			{
				Rows.Add(MakeShared<FJsonValueObject>(MakeActorRow(Actor)));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("world"), World->GetPathName());
		Result->SetStringField(TEXT("world_context"), WorldContext);
		Result->SetNumberField(TEXT("matched_count"), MatchedCount);
		Result->SetNumberField(TEXT("returned_count"), Rows.Num());
		Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
		Result->SetArrayField(FieldName, Rows);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult MakeUnavailable(const FString& Action, const TSharedPtr<FJsonObject>& Params, const FString& Reason)
	{
		bool bConfirm = false;
		Params->TryGetBoolField(TEXT("confirm"), bConfirm);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("action"), Action);
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetBoolField(TEXT("confirm_received"), bConfirm);
		Result->SetStringField(TEXT("reason"), Reason);
		Result->SetBoolField(TEXT("requires_explicit_world_context"), true);
		return FMonolithActionResult::Success(Result);
	}
}

void FMonolithAIMassZoneGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("ai"), TEXT("list_mass_spawners"),
		TEXT("List loaded MassSpawner-like actors in an explicit editor or PIE world context."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::ListMassSpawners),
		FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows"), TEXT("100")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("get_mass_spawner"),
		TEXT("Inspect one MassSpawner-like actor by label, name, or path."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::GetMassSpawner),
		FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Spawner actor label, name, or path")).Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("spawn_mass_spawner"),
		TEXT("Report MassSpawner spawn availability; direct spawning is guarded until class-specific tests exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SpawnMassSpawner),
		FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("despawn_mass_spawner"),
		TEXT("Report MassSpawner despawn availability; direct despawn is guarded until PIE/editor-world tests exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::DespawnMassSpawner),
		FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Spawner actor")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("set_mass_spawner_count"),
		TEXT("Report MassSpawner count mutation availability without mutating runtime state."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SetMassSpawnerCount),
		FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Spawner actor")).Required(TEXT("count"), TEXT("integer"), TEXT("Desired count")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("set_mass_spawner_scale"),
		TEXT("Report MassSpawner scale mutation availability without mutating runtime state."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SetMassSpawnerScale),
		FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Spawner actor")).Required(TEXT("scale"), TEXT("array"), TEXT("[x,y,z] scale")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("get_mass_simulation_status"),
		TEXT("Report Mass/ZoneGraph module availability, world context, and discovered runtime actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::GetMassSimulationStatus),
		FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("pause_mass_simulation"),
		TEXT("Report Mass simulation pause availability; refuses ambiguous editor-vs-PIE state."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::PauseMassSimulation),
		FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("pie")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("resume_mass_simulation"),
		TEXT("Report Mass simulation resume availability; refuses ambiguous editor-vs-PIE state."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::ResumeMassSimulation),
		FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("pie")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());

	Registry.RegisterAction(TEXT("ai"), TEXT("list_zone_shapes"), TEXT("List loaded ZoneShape/ZoneGraph-like actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::ListZoneShapes),
		FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows"), TEXT("100")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("get_zone_shape"), TEXT("Inspect one ZoneShape/ZoneGraph-like actor."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::GetZoneShape),
		FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path")).Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("spawn_zone_shape"), TEXT("Report ZoneShape spawn availability without mutating levels."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SpawnZoneShape), FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_zone_shape"), TEXT("Report ZoneShape removal availability without mutating levels."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::RemoveZoneShape), FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Actor")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("set_zone_shape_points"), TEXT("Report ZoneShape point mutation availability without mutating levels."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SetZoneShapePoints), FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Actor")).Required(TEXT("points"), TEXT("array"), TEXT("Shape points")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("set_zone_shape_tags"), TEXT("Report ZoneShape tag mutation availability without mutating levels."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SetZoneShapeTags), FParamSchemaBuilder().Required(TEXT("actor"), TEXT("string"), TEXT("Actor")).Required(TEXT("tags"), TEXT("array"), TEXT("Zone tags")).Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("rebuild_zone_graph"), TEXT("Report ZoneGraph rebuild availability without starting long-running rebuilds."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::RebuildZoneGraph), FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("list_zone_lane_profiles"), TEXT("Return conservative lane-profile discovery metadata."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::ListZoneLaneProfiles), FParamSchemaBuilder().Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("list_zone_tags"), TEXT("Return tag names found on loaded ZoneShape/ZoneGraph-like actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::ListZoneTags), FParamSchemaBuilder().Optional(TEXT("world_context"), TEXT("string"), TEXT("editor or pie"), TEXT("editor")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("find_nearest_zone_lane"), TEXT("Return the nearest ZoneGraph lane from overlapping candidates."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::FindNearestZoneLane), FParamSchemaBuilder().Required(TEXT("location"), TEXT("object"), TEXT("{x,y,z}")).Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius"), TEXT("1000")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("find_overlapping_zone_lanes"), TEXT("Delegate to ai.query_zone_lanes when ZoneGraph support is registered."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::FindOverlappingZoneLanes), FParamSchemaBuilder().Required(TEXT("location"), TEXT("object"), TEXT("{x,y,z}")).Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius"), TEXT("1000")).Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("get_crowd_lane_state"), TEXT("Report MassCrowd lane-state availability."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::GetCrowdLaneState), FParamSchemaBuilder().Build());
	Registry.RegisterAction(TEXT("ai"), TEXT("set_crowd_lane_state"), TEXT("Report MassCrowd lane-state mutation availability."),
		FMonolithActionHandler::CreateStatic(&FMonolithAIMassZoneGraphActions::SetCrowdLaneState), FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false")).Build());
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::ListMassSpawners(const TSharedPtr<FJsonObject>& Params)
{
	return ListActorsByPredicate(Params, &IsMassSpawnerLike, TEXT("spawners"));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::GetMassSpawner(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	if (AActor* Actor = ResolveActor(Params, &IsMassSpawnerLike, Error))
	{
		return FMonolithActionResult::Success(MakeActorRow(Actor));
	}
	return FMonolithActionResult::Error(Error);
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SpawnMassSpawner(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.spawn_mass_spawner"), Params, TEXT("MassSpawner spawning requires class-specific construction and PIE/editor-world mutation tests."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::DespawnMassSpawner(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.despawn_mass_spawner"), Params, TEXT("MassSpawner despawn remains guarded until runtime ownership and transaction semantics are verified."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SetMassSpawnerCount(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.set_mass_spawner_count"), Params, TEXT("MassSpawner count mutation remains guarded because properties vary by plugin version."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SetMassSpawnerScale(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.set_mass_spawner_scale"), Params, TEXT("MassSpawner scale mutation remains guarded because runtime/editor-world state must be explicit."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::GetMassSimulationStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	FString WorldContext;
	FString Error;
	UWorld* World = FindWorld(Params, WorldContext, Error);
	Result->SetStringField(TEXT("world_context"), WorldContext);
	Result->SetStringField(TEXT("world"), World ? World->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("world_available"), World != nullptr);
	if (!Error.IsEmpty())
	{
		Result->SetStringField(TEXT("world_warning"), Error);
	}

	TArray<TSharedPtr<FJsonValue>> Modules;
	for (const TCHAR* ModuleName : { TEXT("MassEntity"), TEXT("MassSpawner"), TEXT("MassCrowd"), TEXT("ZoneGraph") })
	{
		Modules.Add(MakeShared<FJsonValueObject>(BuildModuleStatus(ModuleName)));
	}
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetBoolField(TEXT("compiled_with_massentity"),
#if WITH_MASSENTITY
		true
#else
		false
#endif
	);
	Result->SetBoolField(TEXT("compiled_with_zonegraph"),
#if WITH_ZONEGRAPH
		true
#else
		false
#endif
	);

	int32 SpawnerCount = 0;
	int32 ZoneShapeCount = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsMassSpawnerLike(*It))
			{
				++SpawnerCount;
			}
			if (IsZoneShapeLike(*It))
			{
				++ZoneShapeCount;
			}
		}
	}
	Result->SetNumberField(TEXT("mass_spawner_count"), SpawnerCount);
	Result->SetNumberField(TEXT("zone_shape_count"), ZoneShapeCount);
	Result->SetStringField(TEXT("simulation_control_status"), TEXT("inspection_only"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::PauseMassSimulation(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.pause_mass_simulation"), Params, TEXT("Mass simulation pause needs plugin-version-specific subsystem control and is not invoked from MCP yet."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::ResumeMassSimulation(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.resume_mass_simulation"), Params, TEXT("Mass simulation resume needs plugin-version-specific subsystem control and is not invoked from MCP yet."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::ListZoneShapes(const TSharedPtr<FJsonObject>& Params)
{
	return ListActorsByPredicate(Params, &IsZoneShapeLike, TEXT("zone_shapes"));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::GetZoneShape(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	if (AActor* Actor = ResolveActor(Params, &IsZoneShapeLike, Error))
	{
		return FMonolithActionResult::Success(MakeActorRow(Actor));
	}
	return FMonolithActionResult::Error(Error);
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SpawnZoneShape(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.spawn_zone_shape"), Params, TEXT("ZoneShape spawning is unavailable until class resolution, transactions, and package dirtiness are verified."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::RemoveZoneShape(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.remove_zone_shape"), Params, TEXT("ZoneShape removal is destructive and remains guarded until transaction tests exist."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SetZoneShapePoints(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.set_zone_shape_points"), Params, TEXT("ZoneShape point editing remains unavailable until spline/shape property contracts are verified."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SetZoneShapeTags(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.set_zone_shape_tags"), Params, TEXT("ZoneShape tag editing remains unavailable until tag serialization semantics are verified."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::RebuildZoneGraph(const TSharedPtr<FJsonObject>& Params)
{
	// P1b (PRD Spec 10): gated behind UMonolithSettings::bEnableZoneGraphRebuildJob.
	// Off preserves the byte-identical legacy "unavailable" report.
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bEnableZoneGraphRebuildJob)
	{
		return MakeUnavailable(TEXT("ai.rebuild_zone_graph"), Params, TEXT("ZoneGraph rebuild can be long-running and remains unavailable until progress reporting is added."));
	}

	// Mint a job up front so callers always get a pollable job_id (monolith.get_job)
	// even on the failure paths below.
	FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
	const FString JobId = JobRegistry.SubmitJob(TEXT("ai"), TEXT("rebuild_zone_graph"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), TEXT("ai.rebuild_zone_graph"));
	Result->SetStringField(TEXT("job_id"), JobId);
	Result->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));

#if WITH_ZONEGRAPH
	// GEditor is required: the ZoneGraph rebuild delegate is WITH_EDITOR-only and
	// drives editor-world data. Refuse honestly (FailJob, no fake completion) when
	// the editor is unavailable (e.g. a cooked/-game process).
	if (!GEditor)
	{
		JobRegistry.FailJob(JobId, TEXT("GEditor is unavailable; ZoneGraph rebuild requires the editor."));
		Result->SetStringField(TEXT("status"), TEXT("failed"));
		Result->SetStringField(TEXT("reason"), TEXT("GEditor is unavailable; ZoneGraph rebuild requires the editor."));
		return FMonolithActionResult::Success(Result);
	}

	JobRegistry.UpdateProgress(JobId, 10.0, TEXT("rebuilding"), TEXT("Broadcasting OnZoneGraphRequestRebuild."));

	// The subsystem's OnRequestRebuild handler runs RebuildGraph(true) synchronously
	// on this broadcast, so when Broadcast() returns the rebuild has completed. There
	// is no separate completion delegate to wait on, so reporting Completed here is
	// honest rather than faked.
	UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild.Broadcast();

	TSharedPtr<FJsonObject> JobResult = MakeShared<FJsonObject>();
	JobResult->SetStringField(TEXT("rebuild_trigger"), TEXT("OnZoneGraphRequestRebuild"));
	JobResult->SetStringField(TEXT("note"), TEXT("Synchronous editor ZoneGraph rebuild broadcast completed."));
	JobRegistry.UpdateProgress(JobId, 100.0, TEXT("completed"), TEXT("ZoneGraph rebuild broadcast completed."));
	JobRegistry.CompleteJob(JobId, JobResult);

	Result->SetStringField(TEXT("status"), TEXT("completed"));
	Result->SetStringField(TEXT("rebuild_trigger"), TEXT("OnZoneGraphRequestRebuild"));
	return FMonolithActionResult::Success(Result);
#else
	// ZoneGraph compiled out of this build: the rebuild cannot run. Fail the job
	// honestly instead of pretending it completed.
	JobRegistry.FailJob(JobId, TEXT("ZoneGraph support is not compiled into this build (WITH_ZONEGRAPH=0)."));
	Result->SetStringField(TEXT("status"), TEXT("failed"));
	Result->SetStringField(TEXT("reason"), TEXT("ZoneGraph support is not compiled into this build (WITH_ZONEGRAPH=0)."));
	return FMonolithActionResult::Success(Result);
#endif // WITH_ZONEGRAPH
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::ListZoneLaneProfiles(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("inspection_only"));
	Result->SetBoolField(TEXT("query_zone_lanes_registered"), FMonolithToolRegistry::Get().HasAction(TEXT("ai"), TEXT("query_zone_lanes")));
	Result->SetStringField(TEXT("reason"), TEXT("Lane profile assets are plugin-version-specific; use ai.query_zone_lanes for currently registered lane queries."));
	Result->SetArrayField(TEXT("profiles"), TArray<TSharedPtr<FJsonValue>>());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::ListZoneTags(const TSharedPtr<FJsonObject>& Params)
{
	FString WorldContext;
	FString Error;
	UWorld* World = FindWorld(Params, WorldContext, Error);
	if (!World)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSet<FString> Tags;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsZoneShapeLike(Actor))
		{
			continue;
		}
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(Tag.ToString());
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& Tag : Tags)
	{
		Rows.Add(MakeShared<FJsonValueString>(Tag));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetStringField(TEXT("world_context"), WorldContext);
	Result->SetNumberField(TEXT("tag_count"), Tags.Num());
	Result->SetArrayField(TEXT("tags"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::FindNearestZoneLane(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (Registry.HasAction(TEXT("ai"), TEXT("query_zone_lanes")))
	{
		FVector QueryLocation;
		FString LocationError;
		if (!TryReadLocationObject(Params, QueryLocation, LocationError))
		{
			return FMonolithActionResult::Error(LocationError);
		}

		FMonolithActionResult QueryResult = Registry.ExecuteAction(TEXT("ai"), TEXT("query_zone_lanes"), Params);
		if (!QueryResult.bSuccess || !QueryResult.Result.IsValid())
		{
			return QueryResult;
		}

		const TArray<TSharedPtr<FJsonValue>>* Lanes = nullptr;
		if (!QueryResult.Result->TryGetArrayField(TEXT("lanes"), Lanes) || !Lanes || Lanes->Num() == 0)
		{
			QueryResult.Result->SetNumberField(TEXT("overlap_count"), 0);
			return QueryResult;
		}
		const int32 OverlapCount = Lanes->Num();

		if (!Registry.HasAction(TEXT("ai"), TEXT("get_zone_lane_info")))
		{
			return FMonolithActionResult::Error(TEXT("ai.get_zone_lane_info is required to compute the nearest ZoneGraph lane"));
		}

		double BestDistanceSquared = TNumericLimits<double>::Max();
		TSharedPtr<FJsonObject> BestLane;
		int32 InspectedLaneCount = 0;

		for (const TSharedPtr<FJsonValue>& LaneValue : *Lanes)
		{
			if (!LaneValue.IsValid() || LaneValue->Type != EJson::Object)
			{
				continue;
			}

			TSharedPtr<FJsonObject> LaneObject = LaneValue->AsObject();
			if (!LaneObject.IsValid())
			{
				continue;
			}

			double LaneIndex = 0.0;
			if (!LaneObject->TryGetNumberField(TEXT("lane_index"), LaneIndex))
			{
				continue;
			}

			TSharedPtr<FJsonObject> LaneInfoParams = MakeShared<FJsonObject>();
			LaneInfoParams->SetNumberField(TEXT("lane_handle"), LaneIndex);
			double DataHandle = 0.0;
			if (LaneObject->TryGetNumberField(TEXT("data_handle"), DataHandle))
			{
				LaneInfoParams->SetNumberField(TEXT("data_handle"), DataHandle);
			}
			FMonolithActionResult LaneInfoResult = Registry.ExecuteAction(TEXT("ai"), TEXT("get_zone_lane_info"), LaneInfoParams);
			if (!LaneInfoResult.bSuccess || !LaneInfoResult.Result.IsValid())
			{
				continue;
			}

			FString StartText;
			FString EndText;
			FVector StartPoint;
			FVector EndPoint;
			if (!LaneInfoResult.Result->TryGetStringField(TEXT("start_point"), StartText)
				|| !LaneInfoResult.Result->TryGetStringField(TEXT("end_point"), EndText)
				|| !TryParseTupleVector(StartText, StartPoint)
				|| !TryParseTupleVector(EndText, EndPoint))
			{
				continue;
			}

			++InspectedLaneCount;
			const double DistanceSquared = DistanceSquaredToSegment(QueryLocation, StartPoint, EndPoint);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestLane = CloneJsonObjectFields(LaneObject);
				BestLane->SetObjectField(TEXT("lane_info"), LaneInfoResult.Result);
				BestLane->SetNumberField(TEXT("distance"), FMath::Sqrt(DistanceSquared));
			}
		}

		if (!BestLane.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Unable to compute nearest ZoneGraph lane from overlapping lane geometry"));
		}

		TArray<TSharedPtr<FJsonValue>> NearestLanes;
		NearestLanes.Add(MakeShared<FJsonValueObject>(BestLane));
		QueryResult.Result->SetArrayField(TEXT("lanes"), NearestLanes);
		QueryResult.Result->SetObjectField(TEXT("nearest_lane"), BestLane);
		QueryResult.Result->SetNumberField(TEXT("count"), 1);
		QueryResult.Result->SetNumberField(TEXT("overlap_count"), OverlapCount);
		QueryResult.Result->SetNumberField(TEXT("inspected_lane_count"), InspectedLaneCount);
		return QueryResult;
	}
	return MakeUnavailable(TEXT("ai.find_nearest_zone_lane"), Params, TEXT("ai.query_zone_lanes is not registered because ZoneGraph support is unavailable in this build."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::FindOverlappingZoneLanes(const TSharedPtr<FJsonObject>& Params)
{
	if (FMonolithToolRegistry::Get().HasAction(TEXT("ai"), TEXT("query_zone_lanes")))
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("query_zone_lanes"), Params);
	}
	return MakeUnavailable(TEXT("ai.find_overlapping_zone_lanes"), Params, TEXT("ai.query_zone_lanes is not registered because ZoneGraph support is unavailable in this build."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::GetCrowdLaneState(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.get_crowd_lane_state"), Params, TEXT("MassCrowd lane-state access is plugin-version-specific and not wired yet."));
}

FMonolithActionResult FMonolithAIMassZoneGraphActions::SetCrowdLaneState(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailable(TEXT("ai.set_crowd_lane_state"), Params, TEXT("MassCrowd lane-state mutation is unavailable until read-first validation and PIE-only guards are implemented."));
}
