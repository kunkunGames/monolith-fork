#include "MonolithWaterActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Modules/ModuleManager.h"
#include "MonolithParamSchema.h"

namespace MonolithWater
{
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	int32 ClampWaterLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	bool IsWaterLikeClass(const UClass* Class)
	{
		if (!Class)
		{
			return false;
		}

		const FString ClassName = Class->GetName();
		const FString ClassPath = Class->GetClassPathName().ToString();
		return ClassPath.Contains(TEXT("/Script/Water"))
			|| ClassName.StartsWith(TEXT("WaterBody"))
			|| ClassName.StartsWith(TEXT("WaterZone"))
			|| ClassName.Contains(TEXT("WaterSpline"))
			|| ClassName.Contains(TEXT("Buoyancy"));
	}

	bool IsWaterLikeActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		if (IsWaterLikeClass(Actor->GetClass()))
		{
			return true;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (const UActorComponent* Component : Components)
		{
			if (Component && IsWaterLikeClass(Component->GetClass()))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeModuleStatus(const TCHAR* ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		auto Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("name"), ModuleName);
		Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
		Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
		return Status;
	}
}

void FMonolithWaterActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("water"), TEXT("get_status"),
		TEXT("Report Water/Landscape module availability and reflected Water-like actor counts. Read-only; no Water or Landscape hard dependency."),
		FMonolithActionHandler::CreateStatic(&FMonolithWaterActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("water"), TEXT("list_bodies"),
		TEXT("List Water-like actors/components in the current editor world using reflected class names only. Does not mutate actors, splines, landscapes, or zones."),
		FMonolithActionHandler::CreateStatic(&FMonolithWaterActions::ListBodies),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum returned rows, clamped to 1..500. Default: 100."))
			.Optional(TEXT("actor_name_filter"), TEXT("string"), TEXT("Optional case-insensitive substring filter on actor label/name."))
			.Build());
}

FMonolithActionResult FMonolithWaterActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("water"));
	Result->SetStringField(TEXT("domain"), TEXT("water_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);
	Result->SetBoolField(TEXT("editor_world_available"), World != nullptr);
	if (World)
	{
		Result->SetStringField(TEXT("world_name"), World->GetName());
		Result->SetStringField(TEXT("world_path"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	}

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Reserve(4);
	Modules.Add(MakeShared<FJsonValueObject>(MonolithWater::MakeModuleStatus(TEXT("Water"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithWater::MakeModuleStatus(TEXT("WaterEditor"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithWater::MakeModuleStatus(TEXT("Landscape"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithWater::MakeModuleStatus(TEXT("LandscapeEditor"))));
	Result->SetArrayField(TEXT("modules"), Modules);

	int32 WaterLikeActorCount = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (MonolithWater::IsWaterLikeActor(*It))
			{
				WaterLikeActorCount++;
			}
		}
	}
	Result->SetNumberField(TEXT("water_like_actor_count"), WaterLikeActorCount);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Reserve(2);
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("water.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("water.list_bodies")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Reserve(4);
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("water.query_surface")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("water.spawn_body")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("water.configure_body")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("water.rebuild_zone")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Reserve(2);
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This first milestone uses reflected class names only; it does not add Water, WaterEditor, Landscape, or LandscapeEditor link dependencies.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Actor, spline, zone, landscape, buoyancy, and rebuild mutations remain future work for the water namespace.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWaterActions::ListBodies(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("Editor world not available"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithWater::ClampWaterLimit(LimitValue);

	FString NameFilter;
	Params->TryGetStringField(TEXT("actor_name_filter"), NameFilter);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!MonolithWater::IsWaterLikeActor(Actor))
		{
			continue;
		}

		const FString ActorLabel = Actor->GetActorLabel();
		const FString ActorName = Actor->GetFName().ToString();
		if (!NameFilter.IsEmpty()
			&& !ActorLabel.Contains(NameFilter, ESearchCase::IgnoreCase)
			&& !ActorName.Contains(NameFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		MatchedCount++;
		const FString ActorClassName = Actor->GetClass()->GetName();
		ClassCounts.FindOrAdd(ActorClassName)++;

		if (Rows.Num() >= Limit)
		{
			continue;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		TArray<TSharedPtr<FJsonValue>> WaterComponents;
		WaterComponents.Reserve(Components.Num());
		for (const UActorComponent* Component : Components)
		{
			if (!Component || !MonolithWater::IsWaterLikeClass(Component->GetClass()))
			{
				continue;
			}

			auto ComponentJson = MakeShared<FJsonObject>();
			ComponentJson->SetStringField(TEXT("name"), Component->GetName());
			ComponentJson->SetStringField(TEXT("class_name"), Component->GetClass()->GetName());
			ComponentJson->SetStringField(TEXT("class_path"), Component->GetClass()->GetClassPathName().ToString());
			WaterComponents.Add(MakeShared<FJsonValueObject>(ComponentJson));
		}

		FVector Origin = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		Actor->GetActorBounds(false, Origin, Extent);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("actor_name"), ActorName);
		Row->SetStringField(TEXT("actor_label"), ActorLabel);
		Row->SetStringField(TEXT("actor_class"), ActorClassName);
		Row->SetStringField(TEXT("actor_class_path"), Actor->GetClass()->GetClassPathName().ToString());
		Row->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
		Row->SetArrayField(TEXT("location"), MonolithWater::VectorToJsonArray(Actor->GetActorLocation()));
		Row->SetArrayField(TEXT("bounds_origin"), MonolithWater::VectorToJsonArray(Origin));
		Row->SetArrayField(TEXT("bounds_extent"), MonolithWater::VectorToJsonArray(Extent));
		Row->SetNumberField(TEXT("water_component_count"), WaterComponents.Num());
		Row->SetArrayField(TEXT("water_components"), WaterComponents);

		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Row->SetArrayField(TEXT("tags"), Tags);

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("water"));
	Result->SetStringField(TEXT("domain"), TEXT("water_discovery"));
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	if (!NameFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("actor_name_filter"), NameFilter);
	}
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("water_bodies"), Rows);
	return FMonolithActionResult::Success(Result);
}
