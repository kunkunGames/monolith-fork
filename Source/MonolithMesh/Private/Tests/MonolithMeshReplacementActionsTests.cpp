#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithMeshReplacementActions.h"
#include "MonolithObjectTraversal.h"
#include "MonolithToolRegistry.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "StaticMeshAttributes.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/GCObjectInfo.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

	namespace
{
	const FString ActionName = TEXT("replace_static_mesh_geometry_in_place");

	TSharedPtr<FJsonObject> MakeValidPolicyParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_asset_path"), TEXT("/ProjectMGH/Test/SM_Source.SM_Source"));
		Params->SetStringField(TEXT("target_asset_path"), TEXT("/Game/Test/SM_Target.SM_Target"));
		Params->SetStringField(TEXT("material_policy"), TEXT("preserve_target_by_name"));
		Params->SetStringField(TEXT("collision_policy"), TEXT("preserve_target_authored_simple"));
		Params->SetStringField(TEXT("uv_policy"), TEXT("copy_source_mesh_description"));
		Params->SetStringField(TEXT("lightmap_policy"), TEXT("preserve_target"));
		Params->SetStringField(TEXT("build_settings_policy"), TEXT("preserve_target"));
		Params->SetStringField(TEXT("lod_policy"), TEXT("copy_all_source_lods"));
		Params->SetStringField(TEXT("section_policy"), TEXT("preserve_target_exact_layout"));
		Params->SetStringField(TEXT("source_control_policy"), TEXT("require_checked_out"));
		Params->SetNumberField(TEXT("target_changelist"), 1203);
		return Params;
	}

	TSharedPtr<FJsonObject> FindReplacementSchema(FString* OutDescription = nullptr)
	{
		for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(TEXT("mesh")))
		{
			if (Info.Action == ActionName)
			{
				if (OutDescription)
				{
					*OutDescription = Info.Description;
				}
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}

	FString HashTestBytes(const TArray<uint8>& Bytes)
	{
		FMD5 Md5;
		if (!Bytes.IsEmpty())
		{
			Md5.Update(Bytes.GetData(), Bytes.Num());
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	struct FTestFileFingerprint
	{
		int64 Size = -1;
		FString Digest;
	};

	bool CaptureTestFileFingerprint(const FString& Filename, FTestFileFingerprint& OutFingerprint)
	{
		OutFingerprint.Size = IFileManager::Get().FileSize(*Filename);
		TArray<uint8> Bytes;
		if (OutFingerprint.Size < 0 || !FFileHelper::LoadFileToArray(Bytes, *Filename))
		{
			return false;
		}
		OutFingerprint.Digest = HashTestBytes(Bytes);
		return OutFingerprint.Size == Bytes.Num();
	}

	FString HashReflectedAuthoredProperties(UObject* Object)
	{
		if (!Object)
		{
			return TEXT("none");
		}

		FBufferArchive Bytes;
		FObjectAndNameAsStringProxyArchive Archive(Bytes, /*bInLoadIfFindFails=*/false);
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
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
				Property->ContainerPtrToValuePtr<void>(Object),
				nullptr);
		}
		return HashTestBytes(Bytes);
	}

	bool CaptureMeshDescriptionDigests(UStaticMesh* Mesh, TArray<FString>& OutDigests)
	{
		OutDigests.Reset();
		if (!Mesh || Mesh->GetNumSourceModels() <= 0)
		{
			return false;
		}

		OutDigests.Reserve(Mesh->GetNumSourceModels());
		for (int32 LodIndex = 0; LodIndex < Mesh->GetNumSourceModels(); ++LodIndex)
		{
			FMeshDescription Description;
			if (!Mesh->CloneMeshDescription(LodIndex, Description))
			{
				return false;
			}
			FBufferArchive Bytes;
			Description.Serialize(Bytes);
			OutDigests.Add(HashTestBytes(Bytes));
		}
		return true;
	}

	bool CaptureRemappedMeshDescriptionDigests(
		UStaticMesh* Mesh,
		const FString& TargetSlotName,
		TArray<FString>& OutDigests)
	{
		OutDigests.Reset();
		if (!Mesh || Mesh->GetNumSourceModels() <= 0 || TargetSlotName.IsEmpty())
		{
			return false;
		}

		OutDigests.Reserve(Mesh->GetNumSourceModels());
		for (int32 LodIndex = 0; LodIndex < Mesh->GetNumSourceModels(); ++LodIndex)
		{
			FMeshDescription Description;
			if (!Mesh->CloneMeshDescription(LodIndex, Description))
			{
				return false;
			}
			FStaticMeshAttributes Attributes(Description);
			TPolygonGroupAttributesRef<FName> SlotNames =
				Attributes.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID GroupId : Description.PolygonGroups().GetElementIDs())
			{
				SlotNames[GroupId] = FName(*TargetSlotName);
			}
			FBufferArchive Bytes;
			Description.Serialize(Bytes);
			OutDigests.Add(HashTestBytes(Bytes));
		}
		return true;
	}

	struct FStaticMeshMemoryFingerprint
	{
		UStaticMesh* Identity = nullptr;
		FString ObjectPath;
		FString PackageName;
		FString ObjectName;
		FString ClassPath;
		FString AuthoredPropertyDigest;
		FString BodySetupDigest;
		TArray<FString> MeshDescriptionDigests;
		bool bPackageDirty = false;
	};

	bool CaptureStaticMeshMemoryFingerprint(
		UStaticMesh* Mesh,
		FStaticMeshMemoryFingerprint& OutFingerprint)
	{
		if (!Mesh || !Mesh->GetOutermost())
		{
			return false;
		}
		OutFingerprint.Identity = Mesh;
		OutFingerprint.ObjectPath = Mesh->GetPathName();
		OutFingerprint.PackageName = Mesh->GetOutermost()->GetName();
		OutFingerprint.ObjectName = Mesh->GetName();
		OutFingerprint.ClassPath = Mesh->GetClass()->GetPathName();
		OutFingerprint.AuthoredPropertyDigest = HashReflectedAuthoredProperties(Mesh);
		OutFingerprint.BodySetupDigest =
			UE::MonolithMesh::Private::HashAuthoredBodySetupForVerification(Mesh->GetBodySetup());
		OutFingerprint.bPackageDirty = Mesh->GetOutermost()->IsDirty();
		return CaptureMeshDescriptionDigests(Mesh, OutFingerprint.MeshDescriptionDigests);
	}

	bool SaveFixtureMesh(UStaticMesh* Mesh, FString& OutFilename)
	{
		if (!Mesh
			|| !FPackageName::TryConvertLongPackageNameToFilename(
				Mesh->GetOutermost()->GetName(),
				OutFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilename), /*Tree=*/true);
		Mesh->GetOutermost()->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Mesh->GetOutermost(), Mesh, *OutFilename, SaveArgs);
	}

	FString ToggleFirstAlphabeticCharacterCase(const FString& Value)
	{
		FString Result = Value;
		for (TCHAR& Character : Result)
		{
			if (FChar::IsAlpha(Character))
			{
				Character = FChar::IsUpper(Character)
					? FChar::ToLower(Character)
					: FChar::ToUpper(Character);
				break;
			}
		}
		return Result;
	}

	bool IsExternalReferenceChain(const FReferenceChainSearch::FReferenceChain& Chain)
	{
		if (Chain.Num() <= 1)
		{
			return false;
		}

		const FReferenceChainSearch::FGraphNode* TargetNode = Chain.GetNode(0);
		const FReferenceChainSearch::FGraphNode* RootNode = Chain.GetRootNode();
		return TargetNode
			&& TargetNode->ObjectInfo
			&& RootNode
			&& RootNode->ObjectInfo
			&& !RootNode->ObjectInfo->IsIn(TargetNode->ObjectInfo);
	}

	bool ReadFirstPolygonGroupSlotName(UStaticMesh* Mesh, FString& OutSlotName)
	{
		FMeshDescription Description;
		if (!Mesh || !Mesh->CloneMeshDescription(0, Description)
			|| Description.PolygonGroups().Num() <= 0)
		{
			return false;
		}

		FStaticMeshAttributes Attributes(Description);
		TPolygonGroupAttributesRef<FName> SlotNames =
			Attributes.GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID GroupId : Description.PolygonGroups().GetElementIDs())
		{
			const FName SlotName = SlotNames[GroupId];
			if (!SlotName.IsNone())
			{
				OutSlotName = SlotName.ToString();
				return true;
			}
		}
		return false;
	}

	class FScopedStaticMeshReplacementFixture
	{
	public:
		explicit FScopedStaticMeshReplacementFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
			Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			MountRoot = FString::Printf(TEXT("/MonolithReplacementTests_%s/"), *Suffix);
			DiskRoot = FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("MonolithReplacementTests"),
				Suffix);
			FPaths::NormalizeDirectoryName(DiskRoot);
			DiskRoot += TEXT("/");
			SourcePackageName = MountRoot + TEXT("SM_Source_") + Suffix;
			TargetPackageName = MountRoot + TEXT("SM_Target_") + Suffix;
		}

		~FScopedStaticMeshReplacementFixture()
		{
			UE::MonolithMesh::Private::ResetStaticMeshReplacementTestHooks();
			if (bMounted && !bCleanupAttempted)
			{
				bCleanupAttempted = true;
				Cleanup();
			}
		}

		bool Initialize(
			const ECollisionTraceFlag TargetCollisionTraceFlag =
				ECollisionTraceFlag::CTF_UseSimpleAsComplex,
			const bool bClearSourcePolygonGroupSlotNames = false,
			const bool bClearTargetPolygonGroupSlotNames = false)
		{
			IFileManager::Get().MakeDirectory(*DiskRoot, /*Tree=*/true);
			FPackageName::RegisterMountPoint(MountRoot, DiskRoot);
			bMounted = true;

			UStaticMesh* SourceTemplate = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			UStaticMesh* TargetTemplate = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!SourceTemplate || !TargetTemplate)
			{
				Test.AddError(TEXT("engine sphere/cube replacement fixtures are unavailable"));
				return false;
			}

			Source = CreateFixture(SourceTemplate, SourcePackageName);
			Target = CreateFixture(TargetTemplate, TargetPackageName);
			if (!Source || !Target)
			{
				Test.AddError(TEXT("source or target StaticMesh fixture could not be created"));
				return false;
			}

			Target->CreateBodySetup();
			UBodySetup* TargetBodySetup = Target->GetBodySetup();
			if (!TargetBodySetup)
			{
				Test.AddError(TEXT("target fixture body setup is unavailable"));
				return false;
			}
			if (TargetBodySetup->AggGeom.GetElementCount() <= 0)
			{
				FKBoxElem Box;
				Box.X = 100.0f;
				Box.Y = 100.0f;
				Box.Z = 100.0f;
				TargetBodySetup->AggGeom.BoxElems.Add(Box);
			}
			TargetBodySetup->CollisionTraceFlag = TargetCollisionTraceFlag;
#if WITH_EDITORONLY_DATA
			Target->ComplexCollisionMesh = nullptr;
#endif
			if (bClearSourcePolygonGroupSlotNames)
			{
				FMeshDescription* SourceDescription = Source->GetMeshDescription(0);
				if (!SourceDescription || SourceDescription->PolygonGroups().Num() != 1)
				{
					Test.AddError(TEXT("single-slot source fixture requires exactly one polygon group"));
					return false;
				}
				FStaticMeshAttributes SourceAttributes(*SourceDescription);
				TPolygonGroupAttributesRef<FName> SourceSlotNames =
					SourceAttributes.GetPolygonGroupMaterialSlotNames();
				for (const FPolygonGroupID PolygonGroupId :
					SourceDescription->PolygonGroups().GetElementIDs())
				{
					SourceSlotNames[PolygonGroupId] = NAME_None;
				}
				Source->CommitMeshDescription(0);
			}
			if (bClearTargetPolygonGroupSlotNames)
			{
				FMeshDescription* TargetDescription = Target->GetMeshDescription(0);
				if (!TargetDescription || TargetDescription->PolygonGroups().Num() != 1)
				{
					Test.AddError(TEXT("single-slot target fixture requires exactly one polygon group"));
					return false;
				}
				FStaticMeshAttributes TargetAttributes(*TargetDescription);
				TPolygonGroupAttributesRef<FName> TargetSlotNames =
					TargetAttributes.GetPolygonGroupMaterialSlotNames();
				for (const FPolygonGroupID PolygonGroupId :
					TargetDescription->PolygonGroups().GetElementIDs())
				{
					TargetSlotNames[PolygonGroupId] = NAME_None;
				}
				Target->CommitMeshDescription(0);
			}

			if (!SaveFixtureMesh(Source, SourceFilename)
				|| !SaveFixtureMesh(Target, TargetFilename)
				|| !FPaths::FileExists(SourceFilename)
				|| !FPaths::FileExists(TargetFilename))
			{
				Test.AddError(TEXT("replacement fixture packages could not be saved"));
				return false;
			}
			Source->GetOutermost()->SetDirtyFlag(false);
			Target->GetOutermost()->SetDirtyFlag(false);
			if (bClearSourcePolygonGroupSlotNames)
			{
				FString UnexpectedSourceSlotName;
				if (ReadFirstPolygonGroupSlotName(Source, UnexpectedSourceSlotName))
				{
					Test.AddError(TEXT("single-slot source fixture still has polygon-group slot metadata"));
					return false;
				}
				SourceSlotName = TEXT("__unnamed_single_slot_fixture__");
			}
			else if (!ReadFirstPolygonGroupSlotName(Source, SourceSlotName))
			{
				Test.AddError(TEXT("replacement fixture source material-slot metadata is incomplete"));
				return false;
			}
			if (bClearTargetPolygonGroupSlotNames)
			{
				FString UnexpectedTargetSlotName;
				if (ReadFirstPolygonGroupSlotName(Target, UnexpectedTargetSlotName))
				{
					Test.AddError(TEXT("single-slot target fixture still has polygon-group slot metadata"));
					return false;
				}
			}
			if (Target->GetStaticMaterials().IsEmpty()
				|| Target->GetStaticMaterials()[0].MaterialSlotName.IsNone())
			{
				Test.AddError(TEXT("replacement fixture material-slot metadata is incomplete"));
				return false;
			}
			TargetSlotName = Target->GetStaticMaterials()[0].MaterialSlotName.ToString();

			TArray<FString> SourceGeometry;
			TArray<FString> TargetGeometry;
			if (!CaptureMeshDescriptionDigests(Source, SourceGeometry)
				|| !CaptureMeshDescriptionDigests(Target, TargetGeometry)
				|| SourceGeometry == TargetGeometry)
			{
				Test.AddError(TEXT("disposable donor geometry must differ from the target geometry"));
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeActionParams(const bool bDryRun) const
		{
			TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
			Params->SetStringField(TEXT("source_asset_path"), GetSourceObjectPath());
			Params->SetStringField(TEXT("target_asset_path"), GetTargetObjectPath());
			Params->SetStringField(TEXT("material_policy"), TEXT("explicit_remap"));
			TSharedPtr<FJsonObject> Remap = MakeShared<FJsonObject>();
			Remap->SetStringField(SourceSlotName, TargetSlotName);
			Params->SetObjectField(TEXT("material_remap"), Remap);
			Params->SetBoolField(TEXT("dry_run"), bDryRun);
			Params->SetBoolField(TEXT("confirm"), !bDryRun);
			return Params;
		}

		FString GetSourceObjectPath() const
		{
			return SourcePackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(SourcePackageName);
		}

		FString GetTargetObjectPath() const
		{
			return TargetPackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(TargetPackageName);
		}

		UStaticMesh* ResolveSource() const
		{
			return LoadObject<UStaticMesh>(nullptr, *GetSourceObjectPath());
		}

		UStaticMesh* ResolveTarget() const
		{
			return LoadObject<UStaticMesh>(nullptr, *GetTargetObjectPath());
		}

		void ConfigureExecute(
			const UE::MonolithMesh::Private::EStaticMeshReplacementTestFault Fault =
				UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::None,
			const int32 ObservedChangelist = 1203) const
		{
			UE::MonolithMesh::Private::ConfigureStaticMeshReplacementTestHooks(
				TargetFilename,
				ObservedChangelist,
				Fault);
		}

		bool CleanupNow()
		{
			if (bCleanupAttempted)
			{
				return true;
			}
			bCleanupAttempted = true;
			return Cleanup();
		}

		const FString& GetSourceFilename() const { return SourceFilename; }
		const FString& GetTargetFilename() const { return TargetFilename; }
		const FString& GetSourceSlotName() const { return SourceSlotName; }
		const FString& GetTargetSlotName() const { return TargetSlotName; }

	private:
		UStaticMesh* CreateFixture(UStaticMesh* Template, const FString& PackageName)
		{
			UPackage* Package = CreatePackage(*PackageName);
			const FString ObjectName = FPackageName::GetLongPackageAssetName(PackageName);
			UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Template, Package, *ObjectName);
			if (!Mesh)
			{
				return nullptr;
			}
			Mesh->SetFlags(RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Mesh);
			FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
			NaniteSettings.bEnabled = false;
			Mesh->SetNaniteSettings(NaniteSettings);
			if (Mesh->IsHiResMeshDescriptionValid())
			{
				Mesh->ClearHiResMeshDescription();
			}
			TArray<FText> BuildErrors;
			Mesh->Build(/*bInSilent=*/true, &BuildErrors);
			if (!BuildErrors.IsEmpty())
			{
				return nullptr;
			}
			return Mesh;
		}

		bool Cleanup()
		{
			UE::MonolithMesh::Private::ResetStaticMeshReplacementTestHooks();
			Source = nullptr;
			Target = nullptr;

			// RegisterMountPoint notifies the Asset Registry, which can still be
			// asynchronously gathering the fixture files when cleanup begins.  Make
			// that producer quiescent before publishing AssetDeleted and unloading;
			// otherwise its in-flight package work can race the unload and repopulate
			// the exact GUID-owned package after the GC pass.
			if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
			{
				AssetRegistry->WaitForCompletion();
			}

			TArray<UPackage*> PackagesToUnload;
			for (const FString& PackageName : { SourcePackageName, TargetPackageName })
			{
				if (UPackage* Package = FindPackage(nullptr, *PackageName))
				{
					if (UStaticMesh* Mesh = FindObject<UStaticMesh>(
						Package,
						*FPackageName::GetLongPackageAssetName(PackageName)))
					{
						FAssetRegistryModule::AssetDeleted(Mesh);
					}
					Package->SetDirtyFlag(false);
					PackagesToUnload.Add(Package);
				}
			}

			if (!PackagesToUnload.IsEmpty())
			{
				TArray<TWeakObjectPtr<UPackage>> PackageReachability;
				PackageReachability.Reserve(PackagesToUnload.Num());
				for (UPackage* Package : PackagesToUnload)
				{
					PackageReachability.Add(Package);
				}
				UPackageTools::FUnloadPackageParams UnloadParams(PackagesToUnload);
				UnloadParams.bUnloadDirtyPackages = false;
				UnloadParams.bResetTransBuffer = false;
				const bool bUnloadChangedPackages = UPackageTools::UnloadPackages(UnloadParams);

				// UPackageTools normally returns from two synchronous full GC passes.
				// Defensively quiesce any pre-existing incremental work before inspecting
				// physical fixture lifetime. This completes an already-started phase; it
				// does not begin another collection to force unload success.
				FinalizeIncrementalReachabilityAnalysis();
				if (IsIncrementalPurgePending())
				{
					IncrementalPurgeGarbage(/*bUseTimeLimit=*/false);
				}
				const bool bGCQuiescent =
					!IsIncrementalReachabilityAnalysisPending()
					&& !IsIncrementalUnhashPending()
					&& !IsIncrementalPurgePending();
				const auto GatherRemainingLiveObjects = [&PackageReachability]()
				{
					TArray<UObject*> LiveObjects;
					for (const TWeakObjectPtr<UPackage>& Package : PackageReachability)
					{
						UPackage* PackageShell = Package.Get(/*bEvenIfPendingKill=*/true);
						if (!PackageShell)
						{
							continue;
						}
						// This is a physical-lifetime check.  IsValid() intentionally rejects
						// logically-garbage objects and would therefore report a false cleanup
						// success before the UObject allocation is actually purged.
						LiveObjects.AddUnique(PackageShell);
						TArray<UObject*> RemainingObjects;
						MonolithObjectTraversal::GetObjectsWithPackage(
							PackageShell,
							RemainingObjects,
							true);
						for (UObject* Object : RemainingObjects)
						{
							if (Object)
							{
								LiveObjects.AddUnique(Object);
							}
						}
					}
					return LiveObjects;
				};

				TArray<UObject*> RemainingLiveObjects = GatherRemainingLiveObjects();
				if (!bUnloadChangedPackages || !bGCQuiescent
					|| !RemainingLiveObjects.IsEmpty()
					|| !UnloadParams.OutErrorMessage.IsEmpty())
				{
					TArray<FString> RemainingLiveObjectDetails;
					int32 ExternalReferenceChainCount = 0;
					int32 TotalReferenceChainCount = 0;
					if (!RemainingLiveObjects.IsEmpty())
					{
						// UE 5.8's non-direct ExternalOnly implementation removes every chain.
						// Search the fixture cohort once, then apply IsExternal's public-header
						// algorithm locally. UE 5.8 declares FReferenceChain::IsExternal without
						// exporting its implementation from CoreUObject, so module tests cannot
						// link a direct call even though the underlying node APIs are public.
						FReferenceChainSearch ReferenceSearch(
							RemainingLiveObjects,
							EReferenceChainSearchMode::Shortest
								| EReferenceChainSearchMode::FullChain);
						TotalReferenceChainCount = ReferenceSearch.GetReferenceChains().Num();
						for (const FReferenceChainSearch::FReferenceChain* Chain
							: ReferenceSearch.GetReferenceChains())
						{
							if (Chain && IsExternalReferenceChain(*Chain))
							{
								++ExternalReferenceChainCount;
							}
						}
					}
					RemainingLiveObjectDetails.Add(FString::Printf(
						TEXT("reference_chains=%d external_reference_chains=%d"),
						TotalReferenceChainCount,
						ExternalReferenceChainCount));
					for (UObject* Object : RemainingLiveObjects)
					{
						RemainingLiveObjectDetails.Add(FString::Printf(
							TEXT("%s flags=0x%08x internal=0x%08x rooted=%s ref_count=%d gc_keep=%s"),
							*Object->GetFullName(),
							static_cast<uint32>(Object->GetFlags()),
							static_cast<uint32>(Object->GetInternalFlags()),
							Object->IsRooted() ? TEXT("true") : TEXT("false"),
							Object->GetRefCount(),
							Object->HasAnyInternalFlags(EInternalObjectFlags_GarbageCollectionKeepFlags)
								? TEXT("true") : TEXT("false")));
					}
					for (const FString& PackageName : { SourcePackageName, TargetPackageName })
					{
						if (UPackage* Package = FindPackage(nullptr, *PackageName))
						{
							if (!IsValid(Package))
							{
								continue;
							}
							if (UStaticMesh* Mesh = FindObject<UStaticMesh>(
								Package,
								*FPackageName::GetLongPackageAssetName(PackageName)))
							{
								FAssetRegistryModule::AssetCreated(Mesh);
							}
						}
					}
					Test.AddError(FString::Printf(
						TEXT("non-modal replacement fixture unload failed; evidence remains at %s: %s; gc_quiescent=%s; live_objects=[%s]"),
						*DiskRoot,
						*UnloadParams.OutErrorMessage.ToString(),
						bGCQuiescent ? TEXT("true") : TEXT("false"),
						*FString::Join(RemainingLiveObjectDetails, TEXT(" | "))));
					return false;
				}
			}

			// RegisterMountPoint starts an asynchronous AssetRegistry gather for the
			// fixture directory.  Package unload can finish while that gather still
			// owns an OS read handle, so deleting immediately is a race on Windows.
			// Drain the registry before removing the mount and its backing files.
			if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
			{
				AssetRegistry->WaitForCompletion();
			}

			FPackageName::UnRegisterMountPoint(MountRoot, DiskRoot);
			bMounted = false;
			const bool bSourceDeleted = SourceFilename.IsEmpty()
				|| IFileManager::Get().Delete(*SourceFilename, false, true);
			const bool bTargetDeleted = TargetFilename.IsEmpty()
				|| IFileManager::Get().Delete(*TargetFilename, false, true);
			const bool bDirectoryDeleted = IFileManager::Get().DeleteDirectory(
				*DiskRoot, false, true);
			if (!bSourceDeleted || !bTargetDeleted || !bDirectoryDeleted)
			{
				Test.AddError(FString::Printf(
					TEXT("replacement fixture files could not be removed from %s"),
					*DiskRoot));
				return false;
			}
			return true;
		}

		FAutomationTestBase& Test;
		FString Suffix;
		FString MountRoot;
		FString DiskRoot;
		FString SourcePackageName;
		FString TargetPackageName;
		FString SourceFilename;
		FString TargetFilename;
		FString SourceSlotName;
		FString TargetSlotName;
		UStaticMesh* Source = nullptr;
		UStaticMesh* Target = nullptr;
		bool bMounted = false;
		bool bCleanupAttempted = false;
	};

	bool RequireCanonicalSourceControlPrepare(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Payload,
		const FString& Context,
		const FString& ExpectedStatus = TEXT("validated_exact_numbered_changelist"),
		const FString& ExpectedObservedChangelist = TEXT("1203"))
	{
		const TSharedPtr<FJsonObject>* Prepare = nullptr;
		if (!Payload.IsValid()
			|| !Payload->TryGetObjectField(TEXT("source_control_prepare"), Prepare)
			|| !Prepare
			|| !Prepare->IsValid())
		{
			Test.AddError(Context + TEXT(" has no canonical source_control_prepare object"));
			return false;
		}

		Test.TestEqual(
			*FString::Printf(TEXT("%s source-control mode"), *Context),
			(*Prepare)->GetStringField(TEXT("mode")),
			FString(TEXT("handler_owned_pre_mutation")));
		Test.TestEqual(
			*FString::Printf(TEXT("%s source-control status"), *Context),
			(*Prepare)->GetStringField(TEXT("status")),
			ExpectedStatus);
		Test.TestEqual(
			*FString::Printf(TEXT("%s exact changelist"), *Context),
			(*Prepare)->GetStringField(TEXT("expected_changelist")),
			FString(TEXT("1203")));

		const TSharedPtr<FJsonObject>* BeforeAction = nullptr;
		if (!(*Prepare)->TryGetObjectField(TEXT("before_action"), BeforeAction)
			|| !BeforeAction
			|| !BeforeAction->IsValid())
		{
			Test.AddError(Context + TEXT(" has no source-control before_action object"));
			return false;
		}
		Test.TestTrue(
			*FString::Printf(TEXT("%s exact fixture is tracked"), *Context),
			(*BeforeAction)->GetBoolField(TEXT("source_controlled")));
		Test.TestTrue(
			*FString::Printf(TEXT("%s exact fixture is checked out"), *Context),
			(*BeforeAction)->GetBoolField(TEXT("checked_out")));
		Test.TestTrue(
			*FString::Printf(TEXT("%s exact fixture revision is current"), *Context),
			(*BeforeAction)->GetBoolField(TEXT("current")));
		Test.TestEqual(
			*FString::Printf(TEXT("%s observed changelist"), *Context),
			(*BeforeAction)->GetStringField(TEXT("actual_changelist")),
			ExpectedObservedChangelist);
		return true;
	}

	bool TestLogicalMeshFingerprintEqual(
		FAutomationTestBase& Test,
		const FString& Context,
		const FStaticMeshMemoryFingerprint& Actual,
		const FStaticMeshMemoryFingerprint& Expected,
		const bool bRequireSamePointer)
	{
		if (bRequireSamePointer)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s UObject pointer"), *Context),
				Actual.Identity, Expected.Identity);
		}
		Test.TestEqual(*FString::Printf(TEXT("%s object path"), *Context),
			Actual.ObjectPath, Expected.ObjectPath);
		Test.TestEqual(*FString::Printf(TEXT("%s package"), *Context),
			Actual.PackageName, Expected.PackageName);
		Test.TestEqual(*FString::Printf(TEXT("%s object name"), *Context),
			Actual.ObjectName, Expected.ObjectName);
		Test.TestEqual(*FString::Printf(TEXT("%s class"), *Context),
			Actual.ClassPath, Expected.ClassPath);
		Test.TestEqual(*FString::Printf(TEXT("%s authored state"), *Context),
			Actual.AuthoredPropertyDigest, Expected.AuthoredPropertyDigest);
		Test.TestEqual(*FString::Printf(TEXT("%s body setup"), *Context),
			Actual.BodySetupDigest, Expected.BodySetupDigest);
		Test.TestEqual(*FString::Printf(TEXT("%s mesh descriptions"), *Context),
			Actual.MeshDescriptionDigests, Expected.MeshDescriptionDigests);
		Test.TestEqual(*FString::Printf(TEXT("%s dirty state"), *Context),
			Actual.bPackageDirty, Expected.bPackageDirty);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementRegistryContractTest,
	"Monolith.Registry.Mesh.StaticMeshReplacementContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementRegistryContractTest::RunTest(const FString& Parameters)
{
	FString Description;
	const TSharedPtr<FJsonObject> Schema = FindReplacementSchema(&Description);
	TestNotNull(TEXT("replacement action schema is registered"), Schema.Get());
	TestTrue(
		TEXT("catalog contract exposes the no-active-compilation precondition"),
		Description.Contains(TEXT("idle compilation")));
	TestTrue(TEXT("catalog contract exposes unsupported HiRes"), Description.Contains(TEXT("no-HiRes")));
	TestTrue(TEXT("catalog contract exposes unsupported Nanite"), Description.Contains(TEXT("non-Nanite")));
	TestTrue(TEXT("catalog contract exposes derived complex-collision rebuild"),
		Description.Contains(TEXT("SimpleAndComplex")));
	TestTrue(TEXT("catalog contract exposes explicit strict single-slot material policy"),
		Description.Contains(TEXT("strict single-slot")));
	TestTrue(TEXT("catalog contract exposes unsupported external complex collision"),
		Description.Contains(TEXT("external complex-collision")));
	TestTrue(TEXT("catalog contract exposes execute-only PIE/SIE gate"), Description.Contains(TEXT("PIE/SIE")));
	TestTrue(TEXT("catalog contract exposes exact numbered changelist"), Description.Contains(TEXT("exact numbered changelist")));
	TestTrue(TEXT("catalog contract exposes case-sensitive slots"), Description.Contains(TEXT("case-sensitive")));
	const FMonolithActionExecutionPolicy Policy = FMonolithToolRegistry::Get().GetActionExecutionPolicy(
		TEXT("mesh"), TEXT("replace_static_mesh_geometry_in_place"));
	TestEqual(TEXT("replacement handler owns its single transaction"),
		Policy.PolicyId, FString(TEXT("track_dirty_packages")));
	TestTrue(TEXT("replacement still participates in dirty-package tracking"),
		Policy.bDirtyPackageTracking);
	TestFalse(TEXT("central guard does not nest a second transaction"),
		Policy.bTransactionWrapping);
	FMonolithActionExecutionGuard& ExecutionGuard = FMonolithActionExecutionGuard::Get();
	FMonolithActionExecutionGuard::FExecutionScope GuardScope = ExecutionGuard.BeginAction(
		TEXT("mesh"),
		ActionName,
		MakeValidPolicyParams());
	TestTrue(TEXT("module registration selects handler-owned source control"),
		GuardScope.bHandlerOwnedSourceControlPrepare);
	TestFalse(TEXT("central guard never auto-prepares a coarse replacement target"),
		GuardScope.bSourceControlPrepareActive);
	ExecutionGuard.SetActionOutcome(
		GuardScope,
		/*bSuccess=*/false,
		/*ErrorCode=*/-1,
		nullptr,
		TEXT("focused registration probe"));
	ExecutionGuard.EndAction(GuardScope);
	if (!Schema.IsValid())
	{
		return false;
	}

	const TCHAR* EnumParams[] = {
		TEXT("material_policy"),
		TEXT("collision_policy"),
		TEXT("uv_policy"),
		TEXT("lightmap_policy"),
		TEXT("build_settings_policy"),
		TEXT("lod_policy"),
		TEXT("section_policy"),
		TEXT("source_control_policy")
	};
	for (const TCHAR* ParamName : EnumParams)
	{
		const TSharedPtr<FJsonObject>* ParamSchema = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		TestTrue(
			FString::Printf(TEXT("%s exposes an enforced enum"), ParamName),
			Schema->TryGetObjectField(ParamName, ParamSchema)
				&& ParamSchema && ParamSchema->IsValid()
				&& (*ParamSchema)->TryGetArrayField(TEXT("enum"), EnumValues)
				&& EnumValues && !EnumValues->IsEmpty());
	}

	const TSharedPtr<FJsonObject>* MaterialPolicySchema = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MaterialPolicyValues = nullptr;
	bool bHasStrictSingleSlotPolicy = false;
	if (Schema->TryGetObjectField(TEXT("material_policy"), MaterialPolicySchema)
		&& MaterialPolicySchema && MaterialPolicySchema->IsValid()
		&& (*MaterialPolicySchema)->TryGetArrayField(TEXT("enum"), MaterialPolicyValues)
		&& MaterialPolicyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *MaterialPolicyValues)
		{
			FString PolicyName;
			if (Value.IsValid() && Value->TryGetString(PolicyName)
				&& PolicyName == TEXT("preserve_target_single_slot"))
			{
				bHasStrictSingleSlotPolicy = true;
				break;
			}
		}
	}
	TestTrue(TEXT("material policy schema exposes preserve_target_single_slot"),
		bHasStrictSingleSlotPolicy);

	const TCHAR* RequiredParams[] = {
		TEXT("source_asset_path"),
		TEXT("target_asset_path"),
		TEXT("material_policy"),
		TEXT("collision_policy"),
		TEXT("uv_policy"),
		TEXT("lightmap_policy"),
		TEXT("build_settings_policy"),
		TEXT("lod_policy"),
		TEXT("section_policy"),
		TEXT("source_control_policy"),
		TEXT("target_changelist")
	};
	for (const TCHAR* ParamName : RequiredParams)
	{
		const TSharedPtr<FJsonObject>* ParamSchema = nullptr;
		TestTrue(
			FString::Printf(TEXT("%s schema exists"), ParamName),
			Schema->TryGetObjectField(ParamName, ParamSchema) && ParamSchema && ParamSchema->IsValid());
		if (ParamSchema && ParamSchema->IsValid())
		{
			TestTrue(
				FString::Printf(TEXT("%s is required"), ParamName),
				(*ParamSchema)->GetBoolField(TEXT("required")));
		}
	}

	const TSharedPtr<FJsonObject>* DryRunSchema = nullptr;
	const TSharedPtr<FJsonObject>* ConfirmSchema = nullptr;
	TestTrue(TEXT("dry_run schema exists"), Schema->TryGetObjectField(TEXT("dry_run"), DryRunSchema));
	TestTrue(TEXT("confirm schema exists"), Schema->TryGetObjectField(TEXT("confirm"), ConfirmSchema));
	if (DryRunSchema && ConfirmSchema)
	{
		TestEqual(TEXT("dry_run safe default"), (*DryRunSchema)->GetStringField(TEXT("default")), FString(TEXT("true")));
		TestEqual(TEXT("confirm safe default"), (*ConfirmSchema)->GetStringField(TEXT("default")), FString(TEXT("false")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementConfirmGuardTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementRequiresConfirmation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementConfirmGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"), ActionName, Params);
	TestFalse(TEXT("execute without confirmation fails"), Result.bSuccess);
	TestTrue(TEXT("confirmation error is explicit"), Result.ErrorMessage.Contains(TEXT("confirm=true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementExactPathGuardTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementRequiresExactPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementExactPathGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
	Params->SetStringField(TEXT("source_asset_path"), TEXT("ProjectMGH/Test/SM_Source.uasset"));
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"), ActionName, Params);
	TestFalse(TEXT("relative/filesystem-like path fails"), Result.bSuccess);
	TestTrue(TEXT("exact mounted path requirement is named"),
		Result.ErrorMessage.Contains(TEXT("exact mounted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementPolicyGuardTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementRejectsImplicitPolicies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementPolicyGuardTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
		Params->SetStringField(TEXT("collision_policy"), TEXT("copy_source"));
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("mesh"), ActionName, Params);
		TestFalse(TEXT("implicit collision copy fails"), Result.bSuccess);
		TestTrue(TEXT("preserve-only collision policy is named"),
			Result.ErrorMessage.Contains(TEXT("preserve_target")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
		Params->SetStringField(TEXT("material_policy"), TEXT("explicit_remap"));
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("mesh"), ActionName, Params);
		TestFalse(TEXT("explicit remap without map fails"), Result.bSuccess);
		TestTrue(TEXT("missing material_remap is named"),
			Result.ErrorMessage.Contains(TEXT("material_remap")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
		Params->SetStringField(TEXT("source_control_policy"), TEXT("checkout_if_needed"));
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("mesh"), ActionName, Params);
		TestFalse(TEXT("implicit/default-changelist checkout policy fails"), Result.bSuccess);
		TestTrue(TEXT("require_checked_out enum is named"),
			Result.ErrorMessage.Contains(TEXT("require_checked_out")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementChangelistGuardTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementRequiresNumberedChangelist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementChangelistGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeValidPolicyParams();
	Params->SetNumberField(TEXT("target_changelist"), 0);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"), ActionName, Params);
	TestFalse(TEXT("default/zero changelist fails"), Result.bSuccess);
	TestTrue(TEXT("numbered changelist lower bound is named"),
		Result.ErrorMessage.Contains(TEXT("target_changelist"))
			&& (Result.ErrorMessage.Contains(TEXT(">= 1"))
				|| Result.ErrorMessage.Contains(TEXT("positive numbered changelist"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementInternalGuardTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementInternalGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementInternalGuardTest::RunTest(const FString& Parameters)
{
	using namespace UE::MonolithMesh::Private;

	TSet<FString> UsedSlots = { TEXT("Wall"), TEXT("Trim") };
	TMap<FString, FString> ExactRemap = {
		{ TEXT("Wall"), TEXT("M_Wall") },
		{ TEXT("Trim"), TEXT("M_Trim") }
	};
	FString Error;
	TestTrue(TEXT("exact material remap key set passes"),
		ValidateExactMaterialRemapKeys(UsedSlots, ExactRemap, Error));

	TMap<FString, FString> AliasedRemap = {
		{ TEXT("wall"), TEXT("M_Wall") },
		{ TEXT("Trim"), TEXT("M_Trim") }
	};
	Error.Reset();
	TestFalse(TEXT("case alias does not substitute for exact source key"),
		ValidateExactMaterialRemapKeys(UsedSlots, AliasedRemap, Error));
	TestTrue(TEXT("aliased remap error is explicit"), Error.Contains(TEXT("aliased")));

	TMap<FString, FString> ExtraRemap = ExactRemap;
	ExtraRemap.Add(TEXT("Typo"), TEXT("M_Wall"));
	Error.Reset();
	TestFalse(TEXT("unused/typo remap key fails"),
		ValidateExactMaterialRemapKeys(UsedSlots, ExtraRemap, Error));
	TestTrue(TEXT("exact key count error is explicit"), Error.Contains(TEXT("exactly equal")));

	Error.Reset();
	TestFalse(TEXT("execute without editor fails"),
		ValidateExecuteEditorState(false, false, false, false, false, Error));
	TestTrue(TEXT("missing editor error is explicit"), Error.Contains(TEXT("live editor")));
	Error.Reset();
	TestFalse(TEXT("queued play session fails"),
		ValidateExecuteEditorState(true, true, false, false, false, Error));
	TestTrue(TEXT("PIE/SIE error is explicit"), Error.Contains(TEXT("PIE/SIE")));
	Error.Reset();
	TestFalse(TEXT("SIE flag fails"),
		ValidateExecuteEditorState(true, false, false, false, true, Error));
	Error.Reset();
	TestFalse(TEXT("play world availability fails"),
		ValidateExecuteEditorState(true, false, true, false, false, Error));
	Error.Reset();
	TestFalse(TEXT("global PIE world flag fails"),
		ValidateExecuteEditorState(true, false, false, true, false, Error));
	Error.Reset();
	TestTrue(TEXT("idle live editor passes"),
		ValidateExecuteEditorState(true, false, false, false, false, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementRollbackBytesTest,
	"Monolith.Persistence.MonolithMesh.StaticMeshReplacementRollbackBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementRollbackBytesTest::RunTest(const FString& Parameters)
{
	using namespace UE::MonolithMesh::Private;
	const FString BackupFilename = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(), TEXT("MonolithMeshRollbackBackup_"), TEXT(".bin"));
	const FString TargetFilename = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(), TEXT("MonolithMeshRollbackTarget_"), TEXT(".bin"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*BackupFilename, false, true);
		IFileManager::Get().Delete(*TargetFilename, false, true);
	};

	const TArray<uint8> OriginalBytes = { 0x4d, 0x47, 0x48, 0x00, 0x7f, 0xa5 };
	const TArray<uint8> PartialWriteBytes = { 0xde, 0xad, 0xbe, 0xef };
	TestTrue(TEXT("fixture backup written"), FFileHelper::SaveArrayToFile(OriginalBytes, *BackupFilename));
	TestTrue(TEXT("fixture partial target written"), FFileHelper::SaveArrayToFile(PartialWriteBytes, *TargetFilename));

	FString Error;
	TestTrue(TEXT("backup bytes restore and MD5 verification pass"),
		RestoreBackupBytesExact(
			BackupFilename,
			TargetFilename,
			OriginalBytes.Num(),
			HashTestBytes(OriginalBytes),
			Error));
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("restored target readable"), FFileHelper::LoadFileToArray(RestoredBytes, *TargetFilename));
	TestEqual(TEXT("restored target is byte-for-byte original"), RestoredBytes, OriginalBytes);

	Error.Reset();
	TestFalse(TEXT("invalid restore destination fails closed"),
		RestoreBackupBytesExact(
			BackupFilename,
			FPaths::ProjectIntermediateDir(),
			OriginalBytes.Num(),
			HashTestBytes(OriginalBytes),
			Error));
	TestTrue(TEXT("failed restore does not consume the only backup"),
		IFileManager::Get().FileExists(*BackupFilename));

	Error.Reset();
	TestFalse(TEXT("missing backup fails closed"),
		RestoreBackupBytesExact(
			BackupFilename + TEXT(".missing"),
			TargetFilename,
			OriginalBytes.Num(),
			HashTestBytes(OriginalBytes),
			Error));
	TestTrue(TEXT("missing backup failure is explicit"), !Error.IsEmpty());

	Error.Reset();
	TestFalse(TEXT("restored byte-count mismatch fails verification"),
		RestoreBackupBytesExact(
			BackupFilename,
			TargetFilename,
			OriginalBytes.Num() + 1,
			HashTestBytes(OriginalBytes),
			Error));
	TestTrue(TEXT("size mismatch is explicit"), Error.Contains(TEXT("size/MD5")));
	TestTrue(TEXT("size mismatch keeps rollback backup"),
		IFileManager::Get().FileExists(*BackupFilename));

	Error.Reset();
	TestFalse(TEXT("restored digest mismatch fails verification"),
		RestoreBackupBytesExact(
			BackupFilename,
			TargetFilename,
			OriginalBytes.Num(),
			TEXT("00000000000000000000000000000000"),
			Error));
	TestTrue(TEXT("digest mismatch is explicit"), Error.Contains(TEXT("size/MD5")));
	TestTrue(TEXT("digest mismatch keeps rollback backup"),
		IFileManager::Get().FileExists(*BackupFilename));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementDryRunSuccessTest,
	"Monolith.Workflow.MonolithMesh.StaticMeshReplacementDryRunSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementDryRunSuccessTest::RunTest(const FString& Parameters)
{
	FScopedStaticMeshReplacementFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	UStaticMesh* Source = Fixture.ResolveSource();
	UStaticMesh* Target = Fixture.ResolveTarget();
	TestNotNull(TEXT("source fixture resolves before dry-run"), Source);
	TestNotNull(TEXT("target fixture resolves before dry-run"), Target);
	if (!Source || !Target)
	{
		return false;
	}

	FTestFileFingerprint SourceFileBefore;
	FTestFileFingerprint TargetFileBefore;
	FStaticMeshMemoryFingerprint SourceMemoryBefore;
	FStaticMeshMemoryFingerprint TargetMemoryBefore;
	TestTrue(TEXT("source file fingerprint captured before dry-run"),
		CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileBefore));
	TestTrue(TEXT("target file fingerprint captured before dry-run"),
		CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileBefore));
	TestTrue(TEXT("source authored memory fingerprint captured before dry-run"),
		CaptureStaticMeshMemoryFingerprint(Source, SourceMemoryBefore));
	TestTrue(TEXT("target authored memory fingerprint captured before dry-run"),
		CaptureStaticMeshMemoryFingerprint(Target, TargetMemoryBefore));

	FString SourceSlot;
	TestTrue(TEXT("source fixture exposes explicit polygon-group slot metadata"),
		ReadFirstPolygonGroupSlotName(Source, SourceSlot));
	TestFalse(TEXT("target fixture exposes at least one material slot"),
		Target->GetStaticMaterials().IsEmpty());
	if (!SourceSlot.IsEmpty() && !Target->GetStaticMaterials().IsEmpty())
	{
		const FString CanonicalTargetSlot =
			Target->GetStaticMaterials()[0].MaterialSlotName.ToString();
		const FString WrongCaseTargetSlot =
			ToggleFirstAlphabeticCharacterCase(CanonicalTargetSlot);
		TSharedPtr<FJsonObject> WrongCaseParams = Fixture.MakeActionParams(/*bDryRun=*/true);
		WrongCaseParams->SetStringField(TEXT("material_policy"), TEXT("explicit_remap"));
		TSharedPtr<FJsonObject> Remap = MakeShared<FJsonObject>();
		Remap->SetStringField(SourceSlot, WrongCaseTargetSlot);
		WrongCaseParams->SetObjectField(TEXT("material_remap"), Remap);
		const FMonolithActionResult WrongCaseResult =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), ActionName, WrongCaseParams);
		TestFalse(TEXT("wrong-case target material slot spelling is rejected"),
			WrongCaseResult.bSuccess);
		TestTrue(TEXT("wrong-case failure names the exact missing target slot"),
			WrongCaseResult.ErrorMessage.Contains(TEXT("missing target slot")));
	}

	TSharedPtr<FJsonObject> Params = Fixture.MakeActionParams(/*bDryRun=*/true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"), ActionName, Params);
	TestTrue(
		FString::Printf(TEXT("validated dry-run succeeds: %s"), *Result.ErrorMessage),
		Result.bSuccess);
	TestNotNull(TEXT("successful dry-run returns a result payload"), Result.Result.Get());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("dry-run status"),
		Result.Result->GetStringField(TEXT("status")),
		FString(TEXT("validated_plan")));
	TestTrue(TEXT("dry-run reports mutation plan"), Result.Result->GetBoolField(TEXT("would_mutate")));
	TestEqual(
		TEXT("dry-run exposes expected changelist"),
		static_cast<int32>(Result.Result->GetNumberField(TEXT("expected_target_changelist"))),
		1203);
	TestFalse(TEXT("untracked Intermediate target is not source-control ready"),
		Result.Result->GetBoolField(TEXT("source_control_ready")));
	TestFalse(TEXT("source-control readiness failure is explicit"),
		Result.Result->GetStringField(TEXT("source_control_readiness_error")).IsEmpty());

	FTestFileFingerprint SourceFileAfter;
	FTestFileFingerprint TargetFileAfter;
	FStaticMeshMemoryFingerprint SourceMemoryAfter;
	FStaticMeshMemoryFingerprint TargetMemoryAfter;
	TestTrue(TEXT("source file fingerprint captured after dry-run"),
		CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileAfter));
	TestTrue(TEXT("target file fingerprint captured after dry-run"),
		CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileAfter));
	TestTrue(TEXT("source authored memory fingerprint captured after dry-run"),
		CaptureStaticMeshMemoryFingerprint(Source, SourceMemoryAfter));
	TestTrue(TEXT("target authored memory fingerprint captured after dry-run"),
		CaptureStaticMeshMemoryFingerprint(Target, TargetMemoryAfter));

	TestEqual(TEXT("source package file size is unchanged"), SourceFileAfter.Size, SourceFileBefore.Size);
	TestEqual(TEXT("source package file MD5 is unchanged"), SourceFileAfter.Digest, SourceFileBefore.Digest);
	TestEqual(TEXT("target package file size is unchanged"), TargetFileAfter.Size, TargetFileBefore.Size);
	TestEqual(TEXT("target package file MD5 is unchanged"), TargetFileAfter.Digest, TargetFileBefore.Digest);
	TestEqual(TEXT("source UObject identity is unchanged"), SourceMemoryAfter.Identity, SourceMemoryBefore.Identity);
	TestEqual(TEXT("source object path is unchanged"), SourceMemoryAfter.ObjectPath, SourceMemoryBefore.ObjectPath);
	TestEqual(TEXT("source package identity is unchanged"), SourceMemoryAfter.PackageName, SourceMemoryBefore.PackageName);
	TestEqual(TEXT("source object name is unchanged"), SourceMemoryAfter.ObjectName, SourceMemoryBefore.ObjectName);
	TestEqual(TEXT("source class identity is unchanged"), SourceMemoryAfter.ClassPath, SourceMemoryBefore.ClassPath);
	TestEqual(TEXT("source authored reflected state is unchanged"),
		SourceMemoryAfter.AuthoredPropertyDigest, SourceMemoryBefore.AuthoredPropertyDigest);
	TestEqual(TEXT("source authored BodySetup collision is unchanged"),
		SourceMemoryAfter.BodySetupDigest, SourceMemoryBefore.BodySetupDigest);
	TestEqual(TEXT("source LOD MeshDescriptions are unchanged"),
		SourceMemoryAfter.MeshDescriptionDigests, SourceMemoryBefore.MeshDescriptionDigests);
	TestEqual(TEXT("source dirty state is unchanged"), SourceMemoryAfter.bPackageDirty, SourceMemoryBefore.bPackageDirty);
	TestEqual(TEXT("target UObject identity is unchanged"), TargetMemoryAfter.Identity, TargetMemoryBefore.Identity);
	TestEqual(TEXT("target object path is unchanged"), TargetMemoryAfter.ObjectPath, TargetMemoryBefore.ObjectPath);
	TestEqual(TEXT("target package identity is unchanged"), TargetMemoryAfter.PackageName, TargetMemoryBefore.PackageName);
	TestEqual(TEXT("target object name is unchanged"), TargetMemoryAfter.ObjectName, TargetMemoryBefore.ObjectName);
	TestEqual(TEXT("target class identity is unchanged"), TargetMemoryAfter.ClassPath, TargetMemoryBefore.ClassPath);
	TestEqual(TEXT("target authored reflected state is unchanged"),
		TargetMemoryAfter.AuthoredPropertyDigest, TargetMemoryBefore.AuthoredPropertyDigest);
	TestEqual(TEXT("target authored BodySetup collision is unchanged"),
		TargetMemoryAfter.BodySetupDigest, TargetMemoryBefore.BodySetupDigest);
	TestEqual(TEXT("target LOD MeshDescriptions are unchanged"),
		TargetMemoryAfter.MeshDescriptionDigests, TargetMemoryBefore.MeshDescriptionDigests);
	TestEqual(TEXT("target dirty state is unchanged"), TargetMemoryAfter.bPackageDirty, TargetMemoryBefore.bPackageDirty);

	// Let the fixture destructor perform the non-modal package unload after the
	// action result, JSON params, UObject locals, and memory fingerprints have
	// left scope.  Unloading here keeps those test-owned values alive across the
	// GC performed by UPackageTools and can retain an otherwise disposable
	// package even though the production dry-run itself holds no references.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementSourceControlInvariantTest,
	"Monolith.ParamGuard.MonolithMesh.StaticMeshReplacementSourceControlInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementSourceControlInvariantTest::RunTest(const FString& Parameters)
{
	FScopedStaticMeshReplacementFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	UStaticMesh* TargetBefore = Fixture.ResolveTarget();
	FTestFileFingerprint TargetFileBefore;
	FStaticMeshMemoryFingerprint TargetMemoryBefore;
	if (!TargetBefore
		|| !CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileBefore)
		|| !CaptureStaticMeshMemoryFingerprint(TargetBefore, TargetMemoryBefore))
	{
		AddError(TEXT("source-control mismatch baseline capture failed"));
		return false;
	}

	Fixture.ConfigureExecute(
		UE::MonolithMesh::Private::EStaticMeshReplacementTestFault::None,
		/*ObservedChangelist=*/1204);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		ActionName,
		Fixture.MakeActionParams(/*bDryRun=*/false));
	TestFalse(TEXT("mismatched observed changelist rejects execute"), Result.bSuccess);
	TestTrue(TEXT("mismatched changelist error is explicit"),
		Result.ErrorMessage.Contains(TEXT("changelist mismatch")));
	TestNotNull(TEXT("prepare failure returns structured error data"), Result.ErrorData.Get());
	if (Result.ErrorData.IsValid())
	{
		RequireCanonicalSourceControlPrepare(
			*this,
			Result.ErrorData,
			TEXT("source-control mismatch"),
			TEXT("failed"),
			TEXT("1204"));
	}
	TestEqual(TEXT("mismatch is observed at planning and prepare only"),
		UE::MonolithMesh::Private::GetStaticMeshReplacementSourceControlReadCountForTests(),
		2);

	FTestFileFingerprint TargetFileAfter;
	FStaticMeshMemoryFingerprint TargetMemoryAfter;
	TestTrue(TEXT("mismatch target file readback captured"),
		CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileAfter));
	TestTrue(TEXT("mismatch target memory readback captured"),
		CaptureStaticMeshMemoryFingerprint(Fixture.ResolveTarget(), TargetMemoryAfter));
	TestEqual(TEXT("mismatch leaves target size unchanged"),
		TargetFileAfter.Size, TargetFileBefore.Size);
	TestEqual(TEXT("mismatch leaves target MD5 unchanged"),
		TargetFileAfter.Digest, TargetFileBefore.Digest);
	TestLogicalMeshFingerprintEqual(
		*this,
		TEXT("source-control mismatch target"),
		TargetMemoryAfter,
		TargetMemoryBefore,
		/*bRequireSamePointer=*/true);
	TestTrue(TEXT("source-control mismatch fixture cleanup succeeds"),
		Fixture.CleanupNow());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementExecuteSuccessTest,
	"Monolith.Workflow.MonolithMesh.StaticMeshReplacementExecuteSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementExecuteSuccessTest::RunTest(const FString& Parameters)
{
	FScopedStaticMeshReplacementFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	UStaticMesh* SourceBefore = Fixture.ResolveSource();
	UStaticMesh* TargetBefore = Fixture.ResolveTarget();
	if (!SourceBefore || !TargetBefore)
	{
		AddError(TEXT("execute fixture objects do not resolve"));
		return false;
	}

	FTestFileFingerprint SourceFileBefore;
	FTestFileFingerprint TargetFileBefore;
	FStaticMeshMemoryFingerprint SourceMemoryBefore;
	FStaticMeshMemoryFingerprint TargetMemoryBefore;
	TArray<FString> PlannedSourceGeometryDigests;
	TestTrue(TEXT("execute source file fingerprint captured"),
		CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileBefore));
	TestTrue(TEXT("execute target file fingerprint captured"),
		CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileBefore));
	TestTrue(TEXT("execute source memory fingerprint captured"),
		CaptureStaticMeshMemoryFingerprint(SourceBefore, SourceMemoryBefore));
	TestTrue(TEXT("execute target memory fingerprint captured"),
		CaptureStaticMeshMemoryFingerprint(TargetBefore, TargetMemoryBefore));
	TestTrue(TEXT("execute planned remapped donor geometry captured"),
		CaptureRemappedMeshDescriptionDigests(
			SourceBefore,
			Fixture.GetTargetSlotName(),
			PlannedSourceGeometryDigests));

	Fixture.ConfigureExecute();
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		ActionName,
		Fixture.MakeActionParams(/*bDryRun=*/false));
	TestTrue(
		FString::Printf(TEXT("disposable execute succeeds: %s"), *Result.ErrorMessage),
		Result.bSuccess);
	TestNotNull(TEXT("execute returns a result payload"), Result.Result.Get());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("execute canonical status"),
		Result.Result->GetStringField(TEXT("status")),
		FString(TEXT("replaced_and_verified")));
	TestTrue(TEXT("execute reports save"), Result.Result->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("execute reports reload"), Result.Result->GetBoolField(TEXT("reloaded")));
	TestTrue(TEXT("execute reports verified postconditions"),
		Result.Result->GetBoolField(TEXT("postconditions_verified")));
	TestFalse(TEXT("legacy source_control_prepared alias is absent"),
		Result.Result->HasField(TEXT("source_control_prepared")));
	RequireCanonicalSourceControlPrepare(*this, Result.Result, TEXT("execute success"));

	const TSharedPtr<FJsonObject>* AfterReloadSourceControl = nullptr;
	TestTrue(TEXT("execute exposes post-reload source-control readback"),
		Result.Result->TryGetObjectField(
			TEXT("source_control_after_reload"),
			AfterReloadSourceControl)
			&& AfterReloadSourceControl
			&& AfterReloadSourceControl->IsValid());
	if (AfterReloadSourceControl && AfterReloadSourceControl->IsValid())
	{
		TestEqual(TEXT("post-reload source-control changelist remains exact"),
			(*AfterReloadSourceControl)->GetStringField(TEXT("actual_changelist")),
			FString(TEXT("1203")));
		TestTrue(TEXT("post-reload source-control target remains checked out"),
			(*AfterReloadSourceControl)->GetBoolField(TEXT("checked_out")));
	}
	TestEqual(TEXT("execute validates source control at every persistence boundary"),
		UE::MonolithMesh::Private::GetStaticMeshReplacementSourceControlReadCountForTests(),
		5);

	UStaticMesh* SourceAfter = Fixture.ResolveSource();
	UStaticMesh* TargetAfter = Fixture.ResolveTarget();
	FTestFileFingerprint SourceFileAfter;
	FTestFileFingerprint TargetFileAfter;
	FStaticMeshMemoryFingerprint SourceMemoryAfter;
	FStaticMeshMemoryFingerprint TargetMemoryAfter;
	TestTrue(TEXT("execute source file readback captured"),
		CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileAfter));
	TestTrue(TEXT("execute target file readback captured"),
		CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileAfter));
	TestTrue(TEXT("execute source memory readback captured"),
		CaptureStaticMeshMemoryFingerprint(SourceAfter, SourceMemoryAfter));
	TestTrue(TEXT("execute target memory readback captured"),
		CaptureStaticMeshMemoryFingerprint(TargetAfter, TargetMemoryAfter));
	TestEqual(TEXT("execute leaves donor package bytes unchanged"),
		SourceFileAfter.Digest, SourceFileBefore.Digest);
	TestLogicalMeshFingerprintEqual(
		*this,
		TEXT("execute donor"),
		SourceMemoryAfter,
		SourceMemoryBefore,
		/*bRequireSamePointer=*/true);
	TestNotEqual(TEXT("execute changes target package bytes"),
		TargetFileAfter.Digest, TargetFileBefore.Digest);
	TestEqual(TEXT("execute copies donor MeshDescription through reload"),
		TargetMemoryAfter.MeshDescriptionDigests,
		PlannedSourceGeometryDigests);
	TestTrue(TEXT("execute target geometry differs from its original geometry"),
		TargetMemoryAfter.MeshDescriptionDigests
			!= TargetMemoryBefore.MeshDescriptionDigests);
	TestEqual(TEXT("execute preserves target collision"),
		TargetMemoryAfter.BodySetupDigest,
		TargetMemoryBefore.BodySetupDigest);
	TestEqual(TEXT("execute preserves target object path"),
		TargetMemoryAfter.ObjectPath,
		TargetMemoryBefore.ObjectPath);
	TestEqual(TEXT("execute preserves target package"),
		TargetMemoryAfter.PackageName,
		TargetMemoryBefore.PackageName);
	TestFalse(TEXT("execute leaves reloaded target clean"), TargetMemoryAfter.bPackageDirty);

	TestTrue(TEXT("execute fixture cleanup succeeds without resetting global undo"),
		Fixture.CleanupNow());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementSimpleAndComplexExecuteTest,
	"Monolith.Workflow.MonolithMesh.StaticMeshReplacementSimpleAndComplexExecute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementSimpleAndComplexExecuteTest::RunTest(const FString& Parameters)
{
	FScopedStaticMeshReplacementFixture Fixture(*this);
	if (!Fixture.Initialize(
		ECollisionTraceFlag::CTF_UseSimpleAndComplex,
		/*bClearSourcePolygonGroupSlotNames=*/true,
		/*bClearTargetPolygonGroupSlotNames=*/true))
	{
		return false;
	}

	UStaticMesh* SourceBefore = Fixture.ResolveSource();
	UStaticMesh* TargetBefore = Fixture.ResolveTarget();
	if (!SourceBefore || !TargetBefore || !TargetBefore->GetBodySetup())
	{
		AddError(TEXT("simple-and-complex fixture objects do not resolve"));
		return false;
	}

	const FString TargetCollisionBefore =
		UE::MonolithMesh::Private::HashAuthoredBodySetupForVerification(TargetBefore->GetBodySetup());
	TArray<FString> PlannedSourceGeometryDigests;
	TestTrue(TEXT("simple-and-complex planned donor geometry captured"),
		CaptureRemappedMeshDescriptionDigests(
			SourceBefore,
			Fixture.GetTargetSlotName(),
			PlannedSourceGeometryDigests));

	Fixture.ConfigureExecute();
	TSharedPtr<FJsonObject> ActionParams = Fixture.MakeActionParams(/*bDryRun=*/false);
	ActionParams->SetStringField(TEXT("material_policy"), TEXT("preserve_target_single_slot"));
	ActionParams->RemoveField(TEXT("material_remap"));
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		ActionName,
		ActionParams);
	TestTrue(
		FString::Printf(TEXT("simple-and-complex execute succeeds: %s"), *Result.ErrorMessage),
		Result.bSuccess);
	if (!Result.bSuccess)
	{
		return false;
	}

	UStaticMesh* TargetAfter = Fixture.ResolveTarget();
	TestNotNull(TEXT("simple-and-complex target reload resolves"), TargetAfter);
	if (!TargetAfter || !TargetAfter->GetBodySetup())
	{
		return false;
	}
	TestEqual(
		TEXT("simple-and-complex trace semantics survive rebuild and reload"),
		TargetAfter->GetBodySetup()->GetCollisionTraceFlag(),
		ECollisionTraceFlag::CTF_UseSimpleAndComplex);
	TestEqual(
		TEXT("simple-and-complex authored BodySetup survives rebuild and reload"),
		UE::MonolithMesh::Private::HashAuthoredBodySetupForVerification(TargetAfter->GetBodySetup()),
		TargetCollisionBefore);
	TArray<FString> TargetGeometryAfter;
	TestTrue(TEXT("simple-and-complex target geometry readback captured"),
		CaptureMeshDescriptionDigests(TargetAfter, TargetGeometryAfter));
	TestEqual(
		TEXT("simple-and-complex target receives the exact remapped donor geometry"),
		TargetGeometryAfter,
		PlannedSourceGeometryDigests);

	TestTrue(TEXT("simple-and-complex fixture cleanup succeeds"), Fixture.CleanupNow());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshReplacementFaultRollbackTest,
	"Monolith.Persistence.MonolithMesh.StaticMeshReplacementFaultRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshReplacementFaultRollbackTest::RunTest(const FString& Parameters)
{
	using UE::MonolithMesh::Private::EStaticMeshReplacementTestFault;
	struct FFaultCase
	{
		EStaticMeshReplacementTestFault Fault;
		const TCHAR* Name;
		int32 ExpectedSourceControlReads;
	};
	const FFaultCase FaultCases[] = {
		{ EStaticMeshReplacementTestFault::AfterBuild, TEXT("after build"), 4 },
		{ EStaticMeshReplacementTestFault::AfterSave, TEXT("after save"), 5 },
		{ EStaticMeshReplacementTestFault::BeforeSuccessReload, TEXT("before reload"), 5 }
	};

	for (const FFaultCase& FaultCase : FaultCases)
	{
		FScopedStaticMeshReplacementFixture Fixture(*this);
		if (!Fixture.Initialize())
		{
			return false;
		}
		UStaticMesh* SourceBefore = Fixture.ResolveSource();
		UStaticMesh* TargetBefore = Fixture.ResolveTarget();
		FTestFileFingerprint SourceFileBefore;
		FTestFileFingerprint TargetFileBefore;
		FStaticMeshMemoryFingerprint SourceMemoryBefore;
		FStaticMeshMemoryFingerprint TargetMemoryBefore;
		if (!SourceBefore || !TargetBefore
			|| !CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileBefore)
			|| !CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileBefore)
			|| !CaptureStaticMeshMemoryFingerprint(SourceBefore, SourceMemoryBefore)
			|| !CaptureStaticMeshMemoryFingerprint(TargetBefore, TargetMemoryBefore))
		{
			AddError(FString::Printf(TEXT("%s rollback baseline capture failed"), FaultCase.Name));
			return false;
		}

		Fixture.ConfigureExecute(FaultCase.Fault);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("mesh"),
			ActionName,
			Fixture.MakeActionParams(/*bDryRun=*/false));
		TestFalse(
			FString::Printf(TEXT("%s injected execute fails"), FaultCase.Name),
			Result.bSuccess);
		TestTrue(
			FString::Printf(TEXT("%s failure names injection"), FaultCase.Name),
			Result.ErrorMessage.Contains(TEXT("injected failure")));
		TestTrue(
			FString::Printf(TEXT("%s failure reports verified rollback"), FaultCase.Name),
			Result.ErrorMessage.Contains(TEXT("rollback=verified")));
		TestNotNull(
			FString::Printf(TEXT("%s failure preserves structured error data"), FaultCase.Name),
			Result.ErrorData.Get());
		if (Result.ErrorData.IsValid())
		{
			RequireCanonicalSourceControlPrepare(
				*this,
				Result.ErrorData,
				FString::Printf(TEXT("%s rollback error"), FaultCase.Name));
		}
		TestEqual(
			FString::Printf(TEXT("%s revalidates source control through rollback"), FaultCase.Name),
			UE::MonolithMesh::Private::GetStaticMeshReplacementSourceControlReadCountForTests(),
			FaultCase.ExpectedSourceControlReads);

		UStaticMesh* SourceAfter = Fixture.ResolveSource();
		UStaticMesh* TargetAfter = Fixture.ResolveTarget();
		FTestFileFingerprint SourceFileAfter;
		FTestFileFingerprint TargetFileAfter;
		FStaticMeshMemoryFingerprint SourceMemoryAfter;
		FStaticMeshMemoryFingerprint TargetMemoryAfter;
		TestTrue(FString::Printf(TEXT("%s source file readback"), FaultCase.Name),
			CaptureTestFileFingerprint(Fixture.GetSourceFilename(), SourceFileAfter));
		TestTrue(FString::Printf(TEXT("%s target file readback"), FaultCase.Name),
			CaptureTestFileFingerprint(Fixture.GetTargetFilename(), TargetFileAfter));
		TestTrue(FString::Printf(TEXT("%s source memory readback"), FaultCase.Name),
			CaptureStaticMeshMemoryFingerprint(SourceAfter, SourceMemoryAfter));
		TestTrue(FString::Printf(TEXT("%s target memory readback"), FaultCase.Name),
			CaptureStaticMeshMemoryFingerprint(TargetAfter, TargetMemoryAfter));
		TestEqual(FString::Printf(TEXT("%s donor bytes remain exact"), FaultCase.Name),
			SourceFileAfter.Digest, SourceFileBefore.Digest);
		TestEqual(FString::Printf(TEXT("%s target byte size rolls back"), FaultCase.Name),
			TargetFileAfter.Size, TargetFileBefore.Size);
		TestEqual(FString::Printf(TEXT("%s target byte MD5 rolls back"), FaultCase.Name),
			TargetFileAfter.Digest, TargetFileBefore.Digest);
		TestLogicalMeshFingerprintEqual(
			*this,
			FString::Printf(TEXT("%s donor memory"), FaultCase.Name),
			SourceMemoryAfter,
			SourceMemoryBefore,
			/*bRequireSamePointer=*/true);
		TestLogicalMeshFingerprintEqual(
			*this,
			FString::Printf(TEXT("%s target rollback memory"), FaultCase.Name),
			TargetMemoryAfter,
			TargetMemoryBefore,
			/*bRequireSamePointer=*/false);
		TestTrue(
			FString::Printf(TEXT("%s fixture cleanup succeeds"), FaultCase.Name),
			Fixture.CleanupNow());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
