#include "MonolithMeshQualityActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ActorComponent.h"
#include "Components/LightComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "IMeshMergeUtilities.h"
#include "MeshMergeModule.h"
#include "MeshMerge/MeshMergingSettings.h"
#include "MonolithPackagePathValidator.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "UObject/SavePackage.h"

// ============================================================================
// Helpers
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FMonolithMeshQualityActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(3);
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshQualityActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_proxy_mesh"),
		TEXT("Merge selected static mesh actors into a single simplified proxy mesh. Uses IMeshMergeUtilities for LOD-aware merging with optional material merging."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::GenerateProxyMesh),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names to merge"))
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for saved mesh (e.g. /Game/Merged/MyProxy)"))
			.Optional(TEXT("screen_size"), TEXT("integer"), TEXT("Screen size for proxy (pixels)"), TEXT("300"))
			.Optional(TEXT("merge_materials"), TEXT("boolean"), TEXT("Merge materials into atlas"), TEXT("true"))
			.Optional(TEXT("texture_size"), TEXT("integer"), TEXT("Merged material texture size"), TEXT("1024"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("setup_hlod"),
		TEXT("Create or configure a UHLODLayer asset with type and settings."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::SetupHlod),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for HLOD layer (e.g. /Game/HLOD/MyLayer)"))
			.Optional(TEXT("layer_type"), TEXT("string"), TEXT("HLOD type: MeshMerge, MeshSimplify, MeshApproximate, Custom"), TEXT("MeshSimplify"))
			.Optional(TEXT("cell_size"), TEXT("integer"), TEXT("HLOD cell size in world units"), TEXT("25600"))
			.Optional(TEXT("loading_range"), TEXT("number"), TEXT("Loading range multiplier"), TEXT("2.0"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_texture_budget"),
		TEXT("Analyze texture memory usage: pool size, used, top textures, by-format breakdown. Identifies budget hogs and gives recommendations."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::AnalyzeTextureBudget),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("scan_path"), TEXT("Content path filter (empty = all)"))
			.Optional(TEXT("top_count"), TEXT("integer"), TEXT("Number of top textures to return"), TEXT("20"))
			.Build());
}

// ============================================================================
// 3. generate_proxy_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::GenerateProxyMesh(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorNamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesArr) || ActorNamesArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Required: actor_names (array of at least 2 actor names to merge)"));
	}

	if (ActorNamesArr->Num() > 100)
	{
		return FMonolithActionResult::Error(TEXT("Too many actors to merge (max 100)"));
	}

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: save_path (asset path for saved proxy mesh)"));
	}

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	double ScreenSizeD = 300.0;
	if (Params->HasField(TEXT("screen_size")) && !Params->TryGetNumberField(TEXT("screen_size"), ScreenSizeD))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'screen_size'. Expected number."));
	}
	int32 ScreenSize = FMath::Clamp(static_cast<int32>(ScreenSizeD), 50, 4096);

	bool bMergeMaterials = true;
	if (Params->HasField(TEXT("merge_materials")) && !Params->TryGetBoolField(TEXT("merge_materials"), bMergeMaterials))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'merge_materials'. Expected boolean."));
	}

	double TextureSizeD = 1024.0;
	if (Params->HasField(TEXT("texture_size")) && !Params->TryGetNumberField(TEXT("texture_size"), TextureSizeD))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'texture_size'. Expected number."));
	}
	int32 TextureSize = FMath::Clamp(static_cast<int32>(TextureSizeD), 64, 4096);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Resolve actor names to components (API takes UPrimitiveComponent*)
	TArray<UPrimitiveComponent*> Components;
	TArray<FString> ResolvedNames;

	for (const TSharedPtr<FJsonValue>& NameVal : *ActorNamesArr)
	{
		FString ActorName;
		if (!NameVal->TryGetString(ActorName) || ActorName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each entry in actor_names must be a non-empty string"));
		}

		FString FindError;
		AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, FindError);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s — %s"), *ActorName, *FindError));
		}

		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents(SMCs);
		if (SMCs.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' has no StaticMeshComponents"), *ActorName));
		}

		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (SMC && SMC->GetStaticMesh())
			{
				Components.Add(SMC);
			}
		}

		ResolvedNames.Add(ActorName);
	}

	if (Components.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Need at least 2 static mesh components to merge"));
	}

	// Get the MeshMergeUtilities module
	const IMeshMergeUtilities& MergeUtilities = FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();

	// Configure merge settings
	FMeshMergingSettings MergeSettings;
	MergeSettings.bMergeMeshSockets = true;
	MergeSettings.bMergePhysicsData = true;
	MergeSettings.bBakeVertexDataToMesh = false;
	MergeSettings.PivotType = EMeshMergePivotType::Automatic;

	if (bMergeMaterials)
	{
		MergeSettings.bMergeMaterials = true;
		MergeSettings.MaterialSettings.TextureSize = FIntPoint(TextureSize, TextureSize);
	}

	// Create the output package
	FString PackageName = SavePath;
	FString AssetName = FPackageName::GetShortName(PackageName);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Perform the merge
	FVector MergedActorPivot = FVector::ZeroVector;
	TArray<UObject*> CreatedAssets;

	MergeUtilities.MergeComponentsToStaticMesh(
		Components,
		World,
		MergeSettings,
		nullptr, // InBaseMaterial
		Package,  // InOuter
		PackageName,
		CreatedAssets,
		MergedActorPivot,
		static_cast<float>(ScreenSize),
		true // bSilent
	);

	if (CreatedAssets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Merge produced no output assets. Check that actors have valid static meshes."));
	}

	// Find the merged static mesh in created assets
	UStaticMesh* MergedMesh = nullptr;
	for (UObject* Obj : CreatedAssets)
	{
		MergedMesh = Cast<UStaticMesh>(Obj);
		if (MergedMesh) break;
	}

	auto Result = MakeShared<FJsonObject>();

	if (MergedMesh)
	{
		// Save the package
		FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, MergedMesh, *PackageFilename, SaveArgs);

		Result->SetStringField(TEXT("status"), TEXT("success"));
		Result->SetStringField(TEXT("merged_mesh_path"), SavePath);
		Result->SetNumberField(TEXT("source_components"), Components.Num());
		Result->SetNumberField(TEXT("source_actors"), ResolvedNames.Num());

		// Report merged mesh stats
		if (MergedMesh->GetRenderData() && MergedMesh->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD0 = MergedMesh->GetRenderData()->LODResources[0];
			Result->SetNumberField(TEXT("merged_triangles"), LOD0.GetNumTriangles());
			Result->SetNumberField(TEXT("merged_vertices"), LOD0.GetNumVertices());
			Result->SetNumberField(TEXT("merged_sections"), LOD0.Sections.Num());
		}

		Result->SetNumberField(TEXT("created_assets"), CreatedAssets.Num());
		Result->SetArrayField(TEXT("pivot"), VectorToJsonArray(MergedActorPivot));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("completed_no_mesh"));
		Result->SetNumberField(TEXT("created_assets"), CreatedAssets.Num());
		Result->SetStringField(TEXT("note"), TEXT("Merge completed but no StaticMesh found in output. Check created assets."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. setup_hlod
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::SetupHlod(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: save_path (asset path for HLOD layer)"));
	}

	FString LayerTypeStr = TEXT("MeshSimplify");
	if (Params->HasField(TEXT("layer_type")) && !Params->TryGetStringField(TEXT("layer_type"), LayerTypeStr))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'layer_type'. Expected string."));
	}

	double CellSizeD = 25600.0;
	if (Params->HasField(TEXT("cell_size")) && !Params->TryGetNumberField(TEXT("cell_size"), CellSizeD))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'cell_size'. Expected number."));
	}
	int32 CellSize = FMath::Clamp(static_cast<int32>(CellSizeD), 1600, 409600);

	double LoadingRange = 2.0;
	if (Params->HasField(TEXT("loading_range")) && !Params->TryGetNumberField(TEXT("loading_range"), LoadingRange))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'loading_range'. Expected number."));
	}
	LoadingRange = FMath::Clamp(LoadingRange, 0.5, 10.0);

	// Map string to enum
	EHLODLayerType LayerType;
	if (LayerTypeStr.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshMerge;
	}
	else if (LayerTypeStr.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshSimplify;
	}
	else if (LayerTypeStr.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshApproximate;
	}
	else if (LayerTypeStr.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::Custom;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid layer_type: %s. Use MeshMerge, MeshSimplify, MeshApproximate, or Custom"), *LayerTypeStr));
	}

	// Create the HLOD layer asset
	FString PackageName = SavePath;
	FString AssetName = FPackageName::GetShortName(PackageName);

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(PackageName); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	UHLODLayer* HLODLayer = NewObject<UHLODLayer>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!HLODLayer)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UHLODLayer"));
	}

	HLODLayer->SetLayerType(LayerType);

	// CellSize and LoadingRange are private UPROPERTY — set via property system
	if (FIntProperty* CellSizeProp = CastField<FIntProperty>(UHLODLayer::StaticClass()->FindPropertyByName(TEXT("CellSize"))))
	{
		CellSizeProp->SetPropertyValue_InContainer(HLODLayer, CellSize);
	}
	if (FDoubleProperty* LoadingRangeProp = CastField<FDoubleProperty>(UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LoadingRange"))))
	{
		LoadingRangeProp->SetPropertyValue_InContainer(HLODLayer, LoadingRange);
	}

	// Mark dirty and save
	HLODLayer->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(HLODLayer);

	FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, HLODLayer, *PackageFilename, SaveArgs);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("success"));
	Result->SetStringField(TEXT("hlod_layer_path"), SavePath);
	Result->SetStringField(TEXT("layer_type"), LayerTypeStr);
	Result->SetNumberField(TEXT("cell_size"), CellSize);
	Result->SetNumberField(TEXT("loading_range"), LoadingRange);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. analyze_texture_budget
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::AnalyzeTextureBudget(const TSharedPtr<FJsonObject>& Params)
{
	FString ScanPath;
	if (Params->HasField(TEXT("scan_path")) && !Params->TryGetStringField(TEXT("scan_path"), ScanPath))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'scan_path'. Expected string."));
	}

	double TopCountD = 20.0;
	if (Params->HasField(TEXT("top_count")) && !Params->TryGetNumberField(TEXT("top_count"), TopCountD))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'top_count'. Expected number."));
	}
	int32 TopCount = FMath::Clamp(static_cast<int32>(TopCountD), 1, 100);

	// Collect all texture info
	struct FTextureInfo
	{
		FString Path;
		FString Format;
		int32 Width = 0;
		int32 Height = 0;
		int64 ResourceSize = 0;
		int32 MipCount = 0;
		bool bIsStreamable = false;
	};

	TArray<FTextureInfo> Textures;
	int64 TotalResourceSize = 0;
	TMap<FString, int64> ByFormat;
	TMap<FString, int32> ByFormatCount;
	int32 TexturesOver2K = 0;
	int32 TexturesOver4K = 0;
	int32 NonPowerOf2 = 0;

	for (TObjectIterator<UTexture2D> It; It; ++It)
	{
		UTexture2D* Tex = *It;
		if (!Tex || Tex->HasAnyFlags(RF_Transient) || Tex->GetPathName().StartsWith(TEXT("/Engine/")))
		{
			continue;
		}

		// Filter by scan path if specified
		FString TexPath = Tex->GetPathName();
		if (!ScanPath.IsEmpty() && !TexPath.StartsWith(ScanPath))
		{
			continue;
		}

		FTextureInfo Info;
		Info.Path = TexPath;
		Info.Width = Tex->GetSizeX();
		Info.Height = Tex->GetSizeY();
		Info.ResourceSize = Tex->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
		Info.MipCount = Tex->GetNumMips();
		Info.bIsStreamable = Tex->IsStreamable();

		// Format string
		if (Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Info.Format = GPixelFormats[Tex->GetPlatformData()->PixelFormat].Name;
		}
		else
		{
			Info.Format = TEXT("Unknown");
		}

		TotalResourceSize += Info.ResourceSize;

		// Accumulate by format
		int64& FormatTotal = ByFormat.FindOrAdd(Info.Format, 0);
		FormatTotal += Info.ResourceSize;
		int32& FormatCount = ByFormatCount.FindOrAdd(Info.Format, 0);
		FormatCount++;

		// Stats
		if (Info.Width > 2048 || Info.Height > 2048)
		{
			TexturesOver2K++;
		}
		if (Info.Width > 4096 || Info.Height > 4096)
		{
			TexturesOver4K++;
		}
		if (!FMath::IsPowerOfTwo(Info.Width) || !FMath::IsPowerOfTwo(Info.Height))
		{
			NonPowerOf2++;
		}

		Textures.Add(MoveTemp(Info));
	}

	// Sort by resource size descending
	Textures.Sort([](const FTextureInfo& A, const FTextureInfo& B)
	{
		return A.ResourceSize > B.ResourceSize;
	});

	// Pool size from CVar
	float PoolSizeMB = 0.0f;
	IConsoleVariable* PoolCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
	if (PoolCVar)
	{
		PoolSizeMB = static_cast<float>(PoolCVar->GetInt());
	}

	// Top textures
	TArray<TSharedPtr<FJsonValue>> TopArr;
	const int32 ReturnTopCount = FMath::Min(TopCount, Textures.Num());
	TopArr.Reserve(ReturnTopCount);
	for (int32 i = 0; i < ReturnTopCount; ++i)
	{
		const FTextureInfo& Info = Textures[i];
		auto TexObj = MakeShared<FJsonObject>();
		TexObj->SetStringField(TEXT("path"), Info.Path);
		TexObj->SetStringField(TEXT("format"), Info.Format);
		TexObj->SetNumberField(TEXT("width"), Info.Width);
		TexObj->SetNumberField(TEXT("height"), Info.Height);
		TexObj->SetNumberField(TEXT("size_mb"), static_cast<double>(Info.ResourceSize) / (1024.0 * 1024.0));
		TexObj->SetNumberField(TEXT("mips"), Info.MipCount);
		TexObj->SetBoolField(TEXT("streamable"), Info.bIsStreamable);
		TopArr.Add(MakeShared<FJsonValueObject>(TexObj));
	}

	// By-format breakdown
	auto FormatObj = MakeShared<FJsonObject>();
	for (const auto& Pair : ByFormat)
	{
		auto FmtEntry = MakeShared<FJsonObject>();
		FmtEntry->SetNumberField(TEXT("size_mb"), static_cast<double>(Pair.Value) / (1024.0 * 1024.0));
		FmtEntry->SetNumberField(TEXT("count"), ByFormatCount.FindRef(Pair.Key));
		FormatObj->SetObjectField(Pair.Key, FmtEntry);
	}

	// Recommendations
	TArray<TSharedPtr<FJsonValue>> Recommendations;
	double UsedMB = static_cast<double>(TotalResourceSize) / (1024.0 * 1024.0);

	if (PoolSizeMB > 0 && UsedMB > PoolSizeMB)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("OVER BUDGET: Using %.1fMB of %.0fMB pool. Reduce largest textures or increase r.Streaming.PoolSize."), UsedMB, PoolSizeMB)));
	}
	if (TexturesOver4K > 0)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%d textures exceed 4K resolution. Consider downscaling to 2K for non-hero assets."), TexturesOver4K)));
	}
	if (NonPowerOf2 > 10)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%d non-power-of-2 textures detected. These can't mip properly and waste VRAM."), NonPowerOf2)));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("pool_size_mb"), PoolSizeMB);
	Result->SetNumberField(TEXT("used_mb"), UsedMB);
	Result->SetNumberField(TEXT("texture_count"), Textures.Num());
	Result->SetNumberField(TEXT("textures_over_2k"), TexturesOver2K);
	Result->SetNumberField(TEXT("textures_over_4k"), TexturesOver4K);
	Result->SetNumberField(TEXT("non_power_of_2"), NonPowerOf2);
	Result->SetArrayField(TEXT("top_textures"), TopArr);
	Result->SetObjectField(TEXT("by_format"), FormatObj);
	Result->SetArrayField(TEXT("recommendations"), Recommendations);

	return FMonolithActionResult::Success(Result);
}
