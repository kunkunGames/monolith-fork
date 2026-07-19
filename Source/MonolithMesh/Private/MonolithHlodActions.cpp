#include "MonolithHlodActions.h"

#include "MonolithMeshUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	int32 ClampHlodLimit(double Value)
	{
		return FMath::Clamp(static_cast<int32>(Value), 1, 1000);
	}

	bool IsHlodLayerAsset(const FAssetData& Asset)
	{
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = Asset.AssetClassPath.ToString();
		return ClassName.Equals(TEXT("HLODLayer"), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("HLODLayer"), ESearchCase::IgnoreCase)
			|| ClassPath.Contains(TEXT("HLODLayer"), ESearchCase::IgnoreCase);
	}

	bool IsHlodActor(const AActor* Actor)
	{
		if (!Actor || !Actor->GetClass())
		{
			return false;
		}

		const FString ClassName = Actor->GetClass()->GetName();
		const FString ClassPath = Actor->GetClass()->GetClassPathName().ToString();
		return ClassName.Contains(TEXT("HLOD"), ESearchCase::IgnoreCase)
			|| ClassPath.Contains(TEXT("HLOD"), ESearchCase::IgnoreCase);
	}

	TArray<TSharedPtr<FJsonValue>> HlodVectorToJson(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(Value.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Arr;
	}

	TSharedPtr<FJsonObject> MakeAssetRow(const FAssetData& Asset)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), Asset.IsAssetLoaded());
		return Row;
	}

	TSharedPtr<FJsonObject> MakeHlodActorRow(AActor* Actor)
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
		Row->SetArrayField(TEXT("location"), HlodVectorToJson(Actor->GetActorLocation()));
		Row->SetArrayField(TEXT("bounds_origin"), HlodVectorToJson(Actor->GetComponentsBoundingBox(true).GetCenter()));
		Row->SetArrayField(TEXT("bounds_extent"), HlodVectorToJson(Actor->GetComponentsBoundingBox(true).GetExtent()));
		return Row;
	}

	TArray<FAssetData> GetHlodLayerAssets(const FString& PackagePath)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*PackagePath));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		Assets.RemoveAll([](const FAssetData& Asset)
		{
			return !IsHlodLayerAsset(Asset);
		});
		return Assets;
	}

	FMonolithActionResult ExecuteSetupHlod(const TSharedPtr<FJsonObject>& Params)
	{
		if (!FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("setup_hlod")))
		{
			return FMonolithActionResult::Error(TEXT("mesh.setup_hlod is not registered"));
		}

		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("setup_hlod"), Params);
	}

	FMonolithActionResult MakeBuildUnavailable(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		bool bConfirm = false;
		Params->TryGetBoolField(TEXT("confirm"), bConfirm);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("action"), Action);
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetBoolField(TEXT("confirm_received"), bConfirm);
		Result->SetStringField(TEXT("reason"), TEXT("Monolith currently exposes HLOD inspection and layer setup. Long-running World Partition or legacy HLOD build/clear operations are not invoked from MCP until progress and cancellation coverage is added."));
		return FMonolithActionResult::Success(Result);
	}

	void AddReflectedProperties(UObject* Object, TSharedPtr<FJsonObject> Row)
	{
		if (!Object)
		{
			return;
		}

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It && Count < 64; ++It)
		{
			FString Value;
			It->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
			Properties->SetStringField(It->GetName(), Value);
			++Count;
		}
		Row->SetObjectField(TEXT("properties"), Properties);
		Row->SetNumberField(TEXT("property_count_returned"), Count);
	}
}

void FMonolithHlodActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("hlod"), TEXT("list_hlod_layers"),
		TEXT("List HLODLayer assets under a package path."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ListHlodLayers),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows"), TEXT("100"))
			.Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("get_hlod_layer"),
		TEXT("Inspect a HLODLayer asset using AssetRegistry and reflection."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::GetHlodLayer),
		FParamSchemaBuilder().Required(TEXT("asset_path"), TEXT("string"), TEXT("HLODLayer asset object or package path")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("create_hlod_layer"),
		TEXT("Create/configure a HLOD layer through mesh.setup_hlod."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::CreateHlodLayer),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Target HLODLayer asset path"))
			.Optional(TEXT("layer_type"), TEXT("string"), TEXT("MeshMerge, MeshSimplify, MeshApproximate, or Custom"), TEXT("MeshSimplify"))
			.Optional(TEXT("cell_size"), TEXT("integer"), TEXT("HLOD cell size"), TEXT("25600"))
			.Optional(TEXT("loading_range"), TEXT("number"), TEXT("Loading range multiplier"), TEXT("2.0"))
			.Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("configure_hlod_layer"),
		TEXT("Configure a HLOD layer through mesh.setup_hlod."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ConfigureHlodLayer),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Target HLODLayer asset path"))
			.Optional(TEXT("layer_type"), TEXT("string"), TEXT("MeshMerge, MeshSimplify, MeshApproximate, or Custom"), TEXT("MeshSimplify"))
			.Optional(TEXT("cell_size"), TEXT("integer"), TEXT("HLOD cell size"), TEXT("25600"))
			.Optional(TEXT("loading_range"), TEXT("number"), TEXT("Loading range multiplier"), TEXT("2.0"))
			.Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("list_hlod_source_actors"),
		TEXT("List loaded static mesh actors that are eligible source candidates for HLOD reports."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ListHlodSourceActors),
		FParamSchemaBuilder().Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows"), TEXT("250")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("list_hlod_actors"),
		TEXT("List loaded HLOD-like actors in the editor world."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ListHlodActors),
		FParamSchemaBuilder().Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows"), TEXT("250")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("get_hlod_stats"),
		TEXT("Return HLOD layer and loaded actor counts for the current editor world."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::GetHlodStats),
		FParamSchemaBuilder().Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path"), TEXT("/Game")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("check_hlod_hash"),
		TEXT("Compute a lightweight HLOD readiness hash from layer and actor identity rows."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::CheckHlodHash),
		FParamSchemaBuilder().Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path"), TEXT("/Game")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("build_hlod"),
		TEXT("Report HLOD build orchestration status. Does not launch long-running builds yet."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::BuildHlod),
		FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future build execution"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("clear_legacy_hlod"),
		TEXT("Report legacy HLOD clear orchestration status. Does not clear generated actors yet."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ClearLegacyHlod),
		FParamSchemaBuilder().Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for future clear execution"), TEXT("false")).Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("legacy_hlod_needs_build"),
		TEXT("Return a conservative legacy-HLOD needs-build signal based on loaded HLOD actor presence."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::LegacyHlodNeedsBuild),
		FParamSchemaBuilder().Build());
	Registry.RegisterAction(TEXT("hlod"), TEXT("export_hlod"),
		TEXT("Export a bounded HLOD inspection report JSON file under Saved/Monolith/HLOD."),
		FMonolithActionHandler::CreateStatic(&FMonolithHlodActions::ExportHlod),
		FParamSchemaBuilder().Optional(TEXT("output_path"), TEXT("string"), TEXT("Optional absolute output JSON path")).Build());
}

FMonolithActionResult FMonolithHlodActions::ListHlodLayers(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampHlodLimit(LimitValue);

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FAssetData> Assets = GetHlodLayerAssets(PackagePath);
	for (const FAssetData& Asset : Assets)
	{
		if (Rows.Num() >= Limit)
		{
			break;
		}
		Rows.Add(MakeShared<FJsonValueObject>(MakeAssetRow(Asset)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), Assets.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), Assets.Num() > Rows.Num());
	Result->SetArrayField(TEXT("layers"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::GetHlodLayer(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));
	}

	if (!AssetPath.Contains(TEXT(".")))
	{
		const FString Name = FPackageName::GetShortName(AssetPath);
		AssetPath = AssetPath + TEXT(".") + Name;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData Asset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!Asset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("HLOD layer asset not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeAssetRow(Asset);
	if (UObject* Object = Asset.GetAsset())
	{
		AddReflectedProperties(Object, Result);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::CreateHlodLayer(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteSetupHlod(Params);
}

FMonolithActionResult FMonolithHlodActions::ConfigureHlodLayer(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteSetupHlod(Params);
}

FMonolithActionResult FMonolithHlodActions::ListHlodSourceActors(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 250.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampHlodLimit(LimitValue);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		++MatchedCount;
		if (Rows.Num() < Limit)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeHlodActorRow(Actor)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("source_actors"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::ListHlodActors(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 250.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampHlodLimit(LimitValue);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsHlodActor(Actor))
		{
			continue;
		}
		++MatchedCount;
		if (Rows.Num() < Limit)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeHlodActorRow(Actor)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("hlod_actors"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::GetHlodStats(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("layer_asset_count"), GetHlodLayerAssets(PackagePath).Num());

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	Result->SetStringField(TEXT("world"), World ? World->GetPathName() : TEXT(""));
	int32 HlodActorCount = 0;
	int32 SourceActorCount = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsHlodActor(*It))
			{
				++HlodActorCount;
			}
		}
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			++SourceActorCount;
		}
	}
	Result->SetNumberField(TEXT("hlod_actor_count"), HlodActorCount);
	Result->SetNumberField(TEXT("source_actor_candidate_count"), SourceActorCount);
	Result->SetStringField(TEXT("build_status"), TEXT("inspection_only"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::CheckHlodHash(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	uint32 Hash = 0;
	for (const FAssetData& Asset : GetHlodLayerAssets(PackagePath))
	{
		Hash = HashCombine(Hash, GetTypeHash(Asset.GetObjectPathString()));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsHlodActor(*It))
			{
				Hash = HashCombine(Hash, GetTypeHash((*It)->GetPathName()));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("hash"), FString::Printf(TEXT("%08x"), Hash));
	Result->SetStringField(TEXT("scope"), PackagePath);
	Result->SetBoolField(TEXT("authoritative_engine_hash"), false);
	Result->SetStringField(TEXT("note"), TEXT("Lightweight readiness hash based on discovered HLOD layer assets and loaded HLOD-like actors."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::BuildHlod(const TSharedPtr<FJsonObject>& Params)
{
	return MakeBuildUnavailable(TEXT("hlod.build_hlod"), Params);
}

FMonolithActionResult FMonolithHlodActions::ClearLegacyHlod(const TSharedPtr<FJsonObject>& Params)
{
	return MakeBuildUnavailable(TEXT("hlod.clear_legacy_hlod"), Params);
}

FMonolithActionResult FMonolithHlodActions::LegacyHlodNeedsBuild(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	int32 HlodActorCount = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsHlodActor(*It))
			{
				++HlodActorCount;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("inspection_only"));
	Result->SetBoolField(TEXT("needs_build"), HlodActorCount == 0);
	Result->SetNumberField(TEXT("loaded_hlod_actor_count"), HlodActorCount);
	Result->SetStringField(TEXT("reason"), TEXT("No generated HLOD-like actors were detected in loaded editor levels. This is a conservative signal, not an engine build-state query."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithHlodActions::ExportHlod(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToIso8601());
	Report->SetStringField(TEXT("kind"), TEXT("monolith_hlod_report"));
	Report->SetObjectField(TEXT("stats"), GetHlodStats(MakeShared<FJsonObject>()).Result);

	FMonolithActionResult LayerResult = ListHlodLayers(MakeShared<FJsonObject>());
	if (LayerResult.bSuccess && LayerResult.Result.IsValid())
	{
		Report->SetObjectField(TEXT("layers"), LayerResult.Result);
	}
	FMonolithActionResult ActorResult = ListHlodActors(MakeShared<FJsonObject>());
	if (ActorResult.bSuccess && ActorResult.Result.IsValid())
	{
		Report->SetObjectField(TEXT("actors"), ActorResult.Result);
	}

	FString OutputPath;
	Params->TryGetStringField(TEXT("output_path"), OutputPath);
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::ProjectSavedDir() / TEXT("Monolith/HLOD/hlod_report_") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT(".json");
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize HLOD report"));
	}
	if (!FFileHelper::SaveStringToFile(Json, *OutputPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write HLOD report: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("output_path"), OutputPath);
	Result->SetStringField(TEXT("format"), TEXT("json"));
	Result->SetNumberField(TEXT("byte_count"), Json.Len());
	return FMonolithActionResult::Success(Result);
}
