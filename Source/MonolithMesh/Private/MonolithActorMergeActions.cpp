#include "MonolithActorMergeActions.h"

#include "MonolithMeshUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(Value.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Arr;
	}

	bool ResolveActorNames(const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutActorNameValues, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorNames = nullptr;
		if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNames) || !ActorNames || ActorNames->Num() == 0)
		{
			OutError = TEXT("Missing or empty required param: actor_names");
			return false;
		}

		OutActorNameValues = *ActorNames;
		OutActors.Reserve(ActorNames->Num());
		for (const TSharedPtr<FJsonValue>& Value : *ActorNames)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty())
			{
				OutError = TEXT("actor_names entries must be non-empty strings");
				return false;
			}

			FString FindError;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Name, FindError);
			if (!Actor)
			{
				OutError = FindError;
				return false;
			}
			OutActors.Add(Actor);
		}

		return true;
	}

	TSharedPtr<FJsonObject> BuildPreview(const TArray<AActor*>& Actors)
	{
		FBox Bounds(ForceInit);
		int32 StaticMeshComponentCount = 0;
		int32 MaterialSlotCount = 0;
		int64 TriangleCount = 0;
		TSet<FString> Materials;
		TArray<TSharedPtr<FJsonValue>> ActorRows;
		ActorRows.Reserve(Actors.Num());

		for (AActor* Actor : Actors)
		{
			if (!Actor)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ActorRow = MakeShared<FJsonObject>();
			ActorRow->SetStringField(TEXT("name"), Actor->GetFName().ToString());
			ActorRow->SetStringField(TEXT("label"), Actor->GetActorLabel());
			ActorRow->SetStringField(TEXT("path"), Actor->GetPathName());
			ActorRow->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : TEXT(""));

			int32 ActorComponentCount = 0;
			int32 ActorMaterialSlots = 0;
			int64 ActorTriangles = 0;
			TArray<UStaticMeshComponent*> Components;
			Actor->GetComponents<UStaticMeshComponent>(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}

				++ActorComponentCount;
				++StaticMeshComponentCount;
				Bounds += Component->Bounds.GetBox();

				UStaticMesh* Mesh = Component->GetStaticMesh();
				if (Mesh && Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
				{
					const int32 Tris = Mesh->GetRenderData()->LODResources[0].GetNumTriangles();
					ActorTriangles += Tris;
					TriangleCount += Tris;
				}

				const int32 Slots = Component->GetNumMaterials();
				ActorMaterialSlots += Slots;
				MaterialSlotCount += Slots;
				for (int32 Index = 0; Index < Slots; ++Index)
				{
					if (UMaterialInterface* Mat = Component->GetMaterial(Index))
					{
						Materials.Add(Mat->GetPathName());
					}
				}
			}

			ActorRow->SetNumberField(TEXT("static_mesh_component_count"), ActorComponentCount);
			ActorRow->SetNumberField(TEXT("material_slot_count"), ActorMaterialSlots);
			ActorRow->SetNumberField(TEXT("triangle_count_lod0"), static_cast<double>(ActorTriangles));
			ActorRows.Add(MakeShared<FJsonValueObject>(ActorRow));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("actor_count"), Actors.Num());
		Result->SetNumberField(TEXT("static_mesh_component_count"), StaticMeshComponentCount);
		Result->SetNumberField(TEXT("material_slot_count"), MaterialSlotCount);
		Result->SetNumberField(TEXT("unique_material_count"), Materials.Num());
		Result->SetNumberField(TEXT("triangle_count_lod0"), static_cast<double>(TriangleCount));
		Result->SetArrayField(TEXT("actors"), ActorRows);
		if (Bounds.IsValid)
		{
			Result->SetArrayField(TEXT("bounds_origin"), VectorToJson(Bounds.GetCenter()));
			Result->SetArrayField(TEXT("bounds_extent"), VectorToJson(Bounds.GetExtent()));
		}

		TArray<TSharedPtr<FJsonValue>> MaterialRows;
		MaterialRows.Reserve(Materials.Num());
		for (const FString& Material : Materials)
		{
			MaterialRows.Add(MakeShared<FJsonValueString>(Material));
		}
		Result->SetArrayField(TEXT("materials"), MaterialRows);
		return Result;
	}

	FMonolithActionResult PreviewFromParams(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>* OutActorNames = nullptr)
	{
		TArray<AActor*> Actors;
		TArray<TSharedPtr<FJsonValue>> ActorNameValues;
		FString Error;
		if (!ResolveActorNames(Params, Actors, ActorNameValues, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (OutActorNames)
		{
			*OutActorNames = ActorNameValues;
		}

		TSharedPtr<FJsonObject> Result = BuildPreview(Actors);
		FString SavePath;
		if (Params->TryGetStringField(TEXT("save_path"), SavePath))
		{
			Result->SetStringField(TEXT("save_path"), SavePath);
		}
		Result->SetStringField(TEXT("default_source_policy"), TEXT("keep_sources"));
		Result->SetBoolField(TEXT("source_mutation_requires_confirm"), true);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult ExecuteProxyMesh(const TSharedPtr<FJsonObject>& Params, const FString& BoundaryAction)
	{
		TArray<TSharedPtr<FJsonValue>> ActorNames;
		FMonolithActionResult Preview = PreviewFromParams(Params, &ActorNames);
		if (!Preview.bSuccess)
		{
			return Preview;
		}

		bool bConfirm = false;
		Params->TryGetBoolField(TEXT("confirm"), bConfirm);
		if (!bConfirm)
		{
			Preview.Result->SetBoolField(TEXT("would_generate_proxy_mesh"), true);
			Preview.Result->SetBoolField(TEXT("confirm_required"), true);
			Preview.Result->SetStringField(TEXT("boundary_action"), BoundaryAction);
			return Preview;
		}

		FString SavePath;
		if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param for confirmed merge: save_path"));
		}

		FString SourcePolicy = TEXT("keep");
		Params->TryGetStringField(TEXT("source_policy"), SourcePolicy);
		if (!SourcePolicy.Equals(TEXT("keep"), ESearchCase::IgnoreCase)
			&& !SourcePolicy.Equals(TEXT("keep_sources"), ESearchCase::IgnoreCase)
			&& !SourcePolicy.Equals(TEXT("side_by_side"), ESearchCase::IgnoreCase))
		{
			return FMonolithActionResult::Error(TEXT("Confirmed proxy creation currently supports only keep_sources/side_by_side source policy. Replacement or deletion remains unavailable."));
		}

		TSharedPtr<FJsonObject> ForwardParams = MakeShared<FJsonObject>();
		ForwardParams->SetArrayField(TEXT("actor_names"), ActorNames);
		ForwardParams->SetStringField(TEXT("save_path"), SavePath);

		bool bMergeMaterials = true;
		Params->TryGetBoolField(TEXT("merge_materials"), bMergeMaterials);
		ForwardParams->SetBoolField(TEXT("merge_materials"), bMergeMaterials);

		double TextureSize = 1024.0;
		Params->TryGetNumberField(TEXT("texture_size"), TextureSize);
		ForwardParams->SetNumberField(TEXT("texture_size"), TextureSize);

		double ScreenSize = 300.0;
		Params->TryGetNumberField(TEXT("screen_size"), ScreenSize);
		ForwardParams->SetNumberField(TEXT("screen_size"), ScreenSize);

		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("generate_proxy_mesh"), ForwardParams);
	}

	FMonolithActionResult MakeUnavailableWithPreview(const TSharedPtr<FJsonObject>& Params, const FString& Action, const FString& Reason)
	{
		FMonolithActionResult Preview = PreviewFromParams(Params);
		if (!Preview.bSuccess)
		{
			return Preview;
		}
		Preview.Result->SetStringField(TEXT("action"), Action);
		Preview.Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Preview.Result->SetStringField(TEXT("reason"), Reason);
		return Preview;
	}
}

void FMonolithActorMergeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("preview_actor_merge"),
		TEXT("Resolve actors and estimate bounds, materials, and LOD0 triangles before actor merge/proxy operations."),
		FMonolithActionHandler::CreateStatic(&FMonolithActorMergeActions::PreviewActorMerge),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to include"))
			.Optional(TEXT("save_path"), TEXT("string"), TEXT("Candidate output StaticMesh path"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("merge_actors"),
		TEXT("Preview or create a side-by-side proxy mesh from actors. Requires confirm=true and keep_sources policy."),
		FMonolithActionHandler::CreateStatic(&FMonolithActorMergeActions::MergeActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to merge"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Output StaticMesh asset path"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for generation"), TEXT("false"))
			.Optional(TEXT("source_policy"), TEXT("string"), TEXT("keep_sources/side_by_side only for now"), TEXT("keep_sources"))
			.Optional(TEXT("merge_materials"), TEXT("boolean"), TEXT("Merge materials into atlas"), TEXT("true"))
			.Optional(TEXT("texture_size"), TEXT("integer"), TEXT("Merged material texture size"), TEXT("1024"))
			.Optional(TEXT("screen_size"), TEXT("integer"), TEXT("Proxy screen size"), TEXT("300"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_proxy_mesh_from_actors"),
		TEXT("Preview or invoke mesh.generate_proxy_mesh from explicit actor_names."),
		FMonolithActionHandler::CreateStatic(&FMonolithActorMergeActions::CreateProxyMeshFromActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to merge"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Output StaticMesh asset path"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for generation"), TEXT("false"))
			.Optional(TEXT("merge_materials"), TEXT("boolean"), TEXT("Merge materials into atlas"), TEXT("true"))
			.Optional(TEXT("texture_size"), TEXT("integer"), TEXT("Merged material texture size"), TEXT("1024"))
			.Optional(TEXT("screen_size"), TEXT("integer"), TEXT("Proxy screen size"), TEXT("300"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("merge_actors_to_instances"),
		TEXT("Preview actor candidates for future instancing conversion; does not mutate scene structure yet."),
		FMonolithActionHandler::CreateStatic(&FMonolithActorMergeActions::MergeActorsToInstances),
		FParamSchemaBuilder().Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to inspect")).Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("bake_actor_materials"),
		TEXT("Preview material bake inputs for actors; dedicated bake output generation is unavailable until bake tests exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithActorMergeActions::BakeActorMaterials),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor labels/names to inspect"))
			.Optional(TEXT("texture_size"), TEXT("integer"), TEXT("Requested bake texture size"), TEXT("1024"))
			.Build());
}

FMonolithActionResult FMonolithActorMergeActions::PreviewActorMerge(const TSharedPtr<FJsonObject>& Params)
{
	return PreviewFromParams(Params);
}

FMonolithActionResult FMonolithActorMergeActions::MergeActors(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteProxyMesh(Params, TEXT("mesh.merge_actors"));
}

FMonolithActionResult FMonolithActorMergeActions::CreateProxyMeshFromActors(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteProxyMesh(Params, TEXT("mesh.create_proxy_mesh_from_actors"));
}

FMonolithActionResult FMonolithActorMergeActions::MergeActorsToInstances(const TSharedPtr<FJsonObject>& Params)
{
	return MakeUnavailableWithPreview(Params, TEXT("mesh.merge_actors_to_instances"),
		TEXT("Instancing conversion can replace scene actors and requires component-level equivalence checks. Monolith currently returns a preview only."));
}

FMonolithActionResult FMonolithActorMergeActions::BakeActorMaterials(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithActionResult Preview = MakeUnavailableWithPreview(Params, TEXT("mesh.bake_actor_materials"),
		TEXT("Dedicated material bake output generation remains unavailable; use mesh.generate_proxy_mesh with merge_materials=true for the current proxy workflow."));
	if (Preview.bSuccess && Preview.Result.IsValid())
	{
		double TextureSize = 1024.0;
		Params->TryGetNumberField(TEXT("texture_size"), TextureSize);
		Preview.Result->SetNumberField(TEXT("requested_texture_size"), TextureSize);
	}
	return Preview;
}
