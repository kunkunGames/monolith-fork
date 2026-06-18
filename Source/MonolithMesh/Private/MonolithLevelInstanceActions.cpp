#include "MonolithLevelInstanceActions.h"

#include "MonolithMeshUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Misc/PackageName.h"

namespace
{
	int32 ClampLevelInstanceLimit(double Value)
	{
		return FMath::Clamp(static_cast<int32>(Value), 1, 500);
	}

	bool IsLevelInstanceLikeActor(const AActor* Actor)
	{
		if (!Actor || !Actor->GetClass())
		{
			return false;
		}

		return Actor->IsA(ALevelInstance::StaticClass())
			|| Actor->GetClass()->ImplementsInterface(ULevelInstanceInterface::StaticClass())
			|| Actor->GetClass()->GetName().Contains(TEXT("LevelInstance"), ESearchCase::IgnoreCase)
			|| Actor->GetClass()->GetClassPathName().ToString().Contains(TEXT("LevelInstance"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonObject> MakeCapabilityNote()
	{
		TSharedPtr<FJsonObject> Note = MakeShared<FJsonObject>();
		Note->SetBoolField(TEXT("edit_session_mutation_available"), false);
		Note->SetStringField(TEXT("reason"), TEXT("Monolith exposes safe inspection and create-from-selected workflows first. Direct nested edit-session takeover remains unavailable until editor-state tests cover conflict handling."));
		return Note;
	}

	TArray<TSharedPtr<FJsonValue>> LevelInstanceVectorToJson(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Value.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Arr;
	}

	TSharedPtr<FJsonObject> MakeLevelInstanceActorRow(AActor* Actor)
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
		Row->SetBoolField(TEXT("is_level_instance"), IsLevelInstanceLikeActor(Actor));
		Row->SetArrayField(TEXT("location"), LevelInstanceVectorToJson(Actor->GetActorLocation()));

		if (ULevel* Level = Actor->GetLevel())
		{
			Row->SetStringField(TEXT("outer_level"), Level->GetPathName());
			if (UPackage* Package = Level->GetOutermost())
			{
				Row->SetStringField(TEXT("outer_package"), Package->GetName());
				Row->SetBoolField(TEXT("outer_package_dirty"), Package->IsDirty());
			}
		}

		TArray<AActor*> Attached;
		Actor->GetAttachedActors(Attached, false, true);
		Row->SetNumberField(TEXT("attached_actor_count"), Attached.Num());
		return Row;
	}

	UWorld* GetEditorWorld()
	{
		return MonolithMeshUtils::GetEditorWorld();
	}

	AActor* ResolveLevelInstanceActor(const TSharedPtr<FJsonObject>& Params, FString& OutError)
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

		UWorld* World = GetEditorWorld();
		if (!World)
		{
			OutError = TEXT("No editor world available");
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !IsLevelInstanceLikeActor(Actor))
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

		OutError = FString::Printf(TEXT("Level Instance actor not found: %s"), *Query);
		return nullptr;
	}

	bool CopyActorNames(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>& OutActorNames, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorNames = nullptr;
		if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNames) || !ActorNames || ActorNames->Num() == 0)
		{
			OutError = TEXT("Missing or empty required param: actor_names");
			return false;
		}

		OutActorNames = *ActorNames;
		return true;
	}

	FMonolithActionResult PreviewOrCreatePrefab(const TSharedPtr<FJsonObject>& Params, const FString& TypeName)
	{
		TArray<TSharedPtr<FJsonValue>> ActorNames;
		FString Error;
		if (!CopyActorNames(Params, ActorNames, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		FString SavePath;
		if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));
		}

		bool bConfirm = false;
		Params->TryGetBoolField(TEXT("confirm"), bConfirm);
		if (!bConfirm)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("would_create"), true);
			Result->SetBoolField(TEXT("confirm_required"), true);
			Result->SetStringField(TEXT("type"), TypeName);
			Result->SetStringField(TEXT("save_path"), SavePath);
			Result->SetNumberField(TEXT("source_actor_count"), ActorNames.Num());
			Result->SetStringField(TEXT("next_step"), TEXT("Re-run with confirm=true to invoke mesh.create_prefab."));
			Result->SetObjectField(TEXT("edit_session"), MakeCapabilityNote());
			return FMonolithActionResult::Success(Result);
		}

		TSharedPtr<FJsonObject> ForwardParams = MakeShared<FJsonObject>();
		ForwardParams->SetArrayField(TEXT("actor_names"), ActorNames);
		ForwardParams->SetStringField(TEXT("save_path"), SavePath);
		ForwardParams->SetStringField(TEXT("type"), TypeName);
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_prefab"), ForwardParams);
	}

	FMonolithActionResult MakeLifecycleUnavailable(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("action"), Action);
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetObjectField(TEXT("edit_session"), MakeCapabilityNote());

		FString Error;
		if (AActor* Actor = ResolveLevelInstanceActor(Params, Error))
		{
			Result->SetObjectField(TEXT("level_instance"), MakeLevelInstanceActorRow(Actor));
		}
		else
		{
			Result->SetStringField(TEXT("resolution_warning"), Error);
		}
		return FMonolithActionResult::Success(Result);
	}
}

void FMonolithLevelInstanceActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("level_instance"), TEXT("list_level_instances"),
		TEXT("List Level Instance-like actors in the current editor world."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::ListLevelInstances),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("level_instance"), TEXT("get_level_instance"),
		TEXT("Inspect a Level Instance actor by label, name, or object path."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::GetLevelInstance),
		FParamSchemaBuilder()
			.Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or object path"), {TEXT("actor_name"), TEXT("actor_path")})
			.Build());

	Registry.RegisterAction(TEXT("level_instance"), TEXT("create_level_instance"),
		TEXT("Preview or create a Level Instance from actor_names. Requires confirm=true for mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::CreateLevelInstance),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to include"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Target level asset package path"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true to invoke creation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("level_instance"), TEXT("edit_level_instance"),
		TEXT("Report Level Instance edit-session availability without taking over editor-global state."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::EditLevelInstance),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("commit_level_instance"),
		TEXT("Report Level Instance commit availability and dirty-package context."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::CommitLevelInstance),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("discard_level_instance"),
		TEXT("Report Level Instance discard availability and dirty-package context."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::DiscardLevelInstance),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("load_level_instance"),
		TEXT("Report Level Instance load availability without forcing nested edit state."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::LoadLevelInstance),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("unload_level_instance"),
		TEXT("Report Level Instance unload availability without forcing nested edit state."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::UnloadLevelInstance),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());

	Registry.RegisterAction(TEXT("level_instance"), TEXT("list_child_instances"),
		TEXT("List attached child Level Instance-like actors for a parent Level Instance."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::ListChildInstances),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("list_instance_actors"),
		TEXT("List directly attached actors for a Level Instance actor."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::ListInstanceActors),
		FParamSchemaBuilder().Optional(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or path"), {TEXT("actor_name"), TEXT("actor_path")}).Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("move_actors_to_instance"),
		TEXT("Preview actor movement into a Level Instance; direct nested mutation is unavailable until conflict tests exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::MoveActorsToInstance),
		FParamSchemaBuilder()
			.Optional(TEXT("actor"), TEXT("string"), TEXT("Target Level Instance actor"), {TEXT("actor_name"), TEXT("actor_path")})
			.Optional(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to move"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future mutation"), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("create_packed_level_actor_blueprint"),
		TEXT("Preview or create a Packed Level Actor from actor_names. Requires confirm=true for mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::CreatePackedLevelActorBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to include"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Target package path"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true to invoke creation"), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("level_instance"), TEXT("pack_level_actor"),
		TEXT("Preview or create a Packed Level Actor using the Level Instance creation path. Requires confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelInstanceActions::PackLevelActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to include"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Target package path"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true to invoke creation"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithLevelInstanceActions::ListLevelInstances(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampLevelInstanceLimit(LimitValue);

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsLevelInstanceLikeActor(Actor))
		{
			continue;
		}

		++MatchedCount;
		if (Rows.Num() < Limit)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeLevelInstanceActorRow(Actor)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("level_instances"), Rows);
	Result->SetObjectField(TEXT("edit_session"), MakeCapabilityNote());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelInstanceActions::GetLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	AActor* Actor = ResolveLevelInstanceActor(Params, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeLevelInstanceActorRow(Actor);
	Result->SetObjectField(TEXT("edit_session"), MakeCapabilityNote());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelInstanceActions::CreateLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return PreviewOrCreatePrefab(Params, TEXT("LevelInstance"));
}

FMonolithActionResult FMonolithLevelInstanceActions::EditLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return MakeLifecycleUnavailable(TEXT("level_instance.edit_level_instance"), Params);
}

FMonolithActionResult FMonolithLevelInstanceActions::CommitLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return MakeLifecycleUnavailable(TEXT("level_instance.commit_level_instance"), Params);
}

FMonolithActionResult FMonolithLevelInstanceActions::DiscardLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return MakeLifecycleUnavailable(TEXT("level_instance.discard_level_instance"), Params);
}

FMonolithActionResult FMonolithLevelInstanceActions::LoadLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return MakeLifecycleUnavailable(TEXT("level_instance.load_level_instance"), Params);
}

FMonolithActionResult FMonolithLevelInstanceActions::UnloadLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
	return MakeLifecycleUnavailable(TEXT("level_instance.unload_level_instance"), Params);
}

FMonolithActionResult FMonolithLevelInstanceActions::ListChildInstances(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	AActor* Actor = ResolveLevelInstanceActor(Params, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<AActor*> Attached;
	Actor->GetAttachedActors(Attached, false, true);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Attached.Num());
	for (AActor* Child : Attached)
	{
		if (IsLevelInstanceLikeActor(Child))
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeLevelInstanceActorRow(Child)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("parent"), MakeLevelInstanceActorRow(Actor));
	Result->SetNumberField(TEXT("child_instance_count"), Rows.Num());
	Result->SetArrayField(TEXT("child_instances"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelInstanceActions::ListInstanceActors(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	AActor* Actor = ResolveLevelInstanceActor(Params, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<AActor*> Attached;
	Actor->GetAttachedActors(Attached, false, true);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Attached.Num());
	for (AActor* Child : Attached)
	{
		Rows.Add(MakeShared<FJsonValueObject>(MakeLevelInstanceActorRow(Child)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("level_instance"), MakeLevelInstanceActorRow(Actor));
	Result->SetNumberField(TEXT("actor_count"), Rows.Num());
	Result->SetArrayField(TEXT("actors"), Rows);
	Result->SetStringField(TEXT("scope_note"), TEXT("Reports directly attached loaded actors. Nested edit-session contents remain guarded until explicit editor-state support is added."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelInstanceActions::MoveActorsToInstance(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("preview_only"));
	Result->SetBoolField(TEXT("confirm_supported"), false);
	Result->SetStringField(TEXT("reason"), TEXT("Moving actors into an existing Level Instance requires nested edit-session ownership and conflict tests. Use level_instance.create_level_instance for safe create-from-selected workflows."));

	FString Error;
	if (AActor* Actor = ResolveLevelInstanceActor(Params, Error))
	{
		Result->SetObjectField(TEXT("target_level_instance"), MakeLevelInstanceActorRow(Actor));
	}
	else
	{
		Result->SetStringField(TEXT("target_warning"), Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorNames = nullptr;
	if (Params->TryGetArrayField(TEXT("actor_names"), ActorNames) && ActorNames)
	{
		Result->SetNumberField(TEXT("candidate_actor_count"), ActorNames->Num());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelInstanceActions::CreatePackedLevelActorBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	return PreviewOrCreatePrefab(Params, TEXT("PackedLevelActor"));
}

FMonolithActionResult FMonolithLevelInstanceActions::PackLevelActor(const TSharedPtr<FJsonObject>& Params)
{
	return PreviewOrCreatePrefab(Params, TEXT("PackedLevelActor"));
}
