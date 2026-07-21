#include "MonolithMeshReplacementActions.h"
#include "MonolithMeshExactNameUtils.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "ISourceControlChangelist.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "MeshDescription.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MonolithMeshReplacementActions"

namespace
{
	FMonolithActionExecutionPolicy ReplacementExecutionPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("track_dirty_packages");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		// The handler owns one transaction spanning its exact target mutation,
		// package save, and byte-verified rollback contract.
		Policy.bTransactionWrapping = false;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}

	struct FExactStaticMeshAsset
	{
		FString InputPath;
		FString ObjectPath;
		FString PackageName;
		FString PackageFilename;
		UStaticMesh* Mesh = nullptr;
		UPackage* Package = nullptr;
	};

	struct FLodSettingsSnapshot
	{
		FMeshBuildSettings BuildSettings;
		FMeshReductionSettings ReductionSettings;
		FPerPlatformFloat ScreenSize;
		FString SourceImportFilename;
#if WITH_EDITORONLY_DATA
		bool bImportWithBaseMesh = false;
#endif
	};

	struct FMeshDescriptionSnapshot
	{
		FMeshDescription Description;
		FString Digest;
		int32 VertexCount = 0;
		int32 VertexInstanceCount = 0;
		int32 TriangleCount = 0;
		int32 PolygonGroupCount = 0;
		int32 UVChannelCount = 0;
		int32 RenderSectionCount = 0;
	};

	struct FStaticMeshSnapshot
	{
		FString ObjectPath;
		FString PackageName;
		FString ObjectName;
		FString ClassPath;
		TArray<FMeshDescriptionSnapshot> Lods;
		TArray<FLodSettingsSnapshot> LodSettings;
		TArray<FStaticMaterial> StaticMaterials;
		int32 LightMapCoordinateIndex = 0;
		int32 LightMapResolution = 0;
		FString CollisionDigest;
		FMeshSectionInfoMap SectionInfoMap;
		FMeshSectionInfoMap OriginalSectionInfoMap;
		bool bNeverStream = false;
	};

	struct FReplacementPolicies
	{
		FString Material;
		FString Collision;
		FString UV;
		FString Lightmap;
		FString BuildSettings;
		FString Lod;
		FString Section;
		FString SourceControl;
		TMap<FString, FString> MaterialRemap;
	};

	struct FSourceControlPreview
	{
		bool bProviderReady = false;
		bool bStateValid = false;
		bool bSourceControlled = false;
		bool bCheckedOut = false;
		bool bCheckedOutOther = false;
		bool bConflicted = false;
		bool bCanCheckout = false;
		bool bIsCurrent = false;
		bool bIsAdded = false;
		bool bIsDeleted = false;
		bool bIsIgnored = false;
		bool bDefaultChangelist = false;
		FString CheckedOutOtherBy;
		FString ProviderName;
		FString ActualChangelist;
	};

	struct FTargetMaterialSlotAlias
	{
		FName Alias;
		FName CanonicalSlot;
		int32 MaterialIndex = INDEX_NONE;
	};

#if WITH_DEV_AUTOMATION_TESTS
	struct FStaticMeshReplacementTestState
	{
		FString ExactTargetFilename;
		FString ExactNumberedChangelist;
		UE::MonolithMesh::Private::EStaticMeshReplacementTestFault Fault =
			UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::None;
		int32 SourceControlReadCount = 0;
	};

	FStaticMeshReplacementTestState GStaticMeshReplacementTestState;

	bool TryReadTestSourceControlPreview(
		const FString& Filename,
		FSourceControlPreview& OutPreview)
	{
		if (GStaticMeshReplacementTestState.ExactTargetFilename.IsEmpty()
			|| !FPaths::IsSamePath(
				Filename,
				GStaticMeshReplacementTestState.ExactTargetFilename))
		{
			return false;
		}

		++GStaticMeshReplacementTestState.SourceControlReadCount;
		OutPreview = FSourceControlPreview();
		OutPreview.bProviderReady = true;
		OutPreview.bStateValid = true;
		OutPreview.bSourceControlled = true;
		OutPreview.bCheckedOut = true;
		OutPreview.bIsCurrent = true;
		OutPreview.ProviderName = TEXT("MonolithReplacementFixture");
		OutPreview.ActualChangelist =
			GStaticMeshReplacementTestState.ExactNumberedChangelist;
		return true;
	}

	bool ConsumeStaticMeshReplacementTestFault(
		const UE::MonolithMesh::Private::EStaticMeshReplacementTestFault Fault)
	{
		if (GStaticMeshReplacementTestState.Fault != Fault)
		{
			return false;
		}
		GStaticMeshReplacementTestState.Fault =
			UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::None;
		return true;
	}
#endif

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	FString HashBytes(const TArray<uint8>& Bytes)
	{
		FMD5 Md5;
		if (Bytes.Num() > 0)
		{
			Md5.Update(Bytes.GetData(), Bytes.Num());
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	bool HashFileBytes(
		const FString& Filename,
		int64& OutSize,
		FString& OutDigest,
		FString& OutError)
	{
		OutSize = IFileManager::Get().FileSize(*Filename);
		OutDigest.Reset();
		if (OutSize < 0)
		{
			OutError = FString::Printf(TEXT("cannot read package file size: %s"), *Filename);
			return false;
		}

		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
		if (!Reader)
		{
			OutError = FString::Printf(TEXT("cannot open package file for hashing: %s"), *Filename);
			return false;
		}

		FMD5 Md5;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1024 * 1024);
		int64 Remaining = OutSize;
		while (Remaining > 0)
		{
			const int32 ChunkSize = static_cast<int32>(FMath::Min<int64>(Remaining, Buffer.Num()));
			Reader->Serialize(Buffer.GetData(), ChunkSize);
			if (Reader->IsError())
			{
				OutError = FString::Printf(TEXT("failed while hashing package file: %s"), *Filename);
				return false;
			}
			Md5.Update(Buffer.GetData(), ChunkSize);
			Remaining -= ChunkSize;
		}

		uint8 Digest[16];
		Md5.Final(Digest);
		OutDigest = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		return true;
	}

	bool MeshSectionInfoEqualsExact(const FMeshSectionInfo& A, const FMeshSectionInfo& B)
	{
		return A.MaterialIndex == B.MaterialIndex
			&& A.bEnableCollision == B.bEnableCollision
			&& A.bCastShadow == B.bCastShadow
			&& A.bVisibleInRayTracing == B.bVisibleInRayTracing
			&& A.bAffectDistanceFieldLighting == B.bAffectDistanceFieldLighting
			&& A.bForceOpaque == B.bForceOpaque;
	}

	bool MeshSectionInfoMapEqualsExact(const FMeshSectionInfoMap& A, const FMeshSectionInfoMap& B)
	{
		if (A.Map.Num() != B.Map.Num())
		{
			return false;
		}
		for (const TPair<uint32, FMeshSectionInfo>& Pair : A.Map)
		{
			const FMeshSectionInfo* Other = B.Map.Find(Pair.Key);
			if (!Other || !MeshSectionInfoEqualsExact(Pair.Value, *Other))
			{
				return false;
			}
		}
		return true;
	}

	FString HashMeshDescription(const FMeshDescription& Description)
	{
		FMeshDescription Copy = Description;
		FBufferArchive Bytes;
		Copy.Serialize(Bytes);
		return HashBytes(Bytes);
	}

	FString HashAuthoredSimpleBodySetup(UBodySetup* BodySetup)
	{
		if (!BodySetup)
		{
			return TEXT("none");
		}

		// Hash every non-transient reflected BodySetup property in stable class
		// declaration order. This includes AggGeom, trace semantics, physical
		// material, slope, FBodyInstance response/profile data, build scale, and
		// authored cook/support flags while excluding derived cooked caches/GUIDs.
		FBufferArchive Bytes;
		FObjectAndNameAsStringProxyArchive Archive(Bytes, /*bInLoadIfFindFails=*/false);
		for (TFieldIterator<FProperty> It(BodySetup->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(
				CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient
				| CPF_Deprecated | CPF_SkipSerialization))
			{
				continue;
			}
			FString PropertyPath = Property->GetPathName();
			Archive << PropertyPath;
			Property->SerializeItem(
				FStructuredArchiveFromArchive(Archive).GetSlot(),
				Property->ContainerPtrToValuePtr<void>(BodySetup),
				nullptr);
		}
		return HashBytes(Bytes);
	}

	bool IsSupportedAuthoredSimpleCollisionTraceFlag(const ECollisionTraceFlag TraceFlag)
	{
		return TraceFlag == ECollisionTraceFlag::CTF_UseSimpleAsComplex
			|| TraceFlag == ECollisionTraceFlag::CTF_UseSimpleAndComplex;
	}

	bool CanonicalizeExactAssetPath(
		const FString& Input,
		FString& OutObjectPath,
		FString& OutError)
	{
		OutObjectPath.Reset();
		OutError.Reset();

		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty() || Trimmed != Input)
		{
			OutError = TEXT("asset paths must be non-empty and contain no leading/trailing whitespace");
			return false;
		}
		if (!Input.StartsWith(TEXT("/")) || Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT("'")) || Input.Contains(TEXT(":"))
			|| Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("asset paths must be exact mounted package/object paths (for example /ProjectMGH/Meshes/SM_Wall.SM_Wall); relative, export-text, subobject, filesystem, and extension forms are rejected");
			return false;
		}

		FString PackageName;
		if (Input.Contains(TEXT(".")))
		{
			const FSoftObjectPath SoftPath(Input);
			if (!SoftPath.IsValid() || !SoftPath.GetSubPathString().IsEmpty())
			{
				OutError = FString::Printf(TEXT("invalid exact object path: %s"), *Input);
				return false;
			}
			OutObjectPath = SoftPath.GetAssetPathString();
			PackageName = FPackageName::ObjectPathToPackageName(OutObjectPath);
		}
		else
		{
			if (!FPackageName::IsValidLongPackageName(Input, /*bIncludeReadOnlyRoots=*/true))
			{
				OutError = FString::Printf(TEXT("invalid exact mounted package path: %s"), *Input);
				return false;
			}
			PackageName = Input;
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
			if (AssetName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("package path has no asset name: %s"), *Input);
				return false;
			}
			OutObjectPath = PackageName + TEXT(".") + AssetName;
		}

		if (!FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = FString::Printf(TEXT("invalid canonical object path: %s"), *OutObjectPath);
			return false;
		}
		return true;
	}

	bool LoadExactStaticMesh(
		const FString& InputPath,
		const bool bRequirePackageFile,
		FExactStaticMeshAsset& OutAsset,
		FString& OutError)
	{
		OutAsset = FExactStaticMeshAsset();
		OutAsset.InputPath = InputPath;
		if (!CanonicalizeExactAssetPath(InputPath, OutAsset.ObjectPath, OutError))
		{
			return false;
		}

		OutAsset.Mesh = Cast<UStaticMesh>(StaticLoadObject(
			UStaticMesh::StaticClass(), nullptr, *OutAsset.ObjectPath));
		if (!OutAsset.Mesh)
		{
			OutError = FString::Printf(TEXT("exact StaticMesh path did not resolve: %s"), *OutAsset.ObjectPath);
			return false;
		}
		if (OutAsset.Mesh->GetClass() != UStaticMesh::StaticClass())
		{
			OutError = FString::Printf(
				TEXT("exact class precondition failed for '%s': expected UStaticMesh, got %s"),
				*OutAsset.ObjectPath,
				*OutAsset.Mesh->GetClass()->GetPathName());
			return false;
		}
		if (OutAsset.Mesh->GetPathName() != OutAsset.ObjectPath)
		{
			OutError = FString::Printf(
				TEXT("redirects and fuzzy identity resolution are forbidden: requested '%s', resolved '%s'"),
				*OutAsset.ObjectPath,
				*OutAsset.Mesh->GetPathName());
			return false;
		}

		OutAsset.Package = OutAsset.Mesh->GetOutermost();
		if (!OutAsset.Package || OutAsset.Package->ContainsMap())
		{
			OutError = FString::Printf(TEXT("StaticMesh must live in a non-map package: %s"), *OutAsset.ObjectPath);
			return false;
		}
		OutAsset.PackageName = OutAsset.Package->GetName();
		if (OutAsset.PackageName != FPackageName::ObjectPathToPackageName(OutAsset.ObjectPath))
		{
			OutError = FString::Printf(TEXT("package identity mismatch for %s"), *OutAsset.ObjectPath);
			return false;
		}

		if (bRequirePackageFile
			&& !FPackageName::DoesPackageExist(OutAsset.PackageName, &OutAsset.PackageFilename))
		{
			OutError = FString::Printf(TEXT("existing target package has no on-disk file: %s"), *OutAsset.PackageName);
			return false;
		}
		return true;
	}

	bool SnapshotLod(
		const UStaticMesh& Mesh,
		const int32 LodIndex,
		FMeshDescriptionSnapshot& OutSnapshot,
		FString& OutError)
	{
		FMeshDescription Description;
		if (!Mesh.CloneMeshDescription(LodIndex, Description))
		{
			OutError = FString::Printf(
				TEXT("StaticMesh '%s' has no committed MeshDescription for LOD%d"),
				*Mesh.GetPathName(), LodIndex);
			return false;
		}

		OutSnapshot.Description = MoveTemp(Description);
		OutSnapshot.Digest = HashMeshDescription(OutSnapshot.Description);
		OutSnapshot.VertexCount = OutSnapshot.Description.Vertices().Num();
		OutSnapshot.VertexInstanceCount = OutSnapshot.Description.VertexInstances().Num();
		OutSnapshot.TriangleCount = OutSnapshot.Description.Triangles().Num();
		OutSnapshot.PolygonGroupCount = OutSnapshot.Description.PolygonGroups().Num();
		FStaticMeshAttributes Attributes(OutSnapshot.Description);
		OutSnapshot.UVChannelCount = Attributes.GetVertexInstanceUVs().GetNumChannels();
		if (!Mesh.GetRenderData() || !Mesh.GetRenderData()->LODResources.IsValidIndex(LodIndex))
		{
			OutError = FString::Printf(
				TEXT("StaticMesh '%s' has no stable render data for LOD%d"),
				*Mesh.GetPathName(),
				LodIndex);
			return false;
		}
		OutSnapshot.RenderSectionCount = Mesh.GetRenderData()->LODResources[LodIndex].Sections.Num();
		if (OutSnapshot.RenderSectionCount <= 0)
		{
			OutError = FString::Printf(
				TEXT("StaticMesh '%s' has no render sections for LOD%d"),
				*Mesh.GetPathName(),
				LodIndex);
			return false;
		}
		return true;
	}

	FLodSettingsSnapshot SnapshotLodSettings(const UStaticMesh& Mesh, const int32 LodIndex)
	{
		const FStaticMeshSourceModel& Model = Mesh.GetSourceModel(LodIndex);
		FLodSettingsSnapshot Snapshot;
		Snapshot.BuildSettings = Model.BuildSettings;
		Snapshot.ReductionSettings = Model.ReductionSettings;
		Snapshot.ScreenSize = Model.ScreenSize;
		Snapshot.SourceImportFilename = Model.SourceImportFilename;
#if WITH_EDITORONLY_DATA
		Snapshot.bImportWithBaseMesh = Model.bImportWithBaseMesh;
#endif
		return Snapshot;
	}

	bool SnapshotStaticMesh(
		const FExactStaticMeshAsset& Asset,
		FStaticMeshSnapshot& OutSnapshot,
		FString& OutError)
	{
		OutSnapshot = FStaticMeshSnapshot();
		OutSnapshot.ObjectPath = Asset.Mesh->GetPathName();
		OutSnapshot.PackageName = Asset.Package->GetName();
		OutSnapshot.ObjectName = Asset.Mesh->GetName();
		OutSnapshot.ClassPath = Asset.Mesh->GetClass()->GetClassPathName().ToString();
		OutSnapshot.StaticMaterials = Asset.Mesh->GetStaticMaterials();
		OutSnapshot.LightMapCoordinateIndex = Asset.Mesh->GetLightMapCoordinateIndex();
		OutSnapshot.LightMapResolution = Asset.Mesh->GetLightMapResolution();
		OutSnapshot.CollisionDigest = HashAuthoredSimpleBodySetup(Asset.Mesh->GetBodySetup());
		OutSnapshot.SectionInfoMap.CopyFrom(Asset.Mesh->GetSectionInfoMap());
		OutSnapshot.OriginalSectionInfoMap.CopyFrom(Asset.Mesh->GetOriginalSectionInfoMap());
		OutSnapshot.bNeverStream = Asset.Mesh->NeverStream;

		const int32 LodCount = Asset.Mesh->GetNumSourceModels();
		if (LodCount <= 0)
		{
			OutError = FString::Printf(TEXT("StaticMesh has no source LODs: %s"), *Asset.ObjectPath);
			return false;
		}
		OutSnapshot.Lods.Reserve(LodCount);
		OutSnapshot.LodSettings.Reserve(LodCount);
		for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
		{
			FMeshDescriptionSnapshot Lod;
			if (!SnapshotLod(*Asset.Mesh, LodIndex, Lod, OutError))
			{
				return false;
			}
			OutSnapshot.Lods.Add(MoveTemp(Lod));
			OutSnapshot.LodSettings.Add(SnapshotLodSettings(*Asset.Mesh, LodIndex));
		}
		return true;
	}

	bool ParsePolicies(
		const TSharedPtr<FJsonObject>& Params,
		FReplacementPolicies& OutPolicies,
		FString& OutError)
	{
		auto ReadRequiredPolicy = [&](const TCHAR* Name, FString& OutValue) -> bool
		{
			if (!Params->TryGetStringField(Name, OutValue) || OutValue.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is required and must be a non-empty string"), Name);
				return false;
			}
			OutValue.ToLowerInline();
			return true;
		};

		if (!ReadRequiredPolicy(TEXT("material_policy"), OutPolicies.Material)
			|| !ReadRequiredPolicy(TEXT("collision_policy"), OutPolicies.Collision)
			|| !ReadRequiredPolicy(TEXT("uv_policy"), OutPolicies.UV)
			|| !ReadRequiredPolicy(TEXT("lightmap_policy"), OutPolicies.Lightmap)
			|| !ReadRequiredPolicy(TEXT("build_settings_policy"), OutPolicies.BuildSettings)
			|| !ReadRequiredPolicy(TEXT("lod_policy"), OutPolicies.Lod)
			|| !ReadRequiredPolicy(TEXT("section_policy"), OutPolicies.Section)
			|| !ReadRequiredPolicy(TEXT("source_control_policy"), OutPolicies.SourceControl))
		{
			return false;
		}

		if (OutPolicies.Material != TEXT("preserve_target_by_name")
			&& OutPolicies.Material != TEXT("preserve_target_single_slot")
			&& OutPolicies.Material != TEXT("explicit_remap"))
		{
			OutError = TEXT("material_policy must be preserve_target_by_name, preserve_target_single_slot, or explicit_remap");
			return false;
		}
		if (OutPolicies.Collision != TEXT("preserve_target_authored_simple"))
		{
			OutError = TEXT("collision_policy currently permits only preserve_target_authored_simple; complex collision, collision copying, and collision generation are outside this action's contract");
			return false;
		}
		if (OutPolicies.UV != TEXT("copy_source_mesh_description"))
		{
			OutError = TEXT("uv_policy currently permits only copy_source_mesh_description because UV layers are part of the copied MeshDescription");
			return false;
		}
		if (OutPolicies.Lightmap != TEXT("preserve_target")
			&& OutPolicies.Lightmap != TEXT("copy_source"))
		{
			OutError = TEXT("lightmap_policy must be preserve_target or copy_source");
			return false;
		}
		if (OutPolicies.BuildSettings != TEXT("preserve_target")
			&& OutPolicies.BuildSettings != TEXT("copy_source"))
		{
			OutError = TEXT("build_settings_policy must be preserve_target or copy_source");
			return false;
		}
		if (OutPolicies.Lod != TEXT("copy_all_source_lods"))
		{
			OutError = TEXT("lod_policy currently permits only copy_all_source_lods");
			return false;
		}
		if (OutPolicies.Section != TEXT("preserve_target_exact_layout"))
		{
			OutError = TEXT("section_policy currently permits only preserve_target_exact_layout");
			return false;
		}
		if (OutPolicies.SourceControl != TEXT("require_checked_out"))
		{
			OutError = TEXT("source_control_policy currently permits only require_checked_out; prepare the target in the intended numbered changelist before executing");
			return false;
		}

		if (OutPolicies.Material == TEXT("explicit_remap"))
		{
			const TSharedPtr<FJsonObject>* RemapObject = nullptr;
			if (!Params->TryGetObjectField(TEXT("material_remap"), RemapObject)
				|| !RemapObject || !RemapObject->IsValid())
			{
				OutError = TEXT("material_remap object is required when material_policy=explicit_remap");
				return false;
			}
			TSet<FName> SourceAliases;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RemapObject)->Values)
			{
				FString TargetSlot;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetSlot)
					|| Pair.Key.IsEmpty() || TargetSlot.IsEmpty())
				{
					OutError = TEXT("material_remap must map non-empty source slot names to non-empty target slot names");
					return false;
				}
				const FName SourceAlias(*Pair.Key);
				if (SourceAliases.Contains(SourceAlias))
				{
					OutError = FString::Printf(
						TEXT("material_remap contains duplicate or case-aliased source slot key '%s'"),
						*Pair.Key);
					return false;
				}
				SourceAliases.Add(SourceAlias);
				OutPolicies.MaterialRemap.Add(Pair.Key, TargetSlot);
			}
		}
		else if (Params->HasField(TEXT("material_remap")))
		{
			OutError = TEXT("material_remap is allowed only when material_policy=explicit_remap");
			return false;
		}
		return true;
	}

	FName ResolvePolygonGroupSlot(
		const FPolygonGroupID PolygonGroupId,
		const TPolygonGroupAttributesRef<FName>& SlotNames)
	{
		// A PolygonGroupID is not a material-array index. Missing metadata is
		// ambiguous and must not be guessed during a name-preserving migration.
		return SlotNames[PolygonGroupId];
	}

	bool BuildTargetSlotLookup(
		const TArray<FStaticMaterial>& Materials,
		TArray<FTargetMaterialSlotAlias>& OutLookup,
		FString& OutError)
	{
		OutLookup.Reset();
		for (int32 MaterialIndex = 0; MaterialIndex < Materials.Num(); ++MaterialIndex)
		{
			const FStaticMaterial& Material = Materials[MaterialIndex];
			if (Material.MaterialSlotName.IsNone())
			{
				OutError = FString::Printf(
					TEXT("target material index %d has no canonical MaterialSlotName"),
					MaterialIndex);
				return false;
			}

			auto AddAlias = [&](const FName Alias) -> bool
			{
				if (Alias.IsNone())
				{
					return true;
				}
				for (const FTargetMaterialSlotAlias& Existing : OutLookup)
				{
					// FName equality alone is case-insensitive, so retain the exact
					// display spelling for lookup while still rejecting aliases that
					// Unreal would collapse onto different material indices.
					if (Existing.Alias == Alias)
					{
						if (Existing.MaterialIndex != MaterialIndex)
						{
							OutError = FString::Printf(
								TEXT("target material alias '%s' is ambiguous between indices %d and %d"),
								*Alias.ToString(),
								Existing.MaterialIndex,
								MaterialIndex);
							return false;
						}

						if (MonolithMeshExactNameUtils::EqualsCaseSensitive(
							Existing.Alias,
							Alias.ToString()))
						{
							return true;
						}
					}
				}
				OutLookup.Add(FTargetMaterialSlotAlias{
					Alias,
					Material.MaterialSlotName,
					MaterialIndex});
				return true;
			};

			if (!AddAlias(Material.MaterialSlotName)
				|| !AddAlias(Material.ImportedMaterialSlotName))
			{
				return false;
			}
		}
		return true;
	}

	const FName* FindExactTargetSlot(
		const TArray<FTargetMaterialSlotAlias>& TargetSlots,
		const FString& RequestedSlot)
	{
		for (const FTargetMaterialSlotAlias& Entry : TargetSlots)
		{
			if (MonolithMeshExactNameUtils::EqualsCaseSensitive(Entry.Alias, RequestedSlot))
			{
				return &Entry.CanonicalSlot;
			}
		}
		return nullptr;
	}

	bool TryGetExactMaterialRemapValue(
		const TMap<FString, FString>& MaterialRemap,
		const FString& ExactSourceSlot,
		const FString*& OutValue,
		FString& OutError)
	{
		OutValue = nullptr;
		const FString* CaseAliasedKey = nullptr;
		for (const TPair<FString, FString>& Pair : MaterialRemap)
		{
			if (Pair.Key.Equals(ExactSourceSlot, ESearchCase::CaseSensitive))
			{
				OutValue = &Pair.Value;
				return true;
			}
			if (Pair.Key.Equals(ExactSourceSlot, ESearchCase::IgnoreCase))
			{
				CaseAliasedKey = &Pair.Key;
			}
		}

		if (CaseAliasedKey)
		{
			OutError = FString::Printf(
				TEXT("material_remap key '%s' is case-aliased; exact source slot key '%s' is required"),
				**CaseAliasedKey,
				*ExactSourceSlot);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("material_remap does not cover exact source slot key '%s'"),
				*ExactSourceSlot);
		}
		return false;
	}

	bool RemapMaterials(
		const int32 SourceMaterialCount,
		const TArray<FStaticMaterial>& TargetMaterials,
		const FReplacementPolicies& Policies,
		TArray<FMeshDescriptionSnapshot>& InOutLods,
		TArray<FString>& OutAppliedMappings,
		FString& OutError)
	{
		TArray<FTargetMaterialSlotAlias> TargetSlots;
		if (!BuildTargetSlotLookup(TargetMaterials, TargetSlots, OutError))
		{
			return false;
		}
		if (TargetSlots.IsEmpty())
		{
			OutError = TEXT("target StaticMesh has no named material slots to preserve/remap");
			return false;
		}
		const bool bPreserveSingleSlot =
			Policies.Material == TEXT("preserve_target_single_slot");
		if (bPreserveSingleSlot
			&& (SourceMaterialCount != 1 || TargetMaterials.Num() != 1))
		{
			OutError = FString::Printf(
				TEXT("material_policy=preserve_target_single_slot requires exactly one source and one target material slot (source=%d, target=%d)"),
				SourceMaterialCount,
				TargetMaterials.Num());
			return false;
		}

		TSet<FString> AppliedPairs;
		TSet<FString> UsedSourceSlots;
		for (int32 LodIndex = 0; LodIndex < InOutLods.Num(); ++LodIndex)
		{
			FMeshDescriptionSnapshot& Lod = InOutLods[LodIndex];
			FStaticMeshAttributes Attributes(Lod.Description);
			TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			if (bPreserveSingleSlot
				&& (Lod.Description.PolygonGroups().Num() != 1
					|| Lod.RenderSectionCount != 1))
			{
				OutError = FString::Printf(
					TEXT("material_policy=preserve_target_single_slot requires exactly one polygon group and one render section in every source LOD; LOD%d has %d polygon group(s) and %d render section(s)"),
					LodIndex,
					Lod.Description.PolygonGroups().Num(),
					Lod.RenderSectionCount);
				return false;
			}
			for (const FPolygonGroupID PolygonGroupId : Lod.Description.PolygonGroups().GetElementIDs())
			{
				const FName SourceSlotName = ResolvePolygonGroupSlot(PolygonGroupId, SlotNames);
				if (bPreserveSingleSlot)
				{
					const FName CanonicalTargetSlot = TargetMaterials[0].MaterialSlotName;
					check(!CanonicalTargetSlot.IsNone());
					SlotNames[PolygonGroupId] = CanonicalTargetSlot;
					AppliedPairs.Add(FString::Printf(
						TEXT("%s -> %s (strict single-slot)"),
						SourceSlotName.IsNone()
							? TEXT("<unnamed polygon group>")
							: *SourceSlotName.ToString(),
						*CanonicalTargetSlot.ToString()));
					continue;
				}
				if (SourceSlotName.IsNone())
				{
					OutError = FString::Printf(TEXT("source LOD polygon group %d has no resolvable material slot name"), PolygonGroupId.GetValue());
					return false;
				}

				const FString SourceSlot = SourceSlotName.ToString();
				UsedSourceSlots.Add(SourceSlot);
				FString RequestedTargetSlot = SourceSlot;
				if (Policies.Material == TEXT("explicit_remap"))
				{
					const FString* Remapped = nullptr;
					if (!TryGetExactMaterialRemapValue(
						Policies.MaterialRemap,
						SourceSlot,
						Remapped,
						OutError))
					{
						return false;
					}
					RequestedTargetSlot = *Remapped;
				}

				const FName* CanonicalTargetSlot = FindExactTargetSlot(TargetSlots, RequestedTargetSlot);
				if (!CanonicalTargetSlot || CanonicalTargetSlot->IsNone())
				{
					OutError = FString::Printf(
						TEXT("source slot '%s' maps to missing target slot '%s'"),
						*SourceSlot, *RequestedTargetSlot);
					return false;
				}

				SlotNames[PolygonGroupId] = *CanonicalTargetSlot;
				AppliedPairs.Add(SourceSlot + TEXT(" -> ") + CanonicalTargetSlot->ToString());
			}
			Lod.Digest = HashMeshDescription(Lod.Description);
		}

		if (Policies.Material == TEXT("explicit_remap"))
		{
			if (!UE::MonolithMesh::Private::ValidateExactMaterialRemapKeys(
				UsedSourceSlots,
				Policies.MaterialRemap,
				OutError))
			{
				return false;
			}
		}

		OutAppliedMappings.Reserve(AppliedPairs.Num());
		for (const FString& Pair : AppliedPairs)
		{
			OutAppliedMappings.Add(Pair);
		}
		OutAppliedMappings.Sort();
		return true;
	}

	bool GetOrderedMaterialSlots(
		FMeshDescription& Description,
		TArray<FString>& OutSlots,
		FString& OutError,
		const FName StrictSingleSlotFallback = NAME_None)
	{
		TArray<TPair<int32, FString>> IndexedSlots;
		FStaticMeshAttributes Attributes(Description);
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID PolygonGroupId : Description.PolygonGroups().GetElementIDs())
		{
			FName SlotName = ResolvePolygonGroupSlot(PolygonGroupId, SlotNames);
			if (SlotName.IsNone()
				&& !StrictSingleSlotFallback.IsNone()
				&& Description.PolygonGroups().Num() == 1)
			{
				// The explicit strict-single-slot policy proves this is the only
				// possible material identity.  Canonicalize the snapshot readback
				// without mutating the live target merely to compare its layout.
				SlotName = StrictSingleSlotFallback;
			}
			if (SlotName.IsNone())
			{
				OutError = FString::Printf(
					TEXT("polygon group %d has no explicit material slot metadata"),
					PolygonGroupId.GetValue());
				return false;
			}
			IndexedSlots.Emplace(
				PolygonGroupId.GetValue(),
				SlotName.ToString());
		}
		IndexedSlots.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B)
		{
			return A.Key < B.Key;
		});

		OutSlots.Reset();
		OutSlots.Reserve(IndexedSlots.Num());
		for (const TPair<int32, FString>& Pair : IndexedSlots)
		{
			OutSlots.Add(Pair.Value);
		}
		return true;
	}

	FSourceControlPreview ReadSourceControlPreview(const FString& Filename)
	{
		FSourceControlPreview Preview;
#if WITH_DEV_AUTOMATION_TESTS
		if (TryReadTestSourceControlPreview(Filename, Preview))
		{
			return Preview;
		}
#endif
		ISourceControlModule& Module = ISourceControlModule::Get();
		Preview.bProviderReady = Module.IsEnabled()
			&& Module.GetProvider().IsEnabled()
			&& Module.GetProvider().IsAvailable();
		if (!Preview.bProviderReady)
		{
			return Preview;
		}

		ISourceControlProvider& Provider = Module.GetProvider();
		Preview.ProviderName = Provider.GetName().ToString();
		const FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::ForceUpdate);
		Preview.bStateValid = State.IsValid();
		if (!State.IsValid())
		{
			return Preview;
		}
		Preview.bSourceControlled = State->IsSourceControlled();
		Preview.bCheckedOut = State->IsCheckedOut();
		Preview.bCheckedOutOther = State->IsCheckedOutOther(&Preview.CheckedOutOtherBy);
		Preview.bConflicted = State->IsConflicted();
		Preview.bCanCheckout = State->CanCheckout();
		Preview.bIsCurrent = State->IsCurrent();
		Preview.bIsAdded = State->IsAdded();
		Preview.bIsDeleted = State->IsDeleted();
		Preview.bIsIgnored = State->IsIgnored();
		if (const FSourceControlChangelistPtr Changelist = State->GetCheckInIdentifier())
		{
			Preview.ActualChangelist = Changelist->GetIdentifier();
			Preview.bDefaultChangelist = Changelist->IsDefault();
		}
		return Preview;
	}

	TSharedPtr<FJsonObject> SourceControlPreviewToJson(const FSourceControlPreview& Preview)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("provider_ready"), Preview.bProviderReady);
		Result->SetStringField(TEXT("provider"), Preview.ProviderName);
		Result->SetBoolField(TEXT("state_valid"), Preview.bStateValid);
		Result->SetBoolField(TEXT("source_controlled"), Preview.bSourceControlled);
		Result->SetBoolField(TEXT("checked_out"), Preview.bCheckedOut);
		Result->SetBoolField(TEXT("checked_out_other"), Preview.bCheckedOutOther);
		Result->SetStringField(TEXT("checked_out_other_by"), Preview.CheckedOutOtherBy);
		Result->SetBoolField(TEXT("conflicted"), Preview.bConflicted);
		Result->SetBoolField(TEXT("can_checkout"), Preview.bCanCheckout);
		Result->SetBoolField(TEXT("current"), Preview.bIsCurrent);
		Result->SetBoolField(TEXT("added"), Preview.bIsAdded);
		Result->SetBoolField(TEXT("deleted"), Preview.bIsDeleted);
		Result->SetBoolField(TEXT("ignored"), Preview.bIsIgnored);
		Result->SetStringField(TEXT("actual_changelist"), Preview.ActualChangelist);
		Result->SetBoolField(TEXT("default_changelist"), Preview.bDefaultChangelist);
		return Result;
	}

	TSharedPtr<FJsonObject> BuildHandlerOwnedSourceControlPrepare(
		const FSourceControlPreview& Preview,
		const FString& ExpectedChangelist,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Prepare = MakeShared<FJsonObject>();
		Prepare->SetStringField(TEXT("mode"), TEXT("handler_owned_pre_mutation"));
		Prepare->SetStringField(TEXT("status"), Status);
		Prepare->SetStringField(TEXT("expected_changelist"), ExpectedChangelist);
		Prepare->SetObjectField(TEXT("before_action"), SourceControlPreviewToJson(Preview));
		return Prepare;
	}

	bool ValidateSourceControlState(
		const FSourceControlPreview& State,
		const FString& ExpectedChangelist,
		FString& OutError)
	{
		if (!State.bProviderReady || !State.bStateValid)
		{
			OutError = TEXT("source-control provider/state is unavailable; target mutation is forbidden");
			return false;
		}
		if (!State.bSourceControlled || State.bIsAdded || State.bIsDeleted || State.bIsIgnored)
		{
			OutError = TEXT("target must be an existing tracked edit, not added, deleted, or ignored");
			return false;
		}
		if (!State.bIsCurrent)
		{
			OutError = TEXT("target source-control revision is not current");
			return false;
		}
		if (State.bCheckedOutOther)
		{
			OutError = FString::Printf(TEXT("target is checked out by another user/client: %s"), *State.CheckedOutOtherBy);
			return false;
		}
		if (State.bConflicted)
		{
			OutError = TEXT("target source-control state is conflicted");
			return false;
		}
		if (!State.bCheckedOut)
		{
			OutError = TEXT("target is not checked out; prepare it in the intended numbered changelist before executing");
			return false;
		}
		if (State.bDefaultChangelist || State.ActualChangelist.IsEmpty()
			|| State.ActualChangelist != ExpectedChangelist)
		{
			OutError = FString::Printf(
				TEXT("target changelist mismatch: expected exact numbered changelist '%s', actual '%s'%s"),
				*ExpectedChangelist,
				State.ActualChangelist.IsEmpty() ? TEXT("<unavailable>") : *State.ActualChangelist,
				State.bDefaultChangelist ? TEXT(" (default changelist is forbidden)") : TEXT(""));
			return false;
		}
		return true;
	}

	bool PrepareSourceControl(
		const FString& Filename,
		const FString& ExpectedChangelist,
		FSourceControlPreview& OutState,
		FString& OutError)
	{
		OutState = ReadSourceControlPreview(Filename);
		return ValidateSourceControlState(OutState, ExpectedChangelist, OutError);
	}

	bool ScreenSizeEquals(const FPerPlatformFloat& A, const FPerPlatformFloat& B)
	{
		if (!FMath::IsNearlyEqual(A.Default, B.Default))
		{
			return false;
		}
#if WITH_EDITORONLY_DATA || WITH_PREVIEW_PPX_DATA
		return A.PerPlatform.OrderIndependentCompareEqual(B.PerPlatform);
#else
		return true;
#endif
	}

	void ApplyLodSettings(UStaticMesh& Mesh, const TArray<FLodSettingsSnapshot>& Settings)
	{
		for (int32 LodIndex = 0; LodIndex < Settings.Num(); ++LodIndex)
		{
			FStaticMeshSourceModel& Model = Mesh.GetSourceModel(LodIndex);
			Model.BuildSettings = Settings[LodIndex].BuildSettings;
			Model.ReductionSettings = Settings[LodIndex].ReductionSettings;
			Model.ScreenSize = Settings[LodIndex].ScreenSize;
			Model.SourceImportFilename = Settings[LodIndex].SourceImportFilename;
#if WITH_EDITORONLY_DATA
			Model.bImportWithBaseMesh = Settings[LodIndex].bImportWithBaseMesh;
#endif
		}
	}

	bool VerifyStaticMeshPostconditions(
		const FExactStaticMeshAsset& Reloaded,
		const FStaticMeshSnapshot& OriginalTarget,
		const TArray<FMeshDescriptionSnapshot>& ExpectedLods,
		const TArray<FLodSettingsSnapshot>& ExpectedSettings,
		const int32 ExpectedLightMapCoordinateIndex,
		const int32 ExpectedLightMapResolution,
		FString& OutError)
	{
		if (Reloaded.ObjectPath != OriginalTarget.ObjectPath
			|| Reloaded.PackageName != OriginalTarget.PackageName
			|| Reloaded.Mesh->GetName() != OriginalTarget.ObjectName
			|| Reloaded.Mesh->GetClass()->GetClassPathName().ToString() != OriginalTarget.ClassPath)
		{
			OutError = TEXT("target logical UObject/package identity changed after reload");
			return false;
		}
		if (Reloaded.Package->IsDirty())
		{
			OutError = TEXT("reloaded target package is unexpectedly dirty");
			return false;
		}
		if (Reloaded.Mesh->GetNumSourceModels() != ExpectedLods.Num())
		{
			OutError = FString::Printf(
				TEXT("LOD postcondition failed: expected %d, got %d"),
				ExpectedLods.Num(), Reloaded.Mesh->GetNumSourceModels());
			return false;
		}

		for (int32 LodIndex = 0; LodIndex < ExpectedLods.Num(); ++LodIndex)
		{
			FMeshDescriptionSnapshot Actual;
			if (!SnapshotLod(*Reloaded.Mesh, LodIndex, Actual, OutError))
			{
				return false;
			}
			if (Actual.Digest != ExpectedLods[LodIndex].Digest
				|| Actual.VertexCount != ExpectedLods[LodIndex].VertexCount
				|| Actual.VertexInstanceCount != ExpectedLods[LodIndex].VertexInstanceCount
				|| Actual.TriangleCount != ExpectedLods[LodIndex].TriangleCount
				|| Actual.PolygonGroupCount != ExpectedLods[LodIndex].PolygonGroupCount
				|| Actual.UVChannelCount != ExpectedLods[LodIndex].UVChannelCount
				|| Actual.RenderSectionCount != ExpectedLods[LodIndex].RenderSectionCount)
			{
				OutError = FString::Printf(TEXT("LOD%d MeshDescription readback does not exactly match the planned geometry"), LodIndex);
				return false;
			}

			const FStaticMeshSourceModel& ActualModel = Reloaded.Mesh->GetSourceModel(LodIndex);
			if (!(ActualModel.BuildSettings == ExpectedSettings[LodIndex].BuildSettings)
				|| !(ActualModel.ReductionSettings == ExpectedSettings[LodIndex].ReductionSettings)
				|| !ScreenSizeEquals(ActualModel.ScreenSize, ExpectedSettings[LodIndex].ScreenSize)
				|| ActualModel.SourceImportFilename != ExpectedSettings[LodIndex].SourceImportFilename
#if WITH_EDITORONLY_DATA
				|| ActualModel.bImportWithBaseMesh != ExpectedSettings[LodIndex].bImportWithBaseMesh
#endif
				)
			{
				OutError = FString::Printf(TEXT("LOD%d build/reduction/screen/import metadata postcondition failed"), LodIndex);
				return false;
			}

			if (!Reloaded.Mesh->GetRenderData()
				|| !Reloaded.Mesh->GetRenderData()->LODResources.IsValidIndex(LodIndex))
			{
				OutError = FString::Printf(TEXT("LOD%d render data is missing after reload"), LodIndex);
				return false;
			}
			const FStaticMeshLODResources& LodResources = Reloaded.Mesh->GetRenderData()->LODResources[LodIndex];
			for (int32 SectionIndex = 0; SectionIndex < LodResources.Sections.Num(); ++SectionIndex)
			{
				const FMeshSectionInfo ExpectedSection = OriginalTarget.SectionInfoMap.Get(LodIndex, SectionIndex);
				const FStaticMeshSection& ActualSection = LodResources.Sections[SectionIndex];
				if (ActualSection.MaterialIndex != ExpectedSection.MaterialIndex
					|| ActualSection.bEnableCollision != ExpectedSection.bEnableCollision
					|| ActualSection.bCastShadow != ExpectedSection.bCastShadow
					|| ActualSection.bVisibleInRayTracing != ExpectedSection.bVisibleInRayTracing
					|| ActualSection.bAffectDistanceFieldLighting != ExpectedSection.bAffectDistanceFieldLighting
					|| ActualSection.bForceOpaque != ExpectedSection.bForceOpaque)
				{
					OutError = FString::Printf(
						TEXT("LOD%d section %d render flags do not match preserved SectionInfoMap"),
						LodIndex,
						SectionIndex);
					return false;
				}
			}
		}

		if (Reloaded.Mesh->GetStaticMaterials() != OriginalTarget.StaticMaterials)
		{
			OutError = TEXT("target material array changed despite target-slot preservation policy");
			return false;
		}
		if (Reloaded.Mesh->GetLightMapCoordinateIndex() != ExpectedLightMapCoordinateIndex
			|| Reloaded.Mesh->GetLightMapResolution() != ExpectedLightMapResolution)
		{
			OutError = TEXT("lightmap policy postcondition failed");
			return false;
		}
		if (HashAuthoredSimpleBodySetup(Reloaded.Mesh->GetBodySetup()) != OriginalTarget.CollisionDigest)
		{
			OutError = TEXT("authored simple collision changed despite collision_policy=preserve_target_authored_simple");
			return false;
		}
		if (!MeshSectionInfoMapEqualsExact(Reloaded.Mesh->GetSectionInfoMap(), OriginalTarget.SectionInfoMap)
			|| !MeshSectionInfoMapEqualsExact(Reloaded.Mesh->GetOriginalSectionInfoMap(), OriginalTarget.OriginalSectionInfoMap))
		{
			OutError = TEXT("section info maps changed despite section_policy=preserve_target_exact_layout");
			return false;
		}
		if (Reloaded.Mesh->NeverStream != OriginalTarget.bNeverStream)
		{
			OutError = TEXT("NeverStream changed during geometry rebuild");
			return false;
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> LodsToJson(const TArray<FMeshDescriptionSnapshot>& Lods)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Lods.Num());
		for (int32 Index = 0; Index < Lods.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("lod"), Index);
			Row->SetNumberField(TEXT("vertices"), Lods[Index].VertexCount);
			Row->SetNumberField(TEXT("vertex_instances"), Lods[Index].VertexInstanceCount);
			Row->SetNumberField(TEXT("triangles"), Lods[Index].TriangleCount);
			Row->SetNumberField(TEXT("polygon_groups"), Lods[Index].PolygonGroupCount);
			Row->SetNumberField(TEXT("uv_channels"), Lods[Index].UVChannelCount);
			Row->SetNumberField(TEXT("render_sections"), Lods[Index].RenderSectionCount);
			Row->SetStringField(TEXT("mesh_description_md5"), Lods[Index].Digest);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	TSharedPtr<FJsonObject> PoliciesToJson(const FReplacementPolicies& Policies)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("material"), Policies.Material);
		Result->SetStringField(TEXT("collision"), Policies.Collision);
		Result->SetStringField(TEXT("uv"), Policies.UV);
		Result->SetStringField(TEXT("lightmap"), Policies.Lightmap);
		Result->SetStringField(TEXT("build_settings"), Policies.BuildSettings);
		Result->SetStringField(TEXT("lod"), Policies.Lod);
		Result->SetStringField(TEXT("section"), Policies.Section);
		Result->SetStringField(TEXT("source_control"), Policies.SourceControl);
		return Result;
	}

	bool ReloadPackageAndResolve(
		const FString& PackageName,
		const FString& ObjectPath,
		const EReloadPackagesInteractionMode InteractionMode,
		FExactStaticMeshAsset& OutReloaded,
		FString& OutError)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("loaded package disappeared before reload: %s"), *PackageName);
			return false;
		}

		FText ReloadError;
		TArray<UPackage*> Packages = { Package };
		if (!UPackageTools::ReloadPackages(
			Packages,
			ReloadError,
			InteractionMode))
		{
			OutError = FString::Printf(TEXT("package reload failed: %s"), *ReloadError.ToString());
			return false;
		}
		return LoadExactStaticMesh(ObjectPath, /*bRequirePackageFile=*/true, OutReloaded, OutError);
	}
}

bool UE::MonolithMesh::Private::ValidateExactMaterialRemapKeys(
	const TSet<FString>& UsedSourceSlots,
	const TMap<FString, FString>& MaterialRemap,
	FString& OutError)
{
	if (MaterialRemap.Num() != UsedSourceSlots.Num())
	{
		OutError = FString::Printf(
			TEXT("material_remap key set must exactly equal the %d source slots actually used across all LODs; got %d key(s)"),
			UsedSourceSlots.Num(),
			MaterialRemap.Num());
		return false;
	}
	for (const FString& UsedSourceSlot : UsedSourceSlots)
	{
		const FString* Remapped = nullptr;
		if (!TryGetExactMaterialRemapValue(
			MaterialRemap,
			UsedSourceSlot,
			Remapped,
			OutError))
		{
			return false;
		}
	}
	return true;
}

bool UE::MonolithMesh::Private::ValidateExecuteEditorState(
	const bool bEditorAvailable,
	const bool bPlaySessionInProgress,
	const bool bPlayWorldAvailable,
	const bool bIsPlayInEditorWorld,
	const bool bIsSimulatingInEditor,
	FString& OutError)
{
	if (!bEditorAvailable)
	{
		OutError = TEXT("execute requires a live editor engine; dry_run remains available without GEditor");
		return false;
	}
	if (bPlaySessionInProgress || bPlayWorldAvailable
		|| bIsPlayInEditorWorld || bIsSimulatingInEditor)
	{
		OutError = TEXT("execute is forbidden while PIE/SIE is active or queued; stop the play/simulate session and retry");
		return false;
	}
	return true;
}

bool UE::MonolithMesh::Private::RestoreBackupBytesExact(
	const FString& BackupFilename,
	const FString& TargetFilename,
	const int64 ExpectedSize,
	const FString& ExpectedDigest,
	FString& OutError)
{
	if (IFileManager::Get().Copy(*TargetFilename, *BackupFilename, /*bReplace=*/true) != COPY_OK)
	{
		OutError = TEXT("failed to restore the original package bytes from the rollback backup");
		return false;
	}

	int64 RestoredSize = 0;
	FString RestoredDigest;
	if (!HashFileBytes(TargetFilename, RestoredSize, RestoredDigest, OutError))
	{
		return false;
	}
	if (RestoredSize != ExpectedSize || RestoredDigest != ExpectedDigest)
	{
		OutError = FString::Printf(
			TEXT("restored target bytes do not match original size/MD5 (size %lld/%lld, md5 %s/%s)"),
			RestoredSize,
			ExpectedSize,
			*RestoredDigest,
			*ExpectedDigest);
		return false;
	}
	return true;
}

FString UE::MonolithMesh::Private::HashAuthoredBodySetupForVerification(UBodySetup* BodySetup)
{
	return HashAuthoredSimpleBodySetup(BodySetup);
}

#if WITH_DEV_AUTOMATION_TESTS
void UE::MonolithMesh::Private::ConfigureStaticMeshReplacementTestHooks(
	const FString& ExactTargetFilename,
	const int32 ExactNumberedChangelist,
	const EStaticMeshReplacementTestFault Fault)
{
	GStaticMeshReplacementTestState = FStaticMeshReplacementTestState();
	GStaticMeshReplacementTestState.ExactTargetFilename =
		FPaths::ConvertRelativePathToFull(ExactTargetFilename);
	FPaths::NormalizeFilename(GStaticMeshReplacementTestState.ExactTargetFilename);
	GStaticMeshReplacementTestState.ExactNumberedChangelist =
		FString::FromInt(ExactNumberedChangelist);
	GStaticMeshReplacementTestState.Fault = Fault;
}

void UE::MonolithMesh::Private::ResetStaticMeshReplacementTestHooks()
{
	GStaticMeshReplacementTestState = FStaticMeshReplacementTestState();
}

int32 UE::MonolithMesh::Private::GetStaticMeshReplacementSourceControlReadCountForTests()
{
	return GStaticMeshReplacementTestState.SourceControlReadCount;
}
#endif

void FMonolithMeshReplacementActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("mesh"),
		TEXT("replace_static_mesh_geometry_in_place"),
		TEXT("Transactionally replace all committed MeshDescription LOD geometry of an existing non-Nanite, no-HiRes StaticMesh while preserving logical UObject/package identity. Requires idle compilation, exact case-sensitive polygon-group slot metadata or the explicit strict single-slot policy, authored simple collision using SimpleAsComplex or SimpleAndComplex, an exact numbered changelist, no PIE/SIE execution, package rollback, save/reload, and readback verification. SimpleAndComplex rebuilds derived complex collision from the copied geometry; external complex-collision meshes remain unsupported."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshReplacementActions::ReplaceStaticMeshGeometryInPlace),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("source_asset_path"), TEXT("string"), TEXT("Exact mounted donor StaticMesh package/object path; redirects and relative/filesystem forms are rejected"))
			.Required(TEXT("target_asset_path"), TEXT("string"), TEXT("Exact existing target StaticMesh package/object path whose logical identity must remain unchanged"))
			.Required(TEXT("material_policy"), TEXT("string"), TEXT("preserve_target_by_name, explicit_remap, or preserve_target_single_slot; the strict single-slot policy requires exactly one source/target material and one polygon group/render section per LOD"))
			.Enum(TEXT("material_policy"), { TEXT("preserve_target_by_name"), TEXT("preserve_target_single_slot"), TEXT("explicit_remap") })
			.Optional(TEXT("material_remap"), TEXT("object"), TEXT("Complete case-sensitive source-slot to target-slot map; required only for explicit_remap"))
			.Required(TEXT("collision_policy"), TEXT("string"), TEXT("Must be preserve_target_authored_simple; target must have authored simple primitives, use SimpleAsComplex or SimpleAndComplex, and have no external complex collision mesh"))
			.Enum(TEXT("collision_policy"), { TEXT("preserve_target_authored_simple") })
			.Required(TEXT("uv_policy"), TEXT("string"), TEXT("Must be copy_source_mesh_description"))
			.Enum(TEXT("uv_policy"), { TEXT("copy_source_mesh_description") })
			.Required(TEXT("lightmap_policy"), TEXT("string"), TEXT("preserve_target or copy_source"))
			.Enum(TEXT("lightmap_policy"), { TEXT("preserve_target"), TEXT("copy_source") })
			.Required(TEXT("build_settings_policy"), TEXT("string"), TEXT("preserve_target or copy_source"))
			.Enum(TEXT("build_settings_policy"), { TEXT("preserve_target"), TEXT("copy_source") })
			.Required(TEXT("lod_policy"), TEXT("string"), TEXT("Must be copy_all_source_lods"))
			.Enum(TEXT("lod_policy"), { TEXT("copy_all_source_lods") })
			.Required(TEXT("section_policy"), TEXT("string"), TEXT("Must be preserve_target_exact_layout; source/target section topology and remapped slot order must match"))
			.Enum(TEXT("section_policy"), { TEXT("preserve_target_exact_layout") })
			.Required(TEXT("source_control_policy"), TEXT("string"), TEXT("Must be require_checked_out; prepare the asset in the intended numbered changelist before execution"))
			.Enum(TEXT("source_control_policy"), { TEXT("require_checked_out") })
			.Required(TEXT("target_changelist"), TEXT("integer"), TEXT("Exact positive numbered changelist that already owns the target; default changelist is forbidden"))
			.Range(TEXT("target_changelist"), 1.0, static_cast<double>(MAX_int32))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Return a validated replacement plan without checkout, mutation, save, or reload"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Explicit destructive confirmation; required when dry_run=false, and execution is rejected while PIE/SIE is active or queued"), TEXT("false"))
			.Build(),
		FString(),
		ReplacementExecutionPolicy());

	Registry.SetActionSearchMetadata(
		TEXT("mesh"),
		TEXT("replace_static_mesh_geometry_in_place"),
		{ TEXT("name preserving mesh replacement"), TEXT("copy donor meshdescription"), TEXT("replace static mesh geometry"), TEXT("preserve asset references"), TEXT("transactional mesh migration") },
		{ TEXT("replace_mesh_geometry"), TEXT("copy_static_mesh_lods_in_place"), TEXT("migrate_mesh_geometry") },
		{ TEXT("replace this existing wall mesh geometry without changing its path"), TEXT("copy donor LOD geometry into an existing target mesh and preserve references") });
}

FMonolithActionResult FMonolithMeshReplacementActions::ReplaceStaticMeshGeometryInPlace(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bDryRun = true;
	bool bConfirm = false;
	if ((Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun))
		|| (Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm)))
	{
		return FMonolithActionResult::Error(TEXT("dry_run and confirm must be booleans"));
	}
	if (!bDryRun && !bConfirm)
	{
		return FMonolithActionResult::Error(TEXT("dry_run=false requires confirm=true"));
	}

	FReplacementPolicies Policies;
	FString Error;
	if (!ParsePolicies(Params, Policies, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	int32 TargetChangelist = 0;
	double TargetChangelistValue = 0.0;
	if (!Params->TryGetNumberField(TEXT("target_changelist"), TargetChangelistValue)
		|| !FMath::IsFinite(TargetChangelistValue)
		|| TargetChangelistValue < 1.0
		|| TargetChangelistValue > static_cast<double>(MAX_int32)
		|| TargetChangelistValue != FMath::FloorToDouble(TargetChangelistValue))
	{
		return FMonolithActionResult::Error(
			TEXT("target_changelist is required and must be an exact positive numbered changelist; default is forbidden"));
	}
	TargetChangelist = static_cast<int32>(TargetChangelistValue);
	const FString ExpectedChangelist = FString::FromInt(TargetChangelist);

	FString SourcePath;
	FString TargetPath;
	if (!Params->TryGetStringField(TEXT("source_asset_path"), SourcePath) || SourcePath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("target_asset_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_asset_path and target_asset_path are required"));
	}

	FExactStaticMeshAsset Source;
	FExactStaticMeshAsset Target;
	if (!LoadExactStaticMesh(SourcePath, /*bRequirePackageFile=*/true, Source, Error))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("source precondition failed: %s"), *Error));
	}
	if (!LoadExactStaticMesh(TargetPath, /*bRequirePackageFile=*/true, Target, Error))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("target precondition failed: %s"), *Error));
	}
	if (Source.ObjectPath == Target.ObjectPath || Source.PackageName == Target.PackageName)
	{
		return FMonolithActionResult::Error(TEXT("source and target must be distinct StaticMesh packages"));
	}
	if (Source.Package->IsDirty())
	{
		return FMonolithActionResult::Error(TEXT("source donor package must be clean so the exact committed geometry is unambiguous"));
	}
	if (Target.Package->IsDirty())
	{
		return FMonolithActionResult::Error(TEXT("target package must be clean before transactional replacement; save or revert unrelated in-memory edits first"));
	}
	if (Source.Mesh->IsCompiling() || Source.Mesh->HasAnyDependenciesCompiling()
		|| Target.Mesh->IsCompiling() || Target.Mesh->HasAnyDependenciesCompiling())
	{
		return FMonolithActionResult::Error(
			TEXT("source and target StaticMeshes plus their dependencies must not be compiling; wait for asset compilation to finish before planning or executing replacement"));
	}
	if (Source.Mesh->IsHiResMeshDescriptionValid() || Target.Mesh->IsHiResMeshDescriptionValid())
	{
		return FMonolithActionResult::Error(
			TEXT("v1 replacement rejects source or target HiRes MeshDescription data; HiRes transfer/preservation is not implemented"));
	}
	if (Source.Mesh->GetNaniteSettings().bEnabled || Source.Mesh->HasValidNaniteData()
		|| Target.Mesh->GetNaniteSettings().bEnabled || Target.Mesh->HasValidNaniteData())
	{
		return FMonolithActionResult::Error(
			TEXT("v1 replacement rejects source or target Nanite settings/data; Nanite rebuild/preservation is not implemented"));
	}
	UBodySetup* TargetBodySetup = Target.Mesh->GetBodySetup();
	if (!TargetBodySetup
		|| TargetBodySetup->AggGeom.GetElementCount() <= 0
		|| !IsSupportedAuthoredSimpleCollisionTraceFlag(TargetBodySetup->GetCollisionTraceFlag())
#if WITH_EDITORONLY_DATA
		|| Target.Mesh->ComplexCollisionMesh != nullptr
#endif
		)
	{
		return FMonolithActionResult::Error(
			TEXT("collision_policy=preserve_target_authored_simple requires authored simple primitives, collision trace UseSimpleAsComplex or UseSimpleAndComplex, and no external ComplexCollisionMesh; UseSimpleAndComplex derives complex collision from the copied geometry, while other complex-collision semantics remain unsupported"));
	}
	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (AssetEditorSubsystem->FindEditorForAsset(Target.Mesh, /*bFocusIfOpen=*/false))
			{
				return FMonolithActionResult::Error(TEXT("close the target StaticMesh editor before replacement so save/reload verification is safe"));
			}
		}
	}

	FStaticMeshSnapshot SourceSnapshot;
	FStaticMeshSnapshot TargetSnapshot;
	if (!SnapshotStaticMesh(Source, SourceSnapshot, Error))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("source snapshot failed: %s"), *Error));
	}
	if (!SnapshotStaticMesh(Target, TargetSnapshot, Error))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("target snapshot failed: %s"), *Error));
	}
	if (Policies.BuildSettings == TEXT("preserve_target")
		&& TargetSnapshot.LodSettings.Num() < SourceSnapshot.Lods.Num())
	{
		return FMonolithActionResult::Error(
			TEXT("build_settings_policy=preserve_target requires the target to have at least as many authored LOD settings as the source"));
	}
	if (SourceSnapshot.Lods.Num() != TargetSnapshot.Lods.Num())
	{
		return FMonolithActionResult::Error(
			TEXT("section_policy=preserve_target_exact_layout requires source and target to have the same authored LOD count"));
	}

	TArray<FMeshDescriptionSnapshot> PlannedLods = SourceSnapshot.Lods;
	TArray<FString> AppliedMaterialMappings;
	if (!RemapMaterials(
		SourceSnapshot.StaticMaterials.Num(),
		TargetSnapshot.StaticMaterials,
		Policies,
		PlannedLods,
		AppliedMaterialMappings,
		Error))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("material policy precondition failed: %s"), *Error));
	}
	for (int32 LodIndex = 0; LodIndex < PlannedLods.Num(); ++LodIndex)
	{
		TArray<FString> PlannedSlots;
		TArray<FString> TargetSlots;
		const FName StrictTargetSingleSlotFallback =
			Policies.Material == TEXT("preserve_target_single_slot")
			&& TargetSnapshot.StaticMaterials.Num() == 1
				? TargetSnapshot.StaticMaterials[0].MaterialSlotName
				: NAME_None;
		if (!GetOrderedMaterialSlots(PlannedLods[LodIndex].Description, PlannedSlots, Error)
			|| !GetOrderedMaterialSlots(
				TargetSnapshot.Lods[LodIndex].Description,
				TargetSlots,
				Error,
				StrictTargetSingleSlotFallback))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("LOD%d section-slot metadata precondition failed: %s"),
				LodIndex,
				*Error));
		}
		if (PlannedLods[LodIndex].RenderSectionCount != TargetSnapshot.Lods[LodIndex].RenderSectionCount
			|| PlannedLods[LodIndex].PolygonGroupCount != TargetSnapshot.Lods[LodIndex].PolygonGroupCount
			|| PlannedSlots != TargetSlots)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("section_policy=preserve_target_exact_layout failed for LOD%d: source/remapped section count, polygon-group count, and ordered target slot names must exactly match the existing target layout"),
				LodIndex));
		}
	}

	TArray<FLodSettingsSnapshot> PlannedLodSettings;
	PlannedLodSettings.Reserve(PlannedLods.Num());
	for (int32 LodIndex = 0; LodIndex < PlannedLods.Num(); ++LodIndex)
	{
		PlannedLodSettings.Add(
			Policies.BuildSettings == TEXT("copy_source")
				? SourceSnapshot.LodSettings[LodIndex]
				: TargetSnapshot.LodSettings[LodIndex]);
		// Source import filenames are not geometry/build settings and must never
		// leak donor file identity into the target package.
		PlannedLodSettings.Last().SourceImportFilename =
			TargetSnapshot.LodSettings.IsValidIndex(LodIndex)
				? TargetSnapshot.LodSettings[LodIndex].SourceImportFilename
				: FString();
#if WITH_EDITORONLY_DATA
		PlannedLodSettings.Last().bImportWithBaseMesh =
			TargetSnapshot.LodSettings.IsValidIndex(LodIndex)
				? TargetSnapshot.LodSettings[LodIndex].bImportWithBaseMesh
				: false;
#endif
	}

	const int32 PlannedLightMapCoordinateIndex = Policies.Lightmap == TEXT("copy_source")
		? SourceSnapshot.LightMapCoordinateIndex
		: TargetSnapshot.LightMapCoordinateIndex;
	const int32 PlannedLightMapResolution = Policies.Lightmap == TEXT("copy_source")
		? SourceSnapshot.LightMapResolution
		: TargetSnapshot.LightMapResolution;
	for (int32 LodIndex = 0; LodIndex < PlannedLods.Num(); ++LodIndex)
	{
		if (PlannedLods[LodIndex].UVChannelCount <= 0
			|| PlannedLightMapCoordinateIndex < 0
			|| PlannedLightMapCoordinateIndex >= PlannedLods[LodIndex].UVChannelCount)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("lightmap coordinate index %d is invalid for copied LOD%d UV channel count %d"),
				PlannedLightMapCoordinateIndex,
				LodIndex,
				PlannedLods[LodIndex].UVChannelCount));
		}
	}

	const FSourceControlPreview SourceControlBefore = ReadSourceControlPreview(Target.PackageFilename);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), TEXT("replace_static_mesh_geometry_in_place"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("source_object_path"), Source.ObjectPath);
	Result->SetStringField(TEXT("target_object_path"), Target.ObjectPath);
	Result->SetStringField(TEXT("target_package"), Target.PackageName);
	Result->SetStringField(TEXT("target_class"), TargetSnapshot.ClassPath);
	Result->SetObjectField(TEXT("policies"), PoliciesToJson(Policies));
	Result->SetObjectField(TEXT("source_control_before"), SourceControlPreviewToJson(SourceControlBefore));
	Result->SetNumberField(TEXT("expected_target_changelist"), TargetChangelist);
	Result->SetArrayField(TEXT("target_lods_before"), LodsToJson(TargetSnapshot.Lods));
	Result->SetArrayField(TEXT("planned_lods"), LodsToJson(PlannedLods));
	Result->SetArrayField(TEXT("material_mappings"), ToJsonStrings(AppliedMaterialMappings));
	Result->SetNumberField(TEXT("planned_lightmap_coordinate_index"), PlannedLightMapCoordinateIndex);
	Result->SetNumberField(TEXT("planned_lightmap_resolution"), PlannedLightMapResolution);
	Result->SetBoolField(TEXT("logical_identity_will_be_preserved"), true);

	if (bDryRun)
	{
		FString SourceControlReadinessError;
		const bool bSourceControlReady = ValidateSourceControlState(
			SourceControlBefore,
			ExpectedChangelist,
			SourceControlReadinessError);
		Result->SetStringField(TEXT("status"), TEXT("validated_plan"));
		Result->SetBoolField(TEXT("would_mutate"), true);
		Result->SetBoolField(TEXT("source_control_ready"), bSourceControlReady);
		Result->SetStringField(TEXT("source_control_readiness_error"), SourceControlReadinessError);
		return FMonolithActionResult::Success(Result);
	}

	if (!UE::MonolithMesh::Private::ValidateExecuteEditorState(
		GEditor != nullptr,
		GEditor && GEditor->IsPlaySessionInProgress(),
		GEditor && GEditor->PlayWorld != nullptr,
		GIsPlayInEditorWorld,
		GEditor && GEditor->bIsSimulatingInEditor,
		Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FSourceControlPreview SourceControlPrepared;
	if (!PrepareSourceControl(Target.PackageFilename, ExpectedChangelist, SourceControlPrepared, Error))
	{
		TSharedPtr<FJsonObject> FailedPrepare = BuildHandlerOwnedSourceControlPrepare(
			SourceControlPrepared,
			ExpectedChangelist,
			TEXT("failed"));
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("source_control_prepare"), FailedPrepare);
		FMonolithActionResult PrepareError = FMonolithActionResult::Error(FString::Printf(
			TEXT("source-control prepare failed: %s"),
			*Error));
		PrepareError.WithErrorData(ErrorData);
		return PrepareError;
	}
	const TSharedPtr<FJsonObject> SourceControlPrepare =
		BuildHandlerOwnedSourceControlPrepare(
			SourceControlPrepared,
			ExpectedChangelist,
			TEXT("validated_exact_numbered_changelist"));
	Result->SetObjectField(TEXT("source_control_prepare"), SourceControlPrepare);
	const auto PreparedError = [&SourceControlPrepare](const FString& Message) -> FMonolithActionResult
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("source_control_prepare"), SourceControlPrepare);
		FMonolithActionResult ErrorResult = FMonolithActionResult::Error(Message);
		ErrorResult.WithErrorData(ErrorData);
		return ErrorResult;
	};

	int64 OriginalPackageSize = 0;
	FString OriginalPackageDigest;
	if (!HashFileBytes(Target.PackageFilename, OriginalPackageSize, OriginalPackageDigest, Error)
		|| OriginalPackageSize <= 0)
	{
		return PreparedError(FString::Printf(
			TEXT("failed to fingerprint the original target package before backup: %s"),
			Error.IsEmpty() ? TEXT("package is empty") : *Error));
	}

	const FString BackupFilename = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(),
		TEXT("MonolithStaticMeshReplace_"),
		TEXT(".uasset.bak"));
	if (IFileManager::Get().Copy(*BackupFilename, *Target.PackageFilename, /*bReplace=*/true) != COPY_OK)
	{
		return PreparedError(TEXT("failed to create the target package rollback backup; no mutation was attempted"));
	}
	bool bDeleteBackupOnExit = true;
	ON_SCOPE_EXIT
	{
		if (bDeleteBackupOnExit)
		{
			IFileManager::Get().Delete(*BackupFilename, /*RequireExists=*/false, /*EvenReadOnly=*/true);
		}
	};

	int64 BackupPackageSize = 0;
	FString BackupPackageDigest;
	if (!HashFileBytes(BackupFilename, BackupPackageSize, BackupPackageDigest, Error)
		|| BackupPackageSize != OriginalPackageSize
		|| BackupPackageDigest != OriginalPackageDigest)
	{
		return PreparedError(FString::Printf(
			TEXT("rollback backup fingerprint does not exactly match the original package; no mutation was attempted%s%s"),
			Error.IsEmpty() ? TEXT("") : TEXT(": "),
			*Error));
	}
	Result->SetNumberField(TEXT("target_package_size_before"), static_cast<double>(OriginalPackageSize));
	Result->SetStringField(TEXT("target_package_md5_before"), OriginalPackageDigest);

	auto RollBackFromDisk = [&](FString& OutRollbackError) -> bool
	{
		auto PreserveBackupAndFail = [&](const FString& Reason) -> bool
		{
			bDeleteBackupOnExit = false;
			OutRollbackError = FString::Printf(
				TEXT("%s; verified original backup preserved at '%s'"),
				*Reason,
				*BackupFilename);
			return false;
		};

		// Once PreEditChange/mutation has begun, always restore the original
		// bytes. A failed package-save return does not prove that no partial
		// write occurred, and Transaction.Cancel() only cancels the undo record.
		FString RestoreBytesError;
		if (!UE::MonolithMesh::Private::RestoreBackupBytesExact(
			BackupFilename,
			Target.PackageFilename,
			OriginalPackageSize,
			OriginalPackageDigest,
			RestoreBytesError))
		{
			return PreserveBackupAndFail(RestoreBytesError);
		}

		FExactStaticMeshAsset RolledBack;
		FString ReloadError;
		if (!ReloadPackageAndResolve(
			Target.PackageName,
			Target.ObjectPath,
			EReloadPackagesInteractionMode::AssumePositive,
			RolledBack,
			ReloadError))
		{
			return PreserveBackupAndFail(FString::Printf(TEXT("rollback package reload failed: %s"), *ReloadError));
		}

		FString PolicyError;
		if (!VerifyStaticMeshPostconditions(
			RolledBack,
			TargetSnapshot,
			TargetSnapshot.Lods,
			TargetSnapshot.LodSettings,
			TargetSnapshot.LightMapCoordinateIndex,
			TargetSnapshot.LightMapResolution,
			PolicyError))
		{
			return PreserveBackupAndFail(FString::Printf(
				TEXT("rollback reloaded but original mesh policies were not restored: %s"),
				*PolicyError));
		}

		const FSourceControlPreview RollbackSourceControl = ReadSourceControlPreview(Target.PackageFilename);
		FString SourceControlError;
		if (!ValidateSourceControlState(RollbackSourceControl, ExpectedChangelist, SourceControlError))
		{
			return PreserveBackupAndFail(FString::Printf(
				TEXT("rollback bytes/policies restored but source-control contract changed: %s"),
				*SourceControlError));
		}
		return true;
	};

	int64 PreMutationPackageSize = 0;
	FString PreMutationPackageDigest;
	if (!HashFileBytes(
		Target.PackageFilename,
		PreMutationPackageSize,
		PreMutationPackageDigest,
		Error)
		|| PreMutationPackageSize != OriginalPackageSize
		|| PreMutationPackageDigest != OriginalPackageDigest)
	{
		return PreparedError(FString::Printf(
			TEXT("target package bytes changed between planning/backup and mutation; no mutation attempted%s%s"),
			Error.IsEmpty() ? TEXT("") : TEXT(": "),
			*Error));
	}
	if (Source.Mesh->IsCompiling() || Source.Mesh->HasAnyDependenciesCompiling()
		|| Target.Mesh->IsCompiling() || Target.Mesh->HasAnyDependenciesCompiling())
	{
		return PreparedError(
			TEXT("asset compilation started after planning; no mutation attempted"));
	}
	if (Source.Package->IsDirty() || Target.Package->IsDirty())
	{
		return PreparedError(
			TEXT("source or target package became dirty after planning; no mutation attempted"));
	}
	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (AssetEditorSubsystem->FindEditorForAsset(Target.Mesh, /*bFocusIfOpen=*/false))
			{
				return PreparedError(
					TEXT("target StaticMesh editor opened after planning; no mutation attempted"));
			}
		}
	}
	if (!UE::MonolithMesh::Private::ValidateExecuteEditorState(
		GEditor != nullptr,
		GEditor && GEditor->IsPlaySessionInProgress(),
		GEditor && GEditor->PlayWorld != nullptr,
		GIsPlayInEditorWorld,
		GEditor && GEditor->bIsSimulatingInEditor,
		Error))
	{
		return PreparedError(FString::Printf(
			TEXT("editor session changed after planning; no mutation attempted: %s"),
			*Error));
	}
	FSourceControlPreview SourceControlImmediatelyBeforeMutation;
	if (!PrepareSourceControl(
		Target.PackageFilename,
		ExpectedChangelist,
		SourceControlImmediatelyBeforeMutation,
		Error))
	{
		return PreparedError(FString::Printf(
			TEXT("source-control state changed before mutation; no mutation attempted: %s"),
			*Error));
	}
	Result->SetObjectField(
		TEXT("source_control_immediately_before_mutation"),
		SourceControlPreviewToJson(SourceControlImmediatelyBeforeMutation));

	FString MutationError;
	{
		FScopedTransaction Transaction(LOCTEXT(
			"ReplaceStaticMeshGeometryInPlace",
			"Monolith: Replace StaticMesh Geometry In Place"));
		Target.Mesh->Modify();
		Target.Mesh->PreEditChange(nullptr);
		Target.Mesh->SetStaticMaterials(TargetSnapshot.StaticMaterials);

		TArray<const FMeshDescription*> MeshDescriptions;
		MeshDescriptions.Reserve(PlannedLods.Num());
		for (const FMeshDescriptionSnapshot& PlannedLod : PlannedLods)
		{
			MeshDescriptions.Add(&PlannedLod.Description);
		}

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		BuildParams.bMarkPackageDirty = true;
		BuildParams.bUseHashAsGuid = false;
		BuildParams.bBuildSimpleCollision = false;
		BuildParams.bCommitMeshDescription = true;
		BuildParams.bFastBuild = false;
		BuildParams.bAllowCpuAccess = Target.Mesh->bAllowCPUAccess;
		if (!Target.Mesh->BuildFromMeshDescriptions(MeshDescriptions, BuildParams))
		{
			MutationError = TEXT("BuildFromMeshDescriptions rejected the planned LOD geometry");
		}
		else
		{
			ApplyLodSettings(*Target.Mesh, PlannedLodSettings);
			Target.Mesh->SetStaticMaterials(TargetSnapshot.StaticMaterials);
			Target.Mesh->SetLightMapCoordinateIndex(PlannedLightMapCoordinateIndex);
			Target.Mesh->SetLightMapResolution(PlannedLightMapResolution);
			TArray<FText> BuildErrors;
			Target.Mesh->Build(/*bInSilent=*/true, &BuildErrors);
			if (BuildErrors.Num() > 0)
			{
				MutationError = FString::Printf(TEXT("StaticMesh build returned %d error(s): %s"),
					BuildErrors.Num(), *BuildErrors[0].ToString());
			}
			else if (HashAuthoredSimpleBodySetup(Target.Mesh->GetBodySetup()) != TargetSnapshot.CollisionDigest)
			{
				MutationError = TEXT("authored simple collision changed during build despite collision_policy=preserve_target_authored_simple");
			}
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (MutationError.IsEmpty()
			&& ConsumeStaticMeshReplacementTestFault(
				UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::AfterBuild))
		{
			MutationError = TEXT("injected failure after the real StaticMesh build");
		}
#endif

		if (MutationError.IsEmpty())
		{
			Target.Mesh->GetSectionInfoMap().CopyFrom(TargetSnapshot.SectionInfoMap);
			Target.Mesh->GetOriginalSectionInfoMap().CopyFrom(TargetSnapshot.OriginalSectionInfoMap);
			Target.Mesh->NeverStream = TargetSnapshot.bNeverStream;
			Target.Mesh->PostEditChange();
			if (!MeshSectionInfoMapEqualsExact(Target.Mesh->GetSectionInfoMap(), TargetSnapshot.SectionInfoMap)
				|| !MeshSectionInfoMapEqualsExact(Target.Mesh->GetOriginalSectionInfoMap(), TargetSnapshot.OriginalSectionInfoMap)
				|| Target.Mesh->NeverStream != TargetSnapshot.bNeverStream)
			{
				MutationError = TEXT("PostEditChange did not preserve the restored section maps or NeverStream policy");
			}
		}

		if (MutationError.IsEmpty())
		{
			Target.Mesh->MarkPackageDirty();
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			const bool bSaveSucceeded = UPackage::SavePackage(
				Target.Package,
				Target.Mesh,
				*Target.PackageFilename,
				SaveArgs);
			if (!bSaveSucceeded || Target.Package->IsDirty())
			{
				MutationError = TEXT("target save failed or package remained dirty after save");
			}
			else
			{
				FSourceControlPreview SourceControlAfterSave;
				if (!PrepareSourceControl(
					Target.PackageFilename,
					ExpectedChangelist,
					SourceControlAfterSave,
					Error))
				{
					MutationError = FString::Printf(
						TEXT("source-control contract changed after save: %s"),
						*Error);
				}
				Result->SetObjectField(
					TEXT("source_control_after_save"),
					SourceControlPreviewToJson(SourceControlAfterSave));
			}
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (MutationError.IsEmpty()
			&& ConsumeStaticMeshReplacementTestFault(
				UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::AfterSave))
		{
			MutationError = TEXT("injected failure after the real target package save");
		}
#endif

		if (!MutationError.IsEmpty())
		{
			Transaction.Cancel();
		}
	}

	if (!MutationError.IsEmpty())
	{
		FString RollbackError;
		const bool bRolledBack = RollBackFromDisk(RollbackError);
		return PreparedError(FString::Printf(
			TEXT("replacement failed: %s; rollback=%s%s"),
			*MutationError,
			bRolledBack ? TEXT("verified") : TEXT("FAILED"),
			RollbackError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *RollbackError)));
	}

	FExactStaticMeshAsset Reloaded;
	bool bInjectedReloadFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
	bInjectedReloadFailure = ConsumeStaticMeshReplacementTestFault(
		UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::BeforeSuccessReload);
	if (bInjectedReloadFailure)
	{
		Error = TEXT("injected failure before the success-path package reload");
	}
#endif
	if (bInjectedReloadFailure
		|| !ReloadPackageAndResolve(
		Target.PackageName,
		Target.ObjectPath,
		EReloadPackagesInteractionMode::AssumeNegative,
		Reloaded,
		Error)
		|| !VerifyStaticMeshPostconditions(
			Reloaded,
			TargetSnapshot,
			PlannedLods,
			PlannedLodSettings,
			PlannedLightMapCoordinateIndex,
			PlannedLightMapResolution,
			Error))
	{
		FString RollbackError;
		const bool bRolledBack = RollBackFromDisk(RollbackError);
		return PreparedError(FString::Printf(
			TEXT("save/reload/readback postcondition failed: %s; rollback=%s%s"),
			*Error,
			bRolledBack ? TEXT("verified") : TEXT("FAILED"),
			RollbackError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *RollbackError)));
	}

	FSourceControlPreview SourceControlAfterReload;
	if (!PrepareSourceControl(
		Target.PackageFilename,
		ExpectedChangelist,
		SourceControlAfterReload,
		Error))
	{
		FString RollbackError;
		const bool bRolledBack = RollBackFromDisk(RollbackError);
		return PreparedError(FString::Printf(
			TEXT("source-control contract changed after reload/readback: %s; rollback=%s%s"),
			*Error,
			bRolledBack ? TEXT("verified") : TEXT("FAILED"),
			RollbackError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *RollbackError)));
	}
	Result->SetObjectField(
		TEXT("source_control_after_reload"),
		SourceControlPreviewToJson(SourceControlAfterReload));

	int64 PackageSize = 0;
	FString PackageDigest;
	if (!HashFileBytes(Target.PackageFilename, PackageSize, PackageDigest, Error)
		|| PackageSize <= 0)
	{
		FString RollbackError;
		const bool bRolledBack = RollBackFromDisk(RollbackError);
		return PreparedError(FString::Printf(
			TEXT("package file postcondition failed%s%s; rollback=%s%s"),
			Error.IsEmpty() ? TEXT("") : TEXT(": "),
			*Error,
			bRolledBack ? TEXT("verified") : TEXT("FAILED"),
			RollbackError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *RollbackError)));
	}

	Result->SetStringField(TEXT("status"), TEXT("replaced_and_verified"));
	Result->SetBoolField(TEXT("logical_identity_preserved"), true);
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetBoolField(TEXT("reloaded"), true);
	Result->SetBoolField(TEXT("postconditions_verified"), true);
	Result->SetNumberField(TEXT("package_file_size"), static_cast<double>(PackageSize));
	Result->SetStringField(TEXT("package_file_md5"), PackageDigest);
	Result->SetArrayField(TEXT("lods_after_reload"), LodsToJson(PlannedLods));
	return FMonolithActionResult::Success(Result);
}

#undef LOCTEXT_NAMESPACE
