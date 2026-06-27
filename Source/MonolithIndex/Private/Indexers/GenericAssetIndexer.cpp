#include "Indexers/GenericAssetIndexer.h"
#include "Utility/MonolithSearchValueWriter.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FGenericAssetIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!LoadedAsset) return false;

	FIndexedNode MetaNode;
	MetaNode.AssetId = AssetId;
	MetaNode.NodeType = TEXT("Metadata");
	MetaNode.NodeName = LoadedAsset->GetName();
	MetaNode.NodeClass = LoadedAsset->GetClass()->GetName();

	auto Props = MakeShared<FJsonObject>();

	if (UStaticMesh* SM = Cast<UStaticMesh>(LoadedAsset))
	{
		if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
			Props->SetNumberField(TEXT("triangles"), LOD0.GetNumTriangles());
			Props->SetNumberField(TEXT("vertices"), LOD0.GetNumVertices());
			Props->SetNumberField(TEXT("sections"), LOD0.Sections.Num());
		}
		Props->SetNumberField(TEXT("lod_count"), SM->GetNumLODs());
		Props->SetNumberField(TEXT("material_slots"), SM->GetStaticMaterials().Num());

		FBoxSphereBounds Bounds = SM->GetBounds();
		Props->SetStringField(TEXT("bounds_extent"),
			FString::Printf(TEXT("%.1f x %.1f x %.1f"),
				Bounds.BoxExtent.X * 2, Bounds.BoxExtent.Y * 2, Bounds.BoxExtent.Z * 2));

		Props->SetBoolField(TEXT("has_collision"), SM->GetBodySetup() != nullptr);
	}
	else if (USkeletalMesh* SK = Cast<USkeletalMesh>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("lod_count"), SK->GetLODNum());
		Props->SetNumberField(TEXT("material_slots"), SK->GetMaterials().Num());

		if (SK->GetSkeleton())
		{
			Props->SetNumberField(TEXT("bone_count"), SK->GetSkeleton()->GetReferenceSkeleton().GetNum());
			Props->SetStringField(TEXT("skeleton"), SK->GetSkeleton()->GetPathName());
		}

		if (SK->GetPhysicsAsset())
		{
			Props->SetStringField(TEXT("physics_asset"), SK->GetPhysicsAsset()->GetPathName());
		}
	}
	else if (UTexture2D* Tex = Cast<UTexture2D>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("width"), Tex->GetSizeX());
		Props->SetNumberField(TEXT("height"), Tex->GetSizeY());
		Props->SetStringField(TEXT("format"), GPixelFormats[Tex->GetPixelFormat()].Name);
		Props->SetNumberField(TEXT("mip_count"), Tex->GetNumMips());
		Props->SetBoolField(TEXT("srgb"), Tex->SRGB);
		Props->SetBoolField(TEXT("has_alpha"), Tex->HasAlphaChannel());
		Props->SetStringField(TEXT("compression"),
			UEnum::GetValueAsString(Tex->CompressionSettings));
		Props->SetStringField(TEXT("lod_group"),
			UEnum::GetValueAsString(Tex->LODGroup));
		Props->SetStringField(TEXT("filter"),
			UEnum::GetValueAsString(Tex->Filter));
		Props->SetStringField(TEXT("address_x"),
			UEnum::GetValueAsString(Tex->GetTextureAddressX()));
		Props->SetStringField(TEXT("address_y"),
			UEnum::GetValueAsString(Tex->GetTextureAddressY()));
#if WITH_EDITORONLY_DATA
		Props->SetBoolField(TEXT("virtual_texture_streaming"), Tex->VirtualTextureStreaming != 0);
		Props->SetBoolField(TEXT("compression_no_alpha"), Tex->CompressionNoAlpha != 0);
#endif
		// Recommended sampler type for material use
		EMaterialSamplerType SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Tex);
		UEnum* SamplerEnum = StaticEnum<EMaterialSamplerType>();
		if (SamplerEnum)
		{
			Props->SetStringField(TEXT("recommended_sampler_type"),
				SamplerEnum->GetNameStringByValue(static_cast<int64>(SamplerType)));
		}

		// PRD AssetSearchSemanticSearch (UE5.8): the texture metadata above lives only on the
		// Metadata-node JSON blob (reachable via get_asset_details), so size/format/PoT are NOT
		// FTS-discoverable by project.search. Mirror the high-value fields into asset_search_values
		// (source_kind 'texture') so they are searchable, and add a derived power-of-two / exact
		// dimensions audit signal. Powers the CLAUDE.md power-of-two / 1024x1024 / 128px-cell atlas
		// audit: "find non_power_of_two textures", "which textures are 2048x2048", "find srgb mask
		// textures", "which atlases use TC_Default compression". Reuses the already-proven accessors.
		FMonolithSearchValueWriter TexSearch(DB);
		if (TexSearch.IsEnabled())
		{
			const FString TexPath = Tex->GetPathName();
			const FString TexName = Tex->GetName();
			// Use the imported (authored) size, NOT GetSizeX()/GetSizeY(): the latter read the
			// runtime PlatformData mip0, which is 0 in a headless -nullrhi indexing editor.
			// GetImportedSize() returns the RHI-independent authored dimensions (Texture2D.h:40).
			const FIntPoint ImportedSize = Tex->GetImportedSize();
			const int32 SizeX = ImportedSize.X;
			const int32 SizeY = ImportedSize.Y;
			auto AddTex = [&TexSearch, AssetId, &TexName, &TexPath](const TCHAR* Field, const FString& Value, const TCHAR* Signal)
			{
				TexSearch.AddValue(AssetId, TEXT("texture"), TexName, TexPath, TEXT("Texture2D"),
					Field, TexPath + TEXT(".") + Field, Value, Signal);
			};
			AddTex(TEXT("dimensions"), FString::Printf(TEXT("%dx%d"), SizeX, SizeY), TEXT("texture_size"));
			AddTex(TEXT("width"), FString::FromInt(SizeX), TEXT("texture_size"));
			AddTex(TEXT("height"), FString::FromInt(SizeY), TEXT("texture_size"));
			// Guard SizeX/SizeY > 0: IsPowerOfTwo(0) is true, so a 0-size (un-imported) texture
			// would otherwise mislabel as power_of_two — flag it as non_power_of_two instead.
			AddTex(TEXT("power_of_two"),
				(SizeX > 0 && SizeY > 0 && FMath::IsPowerOfTwo(SizeX) && FMath::IsPowerOfTwo(SizeY))
					? TEXT("power_of_two") : TEXT("non_power_of_two"),
				TEXT("texture_audit"));
			AddTex(TEXT("compression_settings"), UEnum::GetValueAsString(Tex->CompressionSettings), TEXT("texture_format"));
			AddTex(TEXT("lod_group"), UEnum::GetValueAsString(Tex->LODGroup), TEXT("texture_lod"));
			AddTex(TEXT("srgb"), Tex->SRGB ? TEXT("srgb") : TEXT("linear"), TEXT("texture_audit"));
#if WITH_EDITORONLY_DATA
			// Authored source pixel format (RHI-independent; the runtime GetPixelFormat() is
			// PF_Unknown in a headless -nullrhi indexer). e.g. "TSF_BGRA8".
			AddTex(TEXT("source_format"), UEnum::GetValueAsString(Tex->Source.GetFormat()), TEXT("texture_format"));
#endif
		}
	}
	else if (USoundWave* Sound = Cast<USoundWave>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("duration"), Sound->Duration);
		Props->SetNumberField(TEXT("sample_rate"), Sound->GetSampleRateForCurrentPlatform());
		Props->SetNumberField(TEXT("channels"), Sound->NumChannels);
		Props->SetBoolField(TEXT("looping"), Sound->bLooping);
	}

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);
	MetaNode.Properties = PropsStr;

	DB.InsertNode(MetaNode);
	return true;
}
