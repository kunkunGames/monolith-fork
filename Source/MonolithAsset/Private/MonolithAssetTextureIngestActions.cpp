// Copyright tumourlove. All Rights Reserved.
#include "MonolithAssetTextureIngestActions.h"
#include "MonolithAssetTextureIngestInternal.h"

// Monolith registry
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

// Core / JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"                        // FBase64::Decode
#include "Misc/Guid.h"
#include "Misc/PackageName.h"                   // FPackageName::GetLongPackagePath / LongPackageNameToFilename / GetAssetPackageExtension
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Misc/SecureHash.h"                    // FMD5
#include "HAL/FileManager.h"
#include "UObject/Package.h"                    // UPackage, SavePackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"             // CreatePackage, NewObject
#include "HAL/UnrealMemory.h"                   // FMemory::Memcpy

// Image decoding
#include "IImageWrapper.h"                      // IImageWrapper, ERGBFormat, EImageFormat
#include "IImageWrapperModule.h"                // IImageWrapperModule
#include "Modules/ModuleManager.h"              // FModuleManager::LoadModuleChecked

// Texture creation
#include "Engine/Texture.h"                     // TSF_BGRA8, TextureCompressionSettings, TextureMipGenSettings
#include "Engine/Texture2D.h"                   // UTexture2D
#include "Engine/TextureDefines.h"              // TEXTUREGROUP_UI, TEXTUREGROUP_World, etc.
#include "TextureResource.h"                    // FTexturePlatformData, FTexture2DMipMap
#include "PixelFormat.h"                        // PF_B8G8R8A8
#include "RenderingThread.h"                    // FlushRenderingCommands

// Asset registry + asset tools
#include "AssetRegistry/AssetRegistryModule.h"  // FAssetRegistryModule::AssetCreated
#include "AssetToolsModule.h"                   // FAssetToolsModule
#include "IAssetTools.h"                        // IAssetTools::CreateUniqueAssetName

namespace MonolithAsset::TextureIngestInternal
{
    bool ValidateDecodedImageBounds(
        int64 Width,
        int64 Height,
        int64& OutExpectedBytes,
        FString& OutError)
    {
        OutExpectedBytes = 0;
        OutError.Reset();

        if (Width <= 0 || Height <= 0)
        {
            OutError = FString::Printf(
                TEXT("Image header has invalid dimensions: %lldx%lld"),
                Width,
                Height);
            return false;
        }

        if (Width > MaxDecodedImageDimension || Height > MaxDecodedImageDimension)
        {
            OutError = FString::Printf(
                TEXT("Image dimensions %lldx%lld exceed the per-axis limit of %lld"),
                Width,
                Height,
                MaxDecodedImageDimension);
            return false;
        }

        constexpr int64 BytesPerPixel = 4;
        constexpr int64 MaxPixelCount = MaxDecodedImageBytes / BytesPerPixel;
        if (Width > MaxPixelCount / Height)
        {
            OutError = FString::Printf(
                TEXT("Image dimensions %lldx%lld exceed the decoded BGRA8 byte limit of %lld"),
                Width,
                Height,
                MaxDecodedImageBytes);
            return false;
        }

        OutExpectedBytes = Width * Height * BytesPerPixel;
        return true;
    }

    enum class ETextureConflictPolicy : uint8
    {
        Fail,
        Replace,
        Unique,
    };

    struct FTextureRolePreset
    {
        FString Role;
        TextureCompressionSettings Compression = TC_Default;
        bool bSRGB = true;
        TextureMipGenSettings MipGen = TMGS_FromTextureGroup;
        TextureGroup LODGroup = TEXTUREGROUP_World;
        TextureAddress AddressX = TA_Clamp;
        TextureAddress AddressY = TA_Clamp;
        bool bAlphaBleed = false;
        bool bAlphaFromEdgeBackground = false;
        bool bHarmonizeTileEdges = false;
        bool bValidateNormal = false;
        bool bValidateTile = false;
        bool bValidateMask = false;
        bool bExpectPowerOfTwo = false;
    };

    static bool ParseConflictPolicy(
        const TSharedPtr<FJsonObject>& Params,
        ETextureConflictPolicy& OutPolicy,
        FString& OutPolicyName,
        FString& OutError)
    {
        OutPolicyName = TEXT("fail");
        if (Params->HasField(TEXT("conflict_policy"))
            && !Params->TryGetStringField(TEXT("conflict_policy"), OutPolicyName))
        {
            OutError = TEXT("conflict_policy must be a string");
            return false;
        }
        OutPolicyName.TrimStartAndEndInline();
        OutPolicyName.ToLowerInline();

        if (OutPolicyName == TEXT("fail"))
        {
            OutPolicy = ETextureConflictPolicy::Fail;
            return true;
        }
        if (OutPolicyName == TEXT("replace"))
        {
            OutPolicy = ETextureConflictPolicy::Replace;
            return true;
        }
        if (OutPolicyName == TEXT("unique"))
        {
            OutPolicy = ETextureConflictPolicy::Unique;
            return true;
        }

        OutError = FString::Printf(
            TEXT("Invalid conflict_policy '%s' (expected fail, replace, or unique)"),
            *OutPolicyName);
        return false;
    }

    static FString MakeAssetObjectPath(const FString& PackageName)
    {
        return PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
    }

    static UObject* FindOrLoadAssetAtPackagePath(const FString& PackageName, bool bPackageExistsOnDisk)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
        if (UPackage* LoadedPackage = FindPackage(nullptr, *PackageName))
        {
            if (UObject* ExistingObject = FindObject<UObject>(LoadedPackage, *AssetName))
            {
                return ExistingObject;
            }
        }

        return bPackageExistsOnDisk
            ? LoadObject<UObject>(nullptr, *MakeAssetObjectPath(PackageName))
            : nullptr;
    }

    static TArray<FString> GetTexturePackageFilenameCandidates(const FString& PackageName)
    {
        TArray<FString> Result;
        auto AddCandidate = [&Result](FString Filename)
        {
            if (!Filename.IsEmpty())
            {
                Filename = FPaths::ConvertRelativePathToFull(Filename);
                FPaths::NormalizeFilename(Filename);
                Result.AddUnique(Filename);
            }
        };

        FString HeaderFilename;
        if (FPackageName::TryConvertLongPackageNameToFilename(
                PackageName,
                HeaderFilename,
                FPackageName::GetAssetPackageExtension()))
        {
            AddCandidate(MoveTemp(HeaderFilename));
        }

        FPackagePath PackagePath;
        if (FPackagePath::TryFromPackageName(PackageName, PackagePath))
        {
            const EPackageSegment SidecarSegments[] = {
                EPackageSegment::Exports,
                EPackageSegment::BulkDataDefault,
                EPackageSegment::BulkDataOptional,
                EPackageSegment::BulkDataMemoryMapped,
                EPackageSegment::PayloadSidecar,
            };
            for (const EPackageSegment Segment : SidecarSegments)
            {
                AddCandidate(PackagePath.GetLocalFullPath(Segment));
            }
        }
        return Result;
    }

    static FProperty* GetTexturePropertyChecked(FName PropertyName)
    {
        FProperty* Property = FindFProperty<FProperty>(UTexture2D::StaticClass(), PropertyName);
        checkf(Property, TEXT("Expected reflected UTexture2D property '%s'"), *PropertyName.ToString());
        return Property;
    }

    template <typename TValue, typename TSetter>
    static void ApplyTexturePropertyChange(
        UTexture2D* Texture,
        FName PropertyName,
        const TValue& CurrentValue,
        const TValue& NewValue,
        TSetter&& Setter)
    {
        if (CurrentValue == NewValue)
        {
            return;
        }

        FProperty* EditProperty = GetTexturePropertyChecked(PropertyName);
        Texture->PreEditChange(EditProperty);
        Setter();
        FPropertyChangedEvent PropertyEvent(
            EditProperty,
            EPropertyChangeType::ValueSet);
        Texture->PostEditChangeProperty(PropertyEvent);
    }

    static void ApplyTextureSettings(
        UTexture2D* Texture,
        TextureCompressionSettings Compression,
        bool bSRGB,
        TextureMipGenSettings MipGen,
        TextureGroup LODGroup,
        TextureAddress AddressX,
        TextureAddress AddressY)
    {
        // LOD-group validation can update other texture settings, so dispatch it
        // first and then apply the caller's explicit values property-by-property.
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture, LODGroup),
            static_cast<TextureGroup>(Texture->LODGroup),
            LODGroup,
            [Texture, LODGroup]() { Texture->LODGroup = LODGroup; });
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture, CompressionSettings),
            static_cast<TextureCompressionSettings>(Texture->CompressionSettings),
            Compression,
            [Texture, Compression]() { Texture->CompressionSettings = Compression; });
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture, SRGB),
            Texture->SRGB != 0,
            bSRGB,
            [Texture, bSRGB]() { Texture->SRGB = bSRGB; });
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture, MipGenSettings),
            static_cast<TextureMipGenSettings>(Texture->MipGenSettings),
            MipGen,
            [Texture, MipGen]() { Texture->MipGenSettings = MipGen; });
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture2D, AddressX),
            static_cast<TextureAddress>(Texture->AddressX),
            AddressX,
            [Texture, AddressX]() { Texture->AddressX = AddressX; });
        ApplyTexturePropertyChange(
            Texture,
            GET_MEMBER_NAME_CHECKED(UTexture2D, AddressY),
            static_cast<TextureAddress>(Texture->AddressY),
            AddressY,
            [Texture, AddressY]() { Texture->AddressY = AddressY; });
    }

    struct FTextureSideEffectSnapshot
    {
        void Capture(UTexture2D* Texture)
        {
            Filter = Texture->Filter;
            bUseLegacyGamma = Texture->bUseLegacyGamma;
            PowerOfTwoMode = Texture->PowerOfTwoMode;
            ResizeDuringBuildX = Texture->ResizeDuringBuildX;
            ResizeDuringBuildY = Texture->ResizeDuringBuildY;
            bVirtualTextureStreaming = Texture->VirtualTextureStreaming;
            MaxTextureSize = Texture->MaxTextureSize;
            NumCinematicMipLevels = Texture->NumCinematicMipLevels;
            LightingGuid = Texture->GetLightingGuid();
            bDeferCompression = Texture->DeferCompression;
            bTemporarilyDisableStreaming = GetTemporaryDisableStreamingProperty()
                ->GetPropertyValue_InContainer(Texture);
        }

        void Restore(UTexture2D* Texture) const
        {
            Texture->Filter = Filter;
            Texture->bUseLegacyGamma = bUseLegacyGamma;
            Texture->PowerOfTwoMode = PowerOfTwoMode;
            Texture->ResizeDuringBuildX = ResizeDuringBuildX;
            Texture->ResizeDuringBuildY = ResizeDuringBuildY;
            Texture->VirtualTextureStreaming = bVirtualTextureStreaming;
            Texture->MaxTextureSize = MaxTextureSize;
            Texture->NumCinematicMipLevels = NumCinematicMipLevels;
            Texture->DeferCompression = bDeferCompression;
            GetTemporaryDisableStreamingProperty()
                ->SetPropertyValue_InContainer(Texture, bTemporarilyDisableStreaming);
        }

        void RestoreWithCallbacks(UTexture2D* Texture) const
        {
            ApplyTexturePropertyChange(
                Texture,
                GET_MEMBER_NAME_CHECKED(UTexture, Filter),
                static_cast<TextureFilter>(Texture->Filter),
                Filter,
                [Texture, Filter = Filter]() { Texture->Filter = Filter; });
            ApplyTexturePropertyChange(
                Texture,
                GET_MEMBER_NAME_CHECKED(UTexture, VirtualTextureStreaming),
                Texture->VirtualTextureStreaming != 0,
                bVirtualTextureStreaming,
                [Texture, bVirtualTextureStreaming = bVirtualTextureStreaming]()
                {
                    Texture->VirtualTextureStreaming = bVirtualTextureStreaming;
                });

            // The remaining fields are validation/save side effects rather than
            // values this action explicitly edits. Restore them exactly after
            // the two material/resource-sensitive callbacks above.
            Restore(Texture);
        }

        void RestoreLightingGuid(UTexture2D* Texture) const
        {
            Texture->SetLightingGuid(LightingGuid);
        }

        bool Matches(UTexture2D* Texture) const
        {
            return Texture->Filter == Filter
                && (Texture->bUseLegacyGamma != 0) == bUseLegacyGamma
                && Texture->PowerOfTwoMode == PowerOfTwoMode
                && Texture->ResizeDuringBuildX == ResizeDuringBuildX
                && Texture->ResizeDuringBuildY == ResizeDuringBuildY
                && (Texture->VirtualTextureStreaming != 0) == bVirtualTextureStreaming
                && Texture->MaxTextureSize == MaxTextureSize
                && Texture->NumCinematicMipLevels == NumCinematicMipLevels
                && Texture->GetLightingGuid() == LightingGuid
                && (Texture->DeferCompression != 0) == bDeferCompression
                && GetTemporaryDisableStreamingProperty()->GetPropertyValue_InContainer(Texture)
                    == bTemporarilyDisableStreaming;
        }

    private:
        static FBoolProperty* GetTemporaryDisableStreamingProperty()
        {
            FBoolProperty* Property = FindFProperty<FBoolProperty>(
                UTexture2D::StaticClass(),
                TEXT("bTemporarilyDisableStreaming"));
            check(Property);
            return Property;
        }

        TextureFilter Filter = TF_Default;
        bool bUseLegacyGamma = false;
        ETexturePowerOfTwoSetting::Type PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
        int32 ResizeDuringBuildX = 0;
        int32 ResizeDuringBuildY = 0;
        bool bVirtualTextureStreaming = false;
        int32 MaxTextureSize = 0;
        int32 NumCinematicMipLevels = 0;
        FGuid LightingGuid;
        bool bDeferCompression = false;
        bool bTemporarilyDisableStreaming = false;
    };

    static void SwapTexturePlatformData(
        FTexturePlatformData& Left,
        FTexturePlatformData& Right)
    {
        Swap(Left.SizeX, Right.SizeX);
        Swap(Left.SizeY, Right.SizeY);
        Swap(Left.PackedData, Right.PackedData);
        Swap(Left.PixelFormat, Right.PixelFormat);
        Swap(Left.OptData, Right.OptData);
        Swap(Left.Mips, Right.Mips);
        Swap(Left.VTData, Right.VTData);
        Swap(Left.CPUCopy, Right.CPUCopy);
#if WITH_EDITORONLY_DATA
        check(Left.AsyncTask == nullptr && Right.AsyncTask == nullptr);
        Swap(Left.PreEncodeMipsHash, Right.PreEncodeMipsHash);
        Swap(Left.DerivedDataKey, Right.DerivedDataKey);
        Swap(Left.ResultMetadata, Right.ResultMetadata);
        Swap(Left.FetchOrBuildDerivedDataKey, Right.FetchOrBuildDerivedDataKey);
        Swap(Left.FetchFirstDerivedDataKey, Right.FetchFirstDerivedDataKey);
        Swap(Left.AsyncTask, Right.AsyncTask);
#endif
    }

    static void DestroyOwnedPlatformDataMap(
        TMap<FString, FTexturePlatformData*>& PlatformDataMap,
        std::initializer_list<FTexturePlatformData*> PreservedPlatformData)
    {
        TSet<FTexturePlatformData*> Preserved;
        for (FTexturePlatformData* PlatformData : PreservedPlatformData)
        {
            if (PlatformData)
            {
                Preserved.Add(PlatformData);
            }
        }

        TSet<FTexturePlatformData*> Deleted;
        for (const TPair<FString, FTexturePlatformData*>& Pair : PlatformDataMap)
        {
            FTexturePlatformData* PlatformData = Pair.Value;
            if (PlatformData && !Preserved.Contains(PlatformData) && !Deleted.Contains(PlatformData))
            {
                Deleted.Add(PlatformData);
                delete PlatformData;
            }
        }
        PlatformDataMap.Reset();
    }

    struct FTexturePlatformIdentity
    {
        void Capture(FTexturePlatformData* PlatformData)
        {
            check(PlatformData);
            PlatformDataAddress = PlatformData;
            SizeX = PlatformData->SizeX;
            SizeY = PlatformData->SizeY;
            PackedData = PlatformData->PackedData;
            PixelFormat = PlatformData->PixelFormat;
            OptData = PlatformData->OptData;
            VTDataAddress = PlatformData->VTData;
            CPUCopyAddress = PlatformData->CPUCopy.GetReference();
            MipAddresses.Reserve(PlatformData->Mips.Num());
            for (FTexture2DMipMap& Mip : PlatformData->Mips)
            {
                MipAddresses.Add(&Mip);
            }
#if WITH_EDITORONLY_DATA
            PreEncodeMipsHash = PlatformData->PreEncodeMipsHash;
            DerivedDataKey = PlatformData->DerivedDataKey;
            ResultMetadata = PlatformData->ResultMetadata;
            FetchOrBuildDerivedDataKey = PlatformData->FetchOrBuildDerivedDataKey;
            FetchFirstDerivedDataKey = PlatformData->FetchFirstDerivedDataKey;
#endif
        }

        void SetExpectedPlatformDataAddress(FTexturePlatformData* PlatformData)
        {
            PlatformDataAddress = PlatformData;
        }

        bool Matches(FTexturePlatformData* PlatformData) const
        {
            if (!PlatformData
                || PlatformData != PlatformDataAddress
                || PlatformData->SizeX != SizeX
                || PlatformData->SizeY != SizeY
                || PlatformData->PackedData != PackedData
                || PlatformData->PixelFormat != PixelFormat
                || PlatformData->OptData != OptData
                || PlatformData->VTData != VTDataAddress
                || PlatformData->CPUCopy.GetReference() != CPUCopyAddress
                || PlatformData->Mips.Num() != MipAddresses.Num())
            {
                return false;
            }
            for (int32 MipIndex = 0; MipIndex < MipAddresses.Num(); ++MipIndex)
            {
                if (&PlatformData->Mips[MipIndex] != MipAddresses[MipIndex])
                {
                    return false;
                }
            }
#if WITH_EDITORONLY_DATA
            if (PlatformData->AsyncTask != nullptr
                || PlatformData->PreEncodeMipsHash != PreEncodeMipsHash
                || !DerivedDataKeyMatches(PlatformData->DerivedDataKey, DerivedDataKey)
                || !ResultMetadataMatches(PlatformData->ResultMetadata, ResultMetadata)
                || !FetchKeyMatches(
                    PlatformData->FetchOrBuildDerivedDataKey,
                    FetchOrBuildDerivedDataKey)
                || !FetchKeyMatches(
                    PlatformData->FetchFirstDerivedDataKey,
                    FetchFirstDerivedDataKey))
            {
                return false;
            }
#endif
            return true;
        }

    private:
#if WITH_EDITORONLY_DATA
        static bool DerivedDataKeyMatches(
            const decltype(FTexturePlatformData::DerivedDataKey)& Left,
            const decltype(FTexturePlatformData::DerivedDataKey)& Right)
        {
            if (Left.GetIndex() != Right.GetIndex())
            {
                return false;
            }
            if (Left.IsType<FString>())
            {
                return Left.Get<FString>() == Right.Get<FString>();
            }

            // FCacheKeyProxy is not a byte-comparable POD value. The original
            // variant object is moved out and then moved back during rollback,
            // so matching the active alternative is the strongest safe public
            // check available without depending on private DDC internals.
            return Left.IsType<UE::DerivedData::FCacheKeyProxy>()
                && Right.IsType<UE::DerivedData::FCacheKeyProxy>();
        }

        static bool FetchKeyMatches(
            const decltype(FTexturePlatformData::FetchOrBuildDerivedDataKey)& Left,
            const decltype(FTexturePlatformData::FetchOrBuildDerivedDataKey)& Right)
        {
            if (Left.GetIndex() != Right.GetIndex())
            {
                return false;
            }
            if (Left.IsType<FString>())
            {
                return Left.Get<FString>() == Right.Get<FString>();
            }
            return Left.Get<FTexturePlatformData::FStructuredDerivedDataKey>()
                == Right.Get<FTexturePlatformData::FStructuredDerivedDataKey>();
        }

        static bool ResultMetadataMatches(
            const FTexturePlatformData::FTextureEncodeResultMetadata& Left,
            const FTexturePlatformData::FTextureEncodeResultMetadata& Right)
        {
            return Left.Encoder == Right.Encoder
                && Left.EncodedFormat == Right.EncodedFormat
                && Left.bIsValid == Right.bIsValid
                && Left.bSupportsEncodeSpeed == Right.bSupportsEncodeSpeed
                && Left.bWasEditorCustomEncoding == Right.bWasEditorCustomEncoding
                && Left.RDOSource == Right.RDOSource
                && Left.OodleRDO == Right.OodleRDO
                && Left.OodleEncodeEffort == Right.OodleEncodeEffort
                && Left.OodleUniversalTiling == Right.OodleUniversalTiling
                && Left.EncodeSpeed == Right.EncodeSpeed;
        }
#endif

        FTexturePlatformData* PlatformDataAddress = nullptr;
        int32 SizeX = 0;
        int32 SizeY = 0;
        uint32 PackedData = 0;
        EPixelFormat PixelFormat = PF_Unknown;
        FOptTexturePlatformData OptData;
        FVirtualTextureBuiltData* VTDataAddress = nullptr;
        const FSharedImage* CPUCopyAddress = nullptr;
        TArray<FTexture2DMipMap*> MipAddresses;
#if WITH_EDITORONLY_DATA
        uint64 PreEncodeMipsHash = 0;
        decltype(FTexturePlatformData::DerivedDataKey) DerivedDataKey;
        FTexturePlatformData::FTextureEncodeResultMetadata ResultMetadata;
        decltype(FTexturePlatformData::FetchOrBuildDerivedDataKey) FetchOrBuildDerivedDataKey;
        decltype(FTexturePlatformData::FetchFirstDerivedDataKey) FetchFirstDerivedDataKey;
#endif
    };

    class FTextureReplacementSnapshot
    {
    public:
        ~FTextureReplacementSnapshot()
        {
            if (bCaptured)
            {
                FString IgnoredError;
                Restore(IgnoredError);
            }
        }

        bool Capture(UTexture2D* InTexture, FString& OutError)
        {
            if (!InTexture || !InTexture->GetOutermost())
            {
                OutError = TEXT("Cannot snapshot a null or unowned replacement texture");
                return false;
            }

            Texture = InTexture;
            Texture->BlockOnAnyAsyncBuild();
            Texture->WaitForPendingInitOrStreaming();
            Texture->FinishCachePlatformData();

            FTexturePlatformData* RunningPlatformData = Texture->GetPlatformData();

            if (Texture->ResourceMem != nullptr)
            {
                OutError = TEXT("Atomic replacement does not support textures with active ResourceMem");
                return false;
            }

            for (const TPair<FString, FTexturePlatformData*>& Pair : Texture->CookedPlatformData)
            {
                if (Pair.Value && Pair.Value->AsyncTask != nullptr)
                {
                    OutError = FString::Printf(
                        TEXT("Cannot replace texture while cooked platform data '%s' is still building"),
                        *Pair.Key);
                    return false;
                }
            }

            TSet<FTexturePlatformData*> UniqueCookedPlatformData;
            for (const TPair<FString, FTexturePlatformData*>& Pair : Texture->CookedPlatformData)
            {
                if (!Pair.Value)
                {
                    continue;
                }
                if (Pair.Value == RunningPlatformData || UniqueCookedPlatformData.Contains(Pair.Value))
                {
                    OutError = FString::Printf(
                        TEXT("Cannot atomically replace texture with aliased cooked platform data '%s'"),
                        *Pair.Key);
                    return false;
                }
                UniqueCookedPlatformData.Add(Pair.Value);
            }

            bPackageWasDirty = Texture->GetOutermost()->IsDirty();
            bSourceWasValid = Texture->Source.IsValid();
            if (bSourceWasValid)
            {
                SourcePersistentId = Texture->Source.GetPersistentId();
                SourceIdString = Texture->Source.GetIdString();
                SourceCompression = Texture->Source.GetSourceCompression();
                bSourceWasLongLat = Texture->Source.IsLongLatCubemap();
                SourcePayloadSize = Texture->Source.GetSizeOnDisk();
                SourceStoredPayload = Texture->Source.GetBulkDataPayload();
                Texture->Source.GetLayerColorInfo(SourceLayerColorInfo);
                SourceBlocks.SetNum(Texture->Source.GetNumBlocks());
                for (int32 BlockIndex = 0; BlockIndex < SourceBlocks.Num(); ++BlockIndex)
                {
                    Texture->Source.GetBlock(BlockIndex, SourceBlocks[BlockIndex]);
                }
                SourceLayerFormats.Reserve(Texture->Source.GetNumLayers());
                for (int32 LayerIndex = 0; LayerIndex < Texture->Source.GetNumLayers(); ++LayerIndex)
                {
                    SourceLayerFormats.Add(Texture->Source.GetFormat(LayerIndex));
                }
            }
            Compression = Texture->CompressionSettings;
            bSRGB = Texture->SRGB;
            MipGen = Texture->MipGenSettings;
            LODGroup = Texture->LODGroup;
            AddressX = Texture->AddressX;
            AddressY = Texture->AddressY;
            SideEffects.Capture(Texture);
            CPUCopyTextureSnapshot.Reset(Texture->CPUCopyTexture.Get());

            bHadRunningPlatformData = RunningPlatformData != nullptr;
            if (RunningPlatformData && RunningPlatformData->AsyncTask != nullptr)
            {
                OutError = TEXT("Cannot replace texture while running platform data is still building");
                return false;
            }

            // Capture the transaction before either exact ownership swap. A
            // failed preflight therefore does not modify transaction state,
            // while undo still records the complete original texture.
            Texture->Modify(/*bAlwaysMarkDirty=*/false);
            Swap(Texture->Source, SourceSnapshot);
            Texture->Source.SetOwner(Texture);

            Texture->ReleaseResource();
            FlushRenderingCommands();
            if (RunningPlatformData)
            {
                AttachedPlatformData = RunningPlatformData;
                PlatformDataSnapshot = MakeUnique<FTexturePlatformData>();
                SwapTexturePlatformData(*RunningPlatformData, *PlatformDataSnapshot);
                PlatformIdentity.Capture(PlatformDataSnapshot.Get());
                PlatformIdentity.SetExpectedPlatformDataAddress(AttachedPlatformData);
            }

            ExpectedCookedPlatformData = Texture->CookedPlatformData;
            Swap(Texture->CookedPlatformData, CookedPlatformDataSnapshot);
            bCaptured = true;
            return true;
        }

        void InstallReplacementPlatformData(TUniquePtr<FTexturePlatformData>& PreparedPlatformData)
        {
            check(bCaptured && PreparedPlatformData);
            if (bHadRunningPlatformData)
            {
                check(AttachedPlatformData);
                SwapTexturePlatformData(*AttachedPlatformData, *PreparedPlatformData);
                PreparedPlatformData.Reset();
            }
            else
            {
                check(!AttachedPlatformData && !PlatformDataSnapshot);
                Texture->SetPlatformData(PreparedPlatformData.Release());
            }
        }

        bool Restore(FString& OutError)
        {
            if (!bCaptured || !Texture)
            {
                return true;
            }

            Texture->BlockOnAnyAsyncBuild();
            Texture->WaitForPendingInitOrStreaming();
            Texture->FinishCachePlatformData();
            Texture->ReleaseResource();
            FlushRenderingCommands();

            Texture->PreEditChange(nullptr);
            Swap(Texture->Source, SourceSnapshot);
            Texture->Source.SetOwner(Texture);
            Texture->PostEditChange();

            // The source callback above is generic. Replay the settings as
            // actual replacement-to-original property changes so UTexture's
            // property-specific resource and material invalidation runs too.
            RestoreTextureSettingsWithCallbacks();
            RestoreTextureSettings();
            SideEffects.RestoreLightingGuid(Texture);

            Texture->BlockOnAnyAsyncBuild();
            Texture->WaitForPendingInitOrStreaming();
            Texture->FinishCachePlatformData();
            Texture->ReleaseResource();
            FlushRenderingCommands();

            FTexturePlatformData* CurrentPlatformData = Texture->GetPlatformData();
            // UTexture::ClearAllCachedCookedPlatformData deletes every map
            // value without guarding duplicate or running-data aliases. Clear
            // the replacement map ourselves so an anomalous cache entry can
            // never delete the allocation still owned by UTexture2D.
            DestroyOwnedPlatformDataMap(
                Texture->CookedPlatformData,
                {CurrentPlatformData, PlatformDataSnapshot.Get()});
            if (bHadRunningPlatformData)
            {
                if (CurrentPlatformData == AttachedPlatformData)
                {
                    // One swap restores every original owned field into the
                    // original top-level allocation; resetting the snapshot
                    // then destroys the replacement fields exactly once.
                    SwapTexturePlatformData(*AttachedPlatformData, *PlatformDataSnapshot);
                    PlatformDataSnapshot.Reset();
                }
                else
                {
                    // Do not turn an engine-side allocation change into an
                    // editor-fatal assertion. Reinstall the exact saved object
                    // and report the lost top-level identity as a recoverable
                    // rollback postcondition failure.
                    FTexturePlatformData* RestoredPlatformData = PlatformDataSnapshot.Get();
                    Texture->SetPlatformData(PlatformDataSnapshot.Release());
                    PlatformIdentity.SetExpectedPlatformDataAddress(RestoredPlatformData);
                    bPlatformAllocationChangedDuringRollback = true;
                }
            }
            else
            {
                Texture->SetPlatformData(nullptr);
            }

            Swap(Texture->CookedPlatformData, CookedPlatformDataSnapshot);
            Texture->UTexture::UpdateResourceWithParams(UTexture::EUpdateResourceFlags::Synchronous);
            Texture->BlockOnAnyAsyncBuild();
            Texture->WaitForPendingInitOrStreaming();
            Texture->FinishCachePlatformData();

            RestoreTextureSettings();
            SideEffects.RestoreLightingGuid(Texture);
            Texture->CPUCopyTexture = CPUCopyTextureSnapshot.Get();
            Texture->GetOutermost()->SetDirtyFlag(bPackageWasDirty);
            bCaptured = false;

            TArray<FString> Mismatches;
            if (!SourceMatches())
            {
                Mismatches.Add(TEXT("source"));
            }
            if (!PlatformDataMatches())
            {
                Mismatches.Add(TEXT("platform_data"));
            }
            if (bPlatformAllocationChangedDuringRollback)
            {
                Mismatches.Add(TEXT("platform_data_allocation"));
            }
            if (!TextureSettingsMatch())
            {
                Mismatches.Add(TEXT("settings"));
            }
            if (!CookedPlatformDataMatches())
            {
                Mismatches.Add(TEXT("cooked_platform_data"));
            }
            if (Texture->CPUCopyTexture.Get() != CPUCopyTextureSnapshot.Get())
            {
                Mismatches.Add(TEXT("cpu_copy_texture"));
            }
            if (Texture->GetOutermost()->IsDirty() != bPackageWasDirty)
            {
                Mismatches.Add(TEXT("dirty_state"));
            }

            if (Mismatches.Num() > 0)
            {
                OutError = FString::Printf(
                    TEXT("Texture replacement rollback postcondition failed: %s"),
                    *FString::Join(Mismatches, TEXT(", ")));
                return false;
            }
            return true;
        }

        void Commit()
        {
            DeleteSavedCookedPlatformData();
            PlatformDataSnapshot.Reset();
            CPUCopyTextureSnapshot.Reset();
            bCaptured = false;
        }

    private:
        void RestoreTextureSettingsWithCallbacks() const
        {
            ApplyTextureSettings(
                Texture,
                Compression,
                bSRGB,
                MipGen,
                LODGroup,
                AddressX,
                AddressY);
            SideEffects.RestoreWithCallbacks(Texture);
        }

        void RestoreTextureSettings() const
        {
            Texture->CompressionSettings = Compression;
            Texture->SRGB = bSRGB;
            Texture->MipGenSettings = MipGen;
            Texture->LODGroup = LODGroup;
            Texture->AddressX = AddressX;
            Texture->AddressY = AddressY;
            SideEffects.Restore(Texture);
        }

        bool TextureSettingsMatch() const
        {
            return Texture
                && Texture->CompressionSettings == Compression
                && (Texture->SRGB != 0) == bSRGB
                && Texture->MipGenSettings == MipGen
                && Texture->LODGroup == LODGroup
                && Texture->AddressX == AddressX
                && Texture->AddressY == AddressY
                && SideEffects.Matches(Texture);
        }

        bool CookedPlatformDataMatches() const
        {
            if (!Texture || Texture->CookedPlatformData.Num() != ExpectedCookedPlatformData.Num())
            {
                return false;
            }
            for (const TPair<FString, FTexturePlatformData*>& Pair : ExpectedCookedPlatformData)
            {
                FTexturePlatformData* const* Restored = Texture->CookedPlatformData.Find(Pair.Key);
                if (!Restored || *Restored != Pair.Value)
                {
                    return false;
                }
            }
            return true;
        }

        void DeleteSavedCookedPlatformData()
        {
            DestroyOwnedPlatformDataMap(
                CookedPlatformDataSnapshot,
                {AttachedPlatformData, Texture ? Texture->GetPlatformData() : nullptr, PlatformDataSnapshot.Get()});
            ExpectedCookedPlatformData.Reset();
        }

        bool SourceMatches() const
        {
            if (!Texture || Texture->Source.IsValid() != bSourceWasValid)
            {
                return false;
            }
            if (!bSourceWasValid)
            {
                return true;
            }
            if (Texture->Source.GetPersistentId() != SourcePersistentId
                || Texture->Source.GetIdString() != SourceIdString
                || Texture->Source.GetSourceCompression() != SourceCompression
                || Texture->Source.IsLongLatCubemap() != bSourceWasLongLat
                || Texture->Source.GetSizeOnDisk() != SourcePayloadSize)
            {
                return false;
            }
            if (Texture->Source.GetNumBlocks() != SourceBlocks.Num()
                || Texture->Source.GetNumLayers() != SourceLayerFormats.Num())
            {
                return false;
            }
            for (int32 LayerIndex = 0; LayerIndex < SourceLayerFormats.Num(); ++LayerIndex)
            {
                if (Texture->Source.GetFormat(LayerIndex) != SourceLayerFormats[LayerIndex])
                {
                    return false;
                }
            }
            TArray<FTextureSourceLayerColorInfo> RestoredLayerColorInfo;
            Texture->Source.GetLayerColorInfo(RestoredLayerColorInfo);
            if (RestoredLayerColorInfo.Num() != SourceLayerColorInfo.Num())
            {
                return false;
            }
            for (int32 LayerIndex = 0; LayerIndex < SourceLayerColorInfo.Num(); ++LayerIndex)
            {
                if (RestoredLayerColorInfo[LayerIndex].ColorMin
                        != SourceLayerColorInfo[LayerIndex].ColorMin
                    || RestoredLayerColorInfo[LayerIndex].ColorMax
                        != SourceLayerColorInfo[LayerIndex].ColorMax)
                {
                    return false;
                }
            }
            for (int32 BlockIndex = 0; BlockIndex < SourceBlocks.Num(); ++BlockIndex)
            {
                FTextureSourceBlock RestoredBlock;
                Texture->Source.GetBlock(BlockIndex, RestoredBlock);
                const FTextureSourceBlock& OriginalBlock = SourceBlocks[BlockIndex];
                if (RestoredBlock.BlockX != OriginalBlock.BlockX
                    || RestoredBlock.BlockY != OriginalBlock.BlockY
                    || RestoredBlock.SizeX != OriginalBlock.SizeX
                    || RestoredBlock.SizeY != OriginalBlock.SizeY
                    || RestoredBlock.NumSlices != OriginalBlock.NumSlices
                    || RestoredBlock.NumMips != OriginalBlock.NumMips)
                {
                    return false;
                }
            }

            const FSharedBuffer RestoredStoredPayload = Texture->Source.GetBulkDataPayload();
            if (RestoredStoredPayload.IsNull() != SourceStoredPayload.IsNull()
                || RestoredStoredPayload.GetSize() != SourceStoredPayload.GetSize())
            {
                return false;
            }
            return RestoredStoredPayload.GetSize() == 0
                || FMemory::Memcmp(
                    RestoredStoredPayload.GetData(),
                    SourceStoredPayload.GetData(),
                    static_cast<SIZE_T>(RestoredStoredPayload.GetSize())) == 0;
        }

        bool PlatformDataMatches() const
        {
            return Texture
                && (bHadRunningPlatformData
                    ? PlatformIdentity.Matches(Texture->GetPlatformData())
                    : Texture->GetPlatformData() == nullptr);
        }

        UTexture2D* Texture = nullptr;
        FTextureSource SourceSnapshot;
        TArray<FTextureSourceBlock> SourceBlocks;
        TArray<ETextureSourceFormat> SourceLayerFormats;
        TArray<FTextureSourceLayerColorInfo> SourceLayerColorInfo;
        FSharedBuffer SourceStoredPayload;
        FGuid SourcePersistentId;
        FString SourceIdString;
        ETextureSourceCompressionFormat SourceCompression = TSCF_None;
        int64 SourcePayloadSize = 0;
        TUniquePtr<FTexturePlatformData> PlatformDataSnapshot;
        FTexturePlatformData* AttachedPlatformData = nullptr;
        FTexturePlatformIdentity PlatformIdentity;
        TMap<FString, FTexturePlatformData*> CookedPlatformDataSnapshot;
        TMap<FString, FTexturePlatformData*> ExpectedCookedPlatformData;
        FTextureSideEffectSnapshot SideEffects;
        TStrongObjectPtr<UTexture2D> CPUCopyTextureSnapshot;
        TextureCompressionSettings Compression = TC_Default;
        bool bSRGB = true;
        TextureMipGenSettings MipGen = TMGS_NoMipmaps;
        TextureGroup LODGroup = TEXTUREGROUP_UI;
        TextureAddress AddressX = TA_Clamp;
        TextureAddress AddressY = TA_Clamp;
        bool bPackageWasDirty = false;
        bool bSourceWasValid = false;
        bool bSourceWasLongLat = false;
        bool bHadRunningPlatformData = false;
        bool bPlatformAllocationChangedDuringRollback = false;
        bool bCaptured = false;
    };

    static bool RollbackCreatedTexture(
        UTexture2D* Texture,
        const FString& PackageName,
        FString& OutError)
    {
        OutError.Reset();
        if (!Texture)
        {
            OutError = TEXT("Cannot roll back a null created texture");
            return false;
        }

        UPackage* Package = Texture->GetOutermost();
        FAssetRegistryModule::AssetDeleted(Texture);
        Texture->ClearFlags(RF_Public | RF_Standalone);
        const bool bTextureRenamed = Texture->Rename(
            *FString::Printf(TEXT("__monolith_failed_texture_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short)),
            GetTransientPackage(),
            REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
        Texture->MarkAsGarbage();

        if (Package)
        {
            Package->SetDirtyFlag(false);
            ResetLoaders(Package);
            const bool bPackageRenamed = Package->Rename(
                *FString::Printf(
                    TEXT("/Temp/__monolith_failed_texture_package_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Short)),
                nullptr,
                REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
            if (!bPackageRenamed)
            {
                OutError = TEXT("Failed to detach the created texture package during rollback");
            }
            Package->MarkAsGarbage();
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

        TArray<FString> ResidualFiles;
        for (const FString& PackageFilename : GetTexturePackageFilenameCandidates(PackageName))
        {
            if (IFileManager::Get().FileExists(*PackageFilename)
                && !IFileManager::Get().Delete(
                    *PackageFilename,
                    /*RequireExists=*/false,
                    /*EvenReadOnly=*/true,
                    /*Quiet=*/true))
            {
                ResidualFiles.Add(PackageFilename);
            }
            else if (IFileManager::Get().FileExists(*PackageFilename))
            {
                ResidualFiles.Add(PackageFilename);
            }
        }

        if (!bTextureRenamed)
        {
            OutError = TEXT("Failed to detach the created texture object during rollback");
        }
        TArray<FAssetData> RegisteredAssets;
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().GetAssetsByPackageName(
            FName(*PackageName),
            RegisteredAssets,
            /*bIncludeOnlyOnDiskAssets=*/false);
        const bool bPackageStillLoaded = FindPackage(nullptr, *PackageName) != nullptr;
        if (bPackageStillLoaded
            || RegisteredAssets.Num() > 0
            || ResidualFiles.Num() > 0
            || !OutError.IsEmpty())
        {
            const FString ResidualSuffix = ResidualFiles.Num() > 0
                ? FString::Printf(TEXT("; residual package files: %s"), *FString::Join(ResidualFiles, TEXT(", ")))
                : FString();
            if (OutError.IsEmpty())
            {
                if (bPackageStillLoaded)
                {
                    OutError = TEXT("Created texture package remained loaded after rollback");
                }
                else if (RegisteredAssets.Num() > 0)
                {
                    OutError = TEXT("Created texture remained registered after rollback");
                }
                else
                {
                    OutError = TEXT("Created texture rollback left package-file residuals");
                }
            }
            OutError += ResidualSuffix;
            return false;
        }
        return true;
    }

    // Map a "png" / "jpg" / "jpeg" / "bmp" / "exr" / "tga" hint to an EImageFormat.
    // NOTE: UE 5.7 EImageFormat has NO WebP member (checked against IImageWrapper.h:26-69).
    // Returns EImageFormat::Invalid on unknown hint so the caller can error out with -32602.
    static EImageFormat ParseFormatHint(const FString& Hint)
    {
        const FString Lower = Hint.ToLower();
        if (Lower == TEXT("png"))                               { return EImageFormat::PNG;  }
        if (Lower == TEXT("jpg") || Lower == TEXT("jpeg"))      { return EImageFormat::JPEG; }
        if (Lower == TEXT("bmp"))                               { return EImageFormat::BMP;  }
        if (Lower == TEXT("exr"))                               { return EImageFormat::EXR;  }
        if (Lower == TEXT("tga"))                               { return EImageFormat::TGA;  }
        if (Lower == TEXT("hdr"))                               { return EImageFormat::HDR;  }
        if (Lower == TEXT("tif") || Lower == TEXT("tiff"))      { return EImageFormat::TIFF; }
        if (Lower == TEXT("dds"))                               { return EImageFormat::DDS;  }
        return EImageFormat::Invalid;
    }

    // Map a "TC_Default" / "TC_Grayscale" / ... string to TextureCompressionSettings.
    static bool ParseCompression(const FString& S, TextureCompressionSettings& Out)
    {
        if (S == TEXT("TC_Default"))                  { Out = TC_Default;                  return true; }
        if (S == TEXT("TC_Normalmap"))                { Out = TC_Normalmap;                return true; }
        if (S == TEXT("TC_Masks"))                    { Out = TC_Masks;                    return true; }
        if (S == TEXT("TC_Grayscale"))                { Out = TC_Grayscale;                return true; }
        if (S == TEXT("TC_Displacementmap"))          { Out = TC_Displacementmap;          return true; }
        if (S == TEXT("TC_VectorDisplacementmap"))    { Out = TC_VectorDisplacementmap;    return true; }
        if (S == TEXT("TC_HDR"))                      { Out = TC_HDR;                      return true; }
        if (S == TEXT("TC_EditorIcon"))               { Out = TC_EditorIcon;               return true; }
        if (S == TEXT("TC_Alpha"))                    { Out = TC_Alpha;                    return true; }
        if (S == TEXT("TC_DistanceFieldFont"))        { Out = TC_DistanceFieldFont;        return true; }
        if (S == TEXT("TC_HDR_Compressed"))           { Out = TC_HDR_Compressed;           return true; }
        if (S == TEXT("TC_BC7"))                      { Out = TC_BC7;                      return true; }
        if (S == TEXT("TC_HalfFloat"))                { Out = TC_HalfFloat;                return true; }
        if (S == TEXT("TC_LQ"))                       { Out = TC_LQ;                       return true; }
        if (S == TEXT("TC_EncodedReflectionCapture")) { Out = TC_EncodedReflectionCapture; return true; }
        if (S == TEXT("TC_SingleFloat"))              { Out = TC_SingleFloat;              return true; }
        return false;
    }

    // Map a "TMGS_FromTextureGroup" / "TMGS_NoMipmaps" / ... string to TextureMipGenSettings.
    static bool ParseMipGen(const FString& S, TextureMipGenSettings& Out)
    {
        if (S == TEXT("TMGS_FromTextureGroup"))  { Out = TMGS_FromTextureGroup;  return true; }
        if (S == TEXT("TMGS_SimpleAverage"))     { Out = TMGS_SimpleAverage;     return true; }
        if (S == TEXT("TMGS_Sharpen0"))          { Out = TMGS_Sharpen0;          return true; }
        if (S == TEXT("TMGS_Sharpen1"))          { Out = TMGS_Sharpen1;          return true; }
        if (S == TEXT("TMGS_Sharpen2"))          { Out = TMGS_Sharpen2;          return true; }
        if (S == TEXT("TMGS_Sharpen3"))          { Out = TMGS_Sharpen3;          return true; }
        if (S == TEXT("TMGS_Sharpen4"))          { Out = TMGS_Sharpen4;          return true; }
        if (S == TEXT("TMGS_Sharpen5"))          { Out = TMGS_Sharpen5;          return true; }
        if (S == TEXT("TMGS_Sharpen6"))          { Out = TMGS_Sharpen6;          return true; }
        if (S == TEXT("TMGS_Sharpen7"))          { Out = TMGS_Sharpen7;          return true; }
        if (S == TEXT("TMGS_Sharpen8"))          { Out = TMGS_Sharpen8;          return true; }
        if (S == TEXT("TMGS_Sharpen9"))          { Out = TMGS_Sharpen9;          return true; }
        if (S == TEXT("TMGS_Sharpen10"))         { Out = TMGS_Sharpen10;         return true; }
        if (S == TEXT("TMGS_NoMipmaps"))         { Out = TMGS_NoMipmaps;         return true; }
        if (S == TEXT("TMGS_LeaveExistingMips")) { Out = TMGS_LeaveExistingMips; return true; }
        if (S == TEXT("TMGS_Blur1"))             { Out = TMGS_Blur1;             return true; }
        if (S == TEXT("TMGS_Blur2"))             { Out = TMGS_Blur2;             return true; }
        if (S == TEXT("TMGS_Blur3"))             { Out = TMGS_Blur3;             return true; }
        if (S == TEXT("TMGS_Blur4"))             { Out = TMGS_Blur4;             return true; }
        if (S == TEXT("TMGS_Blur5"))             { Out = TMGS_Blur5;             return true; }
        return false;
    }

    // Map a "TEXTUREGROUP_UI" / "TEXTUREGROUP_World" / ... string to a TextureGroup enum.
    static bool ParseLODGroup(const FString& S, TextureGroup& Out)
    {
        if (S == TEXT("TEXTUREGROUP_World"))              { Out = TEXTUREGROUP_World;              return true; }
        if (S == TEXT("TEXTUREGROUP_WorldNormalMap"))     { Out = TEXTUREGROUP_WorldNormalMap;     return true; }
        if (S == TEXT("TEXTUREGROUP_WorldSpecular"))      { Out = TEXTUREGROUP_WorldSpecular;      return true; }
        if (S == TEXT("TEXTUREGROUP_Character"))          { Out = TEXTUREGROUP_Character;          return true; }
        if (S == TEXT("TEXTUREGROUP_CharacterNormalMap")) { Out = TEXTUREGROUP_CharacterNormalMap; return true; }
        if (S == TEXT("TEXTUREGROUP_CharacterSpecular"))  { Out = TEXTUREGROUP_CharacterSpecular;  return true; }
        if (S == TEXT("TEXTUREGROUP_Weapon"))             { Out = TEXTUREGROUP_Weapon;             return true; }
        if (S == TEXT("TEXTUREGROUP_WeaponNormalMap"))    { Out = TEXTUREGROUP_WeaponNormalMap;    return true; }
        if (S == TEXT("TEXTUREGROUP_WeaponSpecular"))     { Out = TEXTUREGROUP_WeaponSpecular;     return true; }
        if (S == TEXT("TEXTUREGROUP_Vehicle"))            { Out = TEXTUREGROUP_Vehicle;            return true; }
        if (S == TEXT("TEXTUREGROUP_VehicleNormalMap"))   { Out = TEXTUREGROUP_VehicleNormalMap;   return true; }
        if (S == TEXT("TEXTUREGROUP_VehicleSpecular"))    { Out = TEXTUREGROUP_VehicleSpecular;    return true; }
        if (S == TEXT("TEXTUREGROUP_Cinematic"))          { Out = TEXTUREGROUP_Cinematic;          return true; }
        if (S == TEXT("TEXTUREGROUP_Effects"))            { Out = TEXTUREGROUP_Effects;            return true; }
        if (S == TEXT("TEXTUREGROUP_EffectsNotFiltered")) { Out = TEXTUREGROUP_EffectsNotFiltered; return true; }
        if (S == TEXT("TEXTUREGROUP_Skybox"))             { Out = TEXTUREGROUP_Skybox;             return true; }
        if (S == TEXT("TEXTUREGROUP_UI"))                 { Out = TEXTUREGROUP_UI;                 return true; }
        if (S == TEXT("TEXTUREGROUP_Lightmap"))           { Out = TEXTUREGROUP_Lightmap;           return true; }
        if (S == TEXT("TEXTUREGROUP_Shadowmap"))          { Out = TEXTUREGROUP_Shadowmap;          return true; }
        return false;
    }

    static bool ParseAddress(const FString& S, TextureAddress& Out)
    {
        if (S == TEXT("TA_Wrap") || S.Equals(TEXT("wrap"), ESearchCase::IgnoreCase))     { Out = TA_Wrap;   return true; }
        if (S == TEXT("TA_Clamp") || S.Equals(TEXT("clamp"), ESearchCase::IgnoreCase))   { Out = TA_Clamp;  return true; }
        if (S == TEXT("TA_Mirror") || S.Equals(TEXT("mirror"), ESearchCase::IgnoreCase)) { Out = TA_Mirror; return true; }
        return false;
    }

    static FString CompressionToString(TextureCompressionSettings Compression)
    {
        if (const UEnum* Enum = StaticEnum<TextureCompressionSettings>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Compression));
        }
        return FString::FromInt(static_cast<int32>(Compression));
    }

    static FString MipGenToString(TextureMipGenSettings MipGen)
    {
        if (const UEnum* Enum = StaticEnum<TextureMipGenSettings>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(MipGen));
        }
        return FString::FromInt(static_cast<int32>(MipGen));
    }

    static FString LODGroupToString(TextureGroup LODGroup)
    {
        if (const UEnum* Enum = StaticEnum<TextureGroup>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(LODGroup));
        }
        return FString::FromInt(static_cast<int32>(LODGroup));
    }

    static FString AddressToString(TextureAddress Address)
    {
        if (const UEnum* Enum = StaticEnum<TextureAddress>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Address));
        }
        return FString::FromInt(static_cast<int32>(Address));
    }

    static FString NormalizeTextureRole(FString Role)
    {
        Role.TrimStartAndEndInline();
        Role = Role.ToLower();
        Role.ReplaceInline(TEXT("-"), TEXT("_"));
        Role.ReplaceInline(TEXT(" "), TEXT("_"));
        if (Role == TEXT("default") || Role == TEXT("none"))
        {
            return TEXT("");
        }
        if (Role == TEXT("ui"))
        {
            return TEXT("ui_icon");
        }
        if (Role == TEXT("albedo") || Role == TEXT("base_color") || Role == TEXT("diffuse"))
        {
            return TEXT("basecolor");
        }
        if (Role == TEXT("world") || Role == TEXT("mesh") || Role == TEXT("material"))
        {
            return TEXT("basecolor");
        }
        if (Role == TEXT("tile") || Role == TEXT("tileable"))
        {
            return TEXT("world_tile");
        }
        if (Role == TEXT("normalmap") || Role == TEXT("normal_map"))
        {
            return TEXT("normal");
        }
        if (Role == TEXT("orm") || Role == TEXT("packed_mask") || Role == TEXT("masks")
            || Role == TEXT("roughness") || Role == TEXT("metallic") || Role == TEXT("ao"))
        {
            return TEXT("orm_mask");
        }
        if (Role == TEXT("displacement"))
        {
            return TEXT("height");
        }
        return Role;
    }

    static bool IsSupportedTextureRole(const FString& Role)
    {
        return Role.IsEmpty()
            || Role == TEXT("ui_icon")
            || Role == TEXT("sprite")
            || Role == TEXT("decal")
            || Role == TEXT("basecolor")
            || Role == TEXT("world_tile")
            || Role == TEXT("normal")
            || Role == TEXT("orm_mask")
            || Role == TEXT("height")
            || Role == TEXT("emissive");
    }

    static bool GetTextureRole(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>* SettingsObj, FString& OutRole, FString& OutError)
    {
        FString Role;
        int32 ProvidedRoleCount = 0;
        if (Params->HasField(TEXT("texture_role")))
        {
            ++ProvidedRoleCount;
            if (!Params->TryGetStringField(TEXT("texture_role"), Role) || Role.IsEmpty())
            {
                OutError = TEXT("texture_role must be a non-empty string");
                return false;
            }
        }
        if (Params->HasField(TEXT("role")))
        {
            ++ProvidedRoleCount;
            if (!Params->TryGetStringField(TEXT("role"), Role) || Role.IsEmpty())
            {
                OutError = TEXT("role must be a non-empty string");
                return false;
            }
        }
        if (SettingsObj && SettingsObj->IsValid() && (*SettingsObj)->HasField(TEXT("texture_role")))
        {
            ++ProvidedRoleCount;
            if (!(*SettingsObj)->TryGetStringField(TEXT("texture_role"), Role) || Role.IsEmpty())
            {
                OutError = TEXT("settings.texture_role must be a non-empty string");
                return false;
            }
        }
        if (ProvidedRoleCount > 1)
        {
            OutError = TEXT("Texture role was provided more than once; use exactly one of texture_role, role, or settings.texture_role");
            return false;
        }

        OutRole = NormalizeTextureRole(Role);
        if (!IsSupportedTextureRole(OutRole))
        {
            OutError = FString::Printf(
                TEXT("texture_role must be one of: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, emissive (got '%s')"),
                *Role);
            return false;
        }
        return true;
    }

    static FTextureRolePreset BuildRolePreset(const FString& Role)
    {
        FTextureRolePreset Preset;
        Preset.Role = Role;

        if (Role == TEXT("ui_icon") || Role == TEXT("sprite"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_NoMipmaps;
            Preset.LODGroup = TEXTUREGROUP_UI;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            Preset.bAlphaBleed = true;
            Preset.bAlphaFromEdgeBackground = true;
            return Preset;
        }
        if (Role == TEXT("decal"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_Effects;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            Preset.bAlphaBleed = true;
            Preset.bAlphaFromEdgeBackground = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("world_tile"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_World;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bHarmonizeTileEdges = true;
            Preset.bValidateTile = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("normal"))
        {
            Preset.Compression = TC_Normalmap;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_WorldNormalMap;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateNormal = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("orm_mask"))
        {
            Preset.Compression = TC_Masks;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_WorldSpecular;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateMask = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("height"))
        {
            Preset.Compression = TC_Grayscale;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_World;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateMask = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("emissive"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_Effects;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            return Preset;
        }

        Preset.Compression = TC_Default;
        Preset.bSRGB = true;
        Preset.MipGen = TMGS_FromTextureGroup;
        Preset.LODGroup = TEXTUREGROUP_World;
        Preset.AddressX = TA_Wrap;
        Preset.AddressY = TA_Wrap;
        Preset.bExpectPowerOfTwo = true;
        return Preset;
    }

    static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
        Warning->SetStringField(TEXT("code"), Code);
        Warning->SetStringField(TEXT("message"), Message);
        Warnings.Add(MakeShared<FJsonValueObject>(Warning));
    }

    static bool IsPowerOfTwo(int32 Value)
    {
        return Value > 0 && (Value & (Value - 1)) == 0;
    }

    static FString HashBytes(const TArray<uint8>& Bytes)
    {
        FMD5 Md5;
        if (Bytes.Num() > 0)
        {
            Md5.Update(Bytes.GetData(), Bytes.Num());
        }
        uint8 Digest[16];
        Md5.Final(Digest);
        return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
    }

    static bool EncodeRawBgraAsPng(const TArray<uint8>& RawBgra, int32 W, int32 H, TArray<uint8>& OutPngBytes)
    {
        if (W <= 0 || H <= 0 || RawBgra.Num() < static_cast<int64>(W) * static_cast<int64>(H) * 4)
        {
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!PngWrapper.IsValid() || !PngWrapper->SetRaw(RawBgra.GetData(), RawBgra.Num(), W, H, ERGBFormat::BGRA, 8))
        {
            return false;
        }

        const TArray64<uint8> PngBytes64 = PngWrapper->GetCompressed(100);
        if (PngBytes64.Num() == 0)
        {
            return false;
        }

        OutPngBytes.Reset(PngBytes64.Num());
        OutPngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
        return true;
    }

    static bool HasNonOpaqueAlpha(const TArray<uint8>& RawBgra)
    {
        for (int32 Index = 3; Index < RawBgra.Num(); Index += 4)
        {
            if (RawBgra[Index] < 255)
            {
                return true;
            }
        }
        return false;
    }

    static int32 ApplyEdgeBackgroundAlpha(TArray<uint8>& RawBgra, int32 W, int32 H)
    {
        if (W <= 0 || H <= 0 || HasNonOpaqueAlpha(RawBgra))
        {
            return 0;
        }

        int64 SumB = 0;
        int64 SumG = 0;
        int64 SumR = 0;
        int64 Samples = 0;
        auto AddSample = [&RawBgra, W, &SumB, &SumG, &SumR, &Samples](int32 X, int32 Y)
        {
            const int32 PixelIndex = (Y * W + X) * 4;
            SumB += RawBgra[PixelIndex + 0];
            SumG += RawBgra[PixelIndex + 1];
            SumR += RawBgra[PixelIndex + 2];
            ++Samples;
        };

        for (int32 X = 0; X < W; ++X)
        {
            AddSample(X, 0);
            if (H > 1)
            {
                AddSample(X, H - 1);
            }
        }
        for (int32 Y = 1; Y < H - 1; ++Y)
        {
            AddSample(0, Y);
            if (W > 1)
            {
                AddSample(W - 1, Y);
            }
        }

        if (Samples == 0)
        {
            return 0;
        }

        const int32 BackgroundB = static_cast<int32>(SumB / Samples);
        const int32 BackgroundG = static_cast<int32>(SumG / Samples);
        const int32 BackgroundR = static_cast<int32>(SumR / Samples);
        constexpr int32 TransparentThreshold = 12;
        constexpr int32 OpaqueThreshold = 54;

        const int32 PixelCount = W * H;
        TArray<uint8> CandidateAlpha;
        CandidateAlpha.SetNumUninitialized(PixelCount);
        int32 CandidatePixels = 0;
        for (int32 Pixel = 0; Pixel < PixelCount; ++Pixel)
        {
            const int32 Index = Pixel * 4;
            const int32 DeltaB = FMath::Abs(static_cast<int32>(RawBgra[Index + 0]) - BackgroundB);
            const int32 DeltaG = FMath::Abs(static_cast<int32>(RawBgra[Index + 1]) - BackgroundG);
            const int32 DeltaR = FMath::Abs(static_cast<int32>(RawBgra[Index + 2]) - BackgroundR);
            const int32 Delta = FMath::Max3(DeltaB, DeltaG, DeltaR);

            uint8 Alpha = 255;
            if (Delta <= TransparentThreshold)
            {
                Alpha = 0;
            }
            else if (Delta < OpaqueThreshold)
            {
                Alpha = static_cast<uint8>(FMath::Clamp(
                    ((Delta - TransparentThreshold) * 255) / (OpaqueThreshold - TransparentThreshold), 0, 255));
            }
            CandidateAlpha[Pixel] = Alpha;
            if (Alpha < 255)
            {
                ++CandidatePixels;
            }
        }

        if (CandidatePixels == 0)
        {
            return 0;
        }

        TArray<uint8> bBackgroundConnected;
        bBackgroundConnected.Init(0, PixelCount);

        TArray<int32> FloodQueue;
        FloodQueue.Reserve(PixelCount);
        auto TryAddCandidate = [&CandidateAlpha, &bBackgroundConnected, &FloodQueue, PixelCount](int32 Pixel)
        {
            if (Pixel < 0 || Pixel >= PixelCount || bBackgroundConnected[Pixel] != 0 || CandidateAlpha[Pixel] == 255)
            {
                return;
            }
            bBackgroundConnected[Pixel] = 1;
            FloodQueue.Add(Pixel);
        };

        for (int32 X = 0; X < W; ++X)
        {
            TryAddCandidate(X);
            if (H > 1)
            {
                TryAddCandidate((H - 1) * W + X);
            }
        }
        for (int32 Y = 1; Y < H - 1; ++Y)
        {
            TryAddCandidate(Y * W);
            if (W > 1)
            {
                TryAddCandidate(Y * W + (W - 1));
            }
        }

        for (int32 QueueIndex = 0; QueueIndex < FloodQueue.Num(); ++QueueIndex)
        {
            const int32 Pixel = FloodQueue[QueueIndex];
            const int32 X = Pixel % W;
            const int32 Y = Pixel / W;
            if (X > 0)
            {
                TryAddCandidate(Pixel - 1);
            }
            if (X + 1 < W)
            {
                TryAddCandidate(Pixel + 1);
            }
            if (Y > 0)
            {
                TryAddCandidate(Pixel - W);
            }
            if (Y + 1 < H)
            {
                TryAddCandidate(Pixel + W);
            }
        }

        if (FloodQueue.Num() == 0)
        {
            return 0;
        }

        TArray<uint8> Candidate = RawBgra;
        int32 NonOpaquePixels = 0;
        int32 OpaquePixels = 0;
        for (int32 Pixel = 0; Pixel < PixelCount; ++Pixel)
        {
            const int32 Index = Pixel * 4;
            if (bBackgroundConnected[Pixel] != 0)
            {
                Candidate[Index + 3] = CandidateAlpha[Pixel];
                ++NonOpaquePixels;
            }
            else
            {
                Candidate[Index + 3] = 255;
                ++OpaquePixels;
            }
        }

        if (OpaquePixels < FMath::Max(1, PixelCount / 100) || NonOpaquePixels == 0)
        {
            return 0;
        }

        RawBgra = MoveTemp(Candidate);
        return NonOpaquePixels;
    }

    static int32 ApplyAlphaBleed(TArray<uint8>& RawBgra, int32 W, int32 H, int32 Iterations = 2)
    {
        int32 TotalChanged = 0;
        for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
        {
            TArray<uint8> Next = RawBgra;
            int32 ChangedThisPass = 0;
            for (int32 Y = 0; Y < H; ++Y)
            {
                for (int32 X = 0; X < W; ++X)
                {
                    const int32 PixelIndex = (Y * W + X) * 4;
                    if (RawBgra[PixelIndex + 3] != 0)
                    {
                        continue;
                    }

                    int32 SumB = 0;
                    int32 SumG = 0;
                    int32 SumR = 0;
                    int32 Count = 0;
                    for (int32 DY = -1; DY <= 1; ++DY)
                    {
                        for (int32 DX = -1; DX <= 1; ++DX)
                        {
                            if (DX == 0 && DY == 0)
                            {
                                continue;
                            }
                            const int32 NX = X + DX;
                            const int32 NY = Y + DY;
                            if (NX < 0 || NX >= W || NY < 0 || NY >= H)
                            {
                                continue;
                            }
                            const int32 NeighborIndex = (NY * W + NX) * 4;
                            if (RawBgra[NeighborIndex + 3] == 0)
                            {
                                continue;
                            }
                            SumB += RawBgra[NeighborIndex + 0];
                            SumG += RawBgra[NeighborIndex + 1];
                            SumR += RawBgra[NeighborIndex + 2];
                            ++Count;
                        }
                    }

                    if (Count > 0)
                    {
                        Next[PixelIndex + 0] = static_cast<uint8>(SumB / Count);
                        Next[PixelIndex + 1] = static_cast<uint8>(SumG / Count);
                        Next[PixelIndex + 2] = static_cast<uint8>(SumR / Count);
                        ++ChangedThisPass;
                    }
                }
            }

            if (ChangedThisPass == 0)
            {
                break;
            }
            RawBgra = MoveTemp(Next);
            TotalChanged += ChangedThisPass;
        }
        return TotalChanged;
    }

    static int32 ApplyTileSeamHarmonization(TArray<uint8>& RawBgra, int32 W, int32 H)
    {
        if (W <= 1 || H <= 1)
        {
            return 0;
        }

        int32 ChangedPixels = 0;
        auto HarmonizePair = [&RawBgra, &ChangedPixels](int32 AIndex, int32 BIndex)
        {
            const uint8 OldAB = RawBgra[AIndex + 0];
            const uint8 OldAG = RawBgra[AIndex + 1];
            const uint8 OldAR = RawBgra[AIndex + 2];
            const uint8 OldAA = RawBgra[AIndex + 3];
            const uint8 OldBB = RawBgra[BIndex + 0];
            const uint8 OldBG = RawBgra[BIndex + 1];
            const uint8 OldBR = RawBgra[BIndex + 2];
            const uint8 OldBA = RawBgra[BIndex + 3];

            const uint8 NewB = static_cast<uint8>((static_cast<int32>(OldAB) + static_cast<int32>(OldBB)) / 2);
            const uint8 NewG = static_cast<uint8>((static_cast<int32>(OldAG) + static_cast<int32>(OldBG)) / 2);
            const uint8 NewR = static_cast<uint8>((static_cast<int32>(OldAR) + static_cast<int32>(OldBR)) / 2);
            const uint8 NewA = static_cast<uint8>((static_cast<int32>(OldAA) + static_cast<int32>(OldBA)) / 2);

            const bool bChanged =
                OldAB != NewB || OldAG != NewG || OldAR != NewR || OldAA != NewA ||
                OldBB != NewB || OldBG != NewG || OldBR != NewR || OldBA != NewA;

            RawBgra[AIndex + 0] = NewB;
            RawBgra[AIndex + 1] = NewG;
            RawBgra[AIndex + 2] = NewR;
            RawBgra[AIndex + 3] = NewA;
            RawBgra[BIndex + 0] = NewB;
            RawBgra[BIndex + 1] = NewG;
            RawBgra[BIndex + 2] = NewR;
            RawBgra[BIndex + 3] = NewA;

            if (bChanged)
            {
                ChangedPixels += 2;
            }
        };

        for (int32 Y = 0; Y < H; ++Y)
        {
            HarmonizePair((Y * W + 0) * 4, (Y * W + (W - 1)) * 4);
        }
        for (int32 X = 0; X < W; ++X)
        {
            HarmonizePair((0 * W + X) * 4, ((H - 1) * W + X) * 4);
        }

        return ChangedPixels;
    }

    static TSharedPtr<FJsonObject> BuildAppliedSettingsJson(
        TextureCompressionSettings Compression,
        bool bSRGB,
        TextureMipGenSettings MipGen,
        TextureGroup LODGroup,
        TextureAddress AddressX,
        TextureAddress AddressY)
    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(TEXT("compression_settings"), CompressionToString(Compression));
        Settings->SetBoolField(TEXT("srgb"), bSRGB);
        Settings->SetStringField(TEXT("mip_gen_settings"), MipGenToString(MipGen));
        Settings->SetStringField(TEXT("lod_group"), LODGroupToString(LODGroup));
        Settings->SetStringField(TEXT("address_x"), AddressToString(AddressX));
        Settings->SetStringField(TEXT("address_y"), AddressToString(AddressY));
        return Settings;
    }

    static TSharedPtr<FJsonObject> BuildTextureValidationJson(
        const FString& Role,
        const TArray<uint8>& RawBgra,
        int32 W,
        int32 H,
        const FTextureRolePreset& Preset,
        int32 AlphaFromBackgroundPixels,
        int32 AlphaBleedPixels,
        int32 TileSeamPixels)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        const int64 PixelCount = static_cast<int64>(W) * static_cast<int64>(H);
        int64 TransparentPixels = 0;
        int64 NonOpaquePixels = 0;

        uint8 MinR = 255, MinG = 255, MinB = 255, MinA = 255;
        uint8 MaxR = 0, MaxG = 0, MaxB = 0, MaxA = 0;
        double SumR = 0.0, SumG = 0.0, SumB = 0.0, SumA = 0.0;

        int64 BadNormalPixels = 0;
        int64 LowBlueNormalPixels = 0;
        for (int64 Pixel = 0; Pixel < PixelCount; ++Pixel)
        {
            const int64 Index = Pixel * 4;
            const uint8 B = RawBgra[Index + 0];
            const uint8 G = RawBgra[Index + 1];
            const uint8 R = RawBgra[Index + 2];
            const uint8 A = RawBgra[Index + 3];

            MinR = FMath::Min(MinR, R); MaxR = FMath::Max(MaxR, R);
            MinG = FMath::Min(MinG, G); MaxG = FMath::Max(MaxG, G);
            MinB = FMath::Min(MinB, B); MaxB = FMath::Max(MaxB, B);
            MinA = FMath::Min(MinA, A); MaxA = FMath::Max(MaxA, A);
            SumR += R; SumG += G; SumB += B; SumA += A;

            if (A == 0)
            {
                ++TransparentPixels;
            }
            if (A < 255)
            {
                ++NonOpaquePixels;
            }

            if (Preset.bValidateNormal)
            {
                const double NX = (static_cast<double>(R) / 127.5) - 1.0;
                const double NY = (static_cast<double>(G) / 127.5) - 1.0;
                const double NZ = (static_cast<double>(B) / 127.5) - 1.0;
                const double Length = FMath::Sqrt(NX * NX + NY * NY + NZ * NZ);
                if (Length < 0.5 || Length > 1.5)
                {
                    ++BadNormalPixels;
                }
                if (B < 128)
                {
                    ++LowBlueNormalPixels;
                }
            }
        }

        if (Preset.bExpectPowerOfTwo && (!IsPowerOfTwo(W) || !IsPowerOfTwo(H)))
        {
            AddWarning(Warnings, TEXT("non_power_of_two"), TEXT("Role expects power-of-two dimensions for stable mips and streaming."));
        }

        if (Preset.bValidateNormal)
        {
            const double BadRatio = PixelCount > 0 ? static_cast<double>(BadNormalPixels) / static_cast<double>(PixelCount) : 0.0;
            const double LowBlueRatio = PixelCount > 0 ? static_cast<double>(LowBlueNormalPixels) / static_cast<double>(PixelCount) : 0.0;
            if (BadRatio > 0.25)
            {
                AddWarning(Warnings, TEXT("normal_length_suspicious"), TEXT("Many pixels do not decode to plausible tangent-space normal vectors."));
            }
            if (LowBlueRatio > 0.25)
            {
                AddWarning(Warnings, TEXT("normal_blue_channel_suspicious"), TEXT("Normal maps usually have a mostly high blue channel."));
            }
        }

        if (Preset.bValidateMask)
        {
            if (MaxR == MinR && MaxG == MinG && MaxB == MinB)
            {
                AddWarning(Warnings, TEXT("mask_low_dynamic_range"), TEXT("Packed mask role has no visible RGB channel variation."));
            }
        }

        double TileAverageDelta = 0.0;
        double TileMaxDelta = 0.0;
        if (Preset.bValidateTile && W > 1 && H > 1)
        {
            int64 EdgeSamples = 0;
            double SumDelta = 0.0;
            auto AccumulateDelta = [&RawBgra, &SumDelta, &TileMaxDelta, &EdgeSamples](int32 AIndex, int32 BIndex)
            {
                const double DB = FMath::Abs(static_cast<double>(RawBgra[AIndex + 0]) - static_cast<double>(RawBgra[BIndex + 0]));
                const double DG = FMath::Abs(static_cast<double>(RawBgra[AIndex + 1]) - static_cast<double>(RawBgra[BIndex + 1]));
                const double DR = FMath::Abs(static_cast<double>(RawBgra[AIndex + 2]) - static_cast<double>(RawBgra[BIndex + 2]));
                const double Delta = (DB + DG + DR) / 3.0;
                SumDelta += Delta;
                TileMaxDelta = FMath::Max(TileMaxDelta, Delta);
                ++EdgeSamples;
            };

            for (int32 Y = 0; Y < H; ++Y)
            {
                AccumulateDelta((Y * W + 0) * 4, (Y * W + (W - 1)) * 4);
            }
            for (int32 X = 0; X < W; ++X)
            {
                AccumulateDelta((0 * W + X) * 4, ((H - 1) * W + X) * 4);
            }

            TileAverageDelta = EdgeSamples > 0 ? SumDelta / static_cast<double>(EdgeSamples) : 0.0;
            if (TileAverageDelta > 8.0 || TileMaxDelta > 32.0)
            {
                AddWarning(Warnings, TEXT("tile_edge_mismatch"), TEXT("Opposite image edges differ enough to show seams when wrapped."));
            }
        }

        TSharedPtr<FJsonObject> ChannelStats = MakeShared<FJsonObject>();
        auto AddStats = [PixelCount, &ChannelStats](const TCHAR* Name, uint8 Min, uint8 Max, double Sum)
        {
            TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
            Channel->SetNumberField(TEXT("min"), Min);
            Channel->SetNumberField(TEXT("max"), Max);
            Channel->SetNumberField(TEXT("mean"), PixelCount > 0 ? Sum / static_cast<double>(PixelCount) : 0.0);
            ChannelStats->SetObjectField(Name, Channel);
        };
        AddStats(TEXT("r"), MinR, MaxR, SumR);
        AddStats(TEXT("g"), MinG, MaxG, SumG);
        AddStats(TEXT("b"), MinB, MaxB, SumB);
        AddStats(TEXT("a"), MinA, MaxA, SumA);

        TSharedPtr<FJsonObject> PostProcess = MakeShared<FJsonObject>();
        PostProcess->SetNumberField(TEXT("alpha_from_edge_background_pixels"), AlphaFromBackgroundPixels);
        PostProcess->SetNumberField(TEXT("alpha_bleed_pixels"), AlphaBleedPixels);
        PostProcess->SetNumberField(TEXT("tile_seam_harmonized_pixels"), TileSeamPixels);

        TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
        Validation->SetStringField(TEXT("texture_role"), Role.IsEmpty() ? TEXT("default") : Role);
        Validation->SetBoolField(TEXT("passed"), Warnings.Num() == 0);
        Validation->SetBoolField(TEXT("has_alpha"), NonOpaquePixels > 0);
        Validation->SetNumberField(TEXT("alpha_coverage"), PixelCount > 0 ? 1.0 - (static_cast<double>(TransparentPixels) / static_cast<double>(PixelCount)) : 0.0);
        Validation->SetNumberField(TEXT("non_opaque_pixels"), static_cast<double>(NonOpaquePixels));
        Validation->SetObjectField(TEXT("channels"), ChannelStats);
        Validation->SetObjectField(TEXT("postprocess"), PostProcess);
        if (Preset.bValidateTile)
        {
            TSharedPtr<FJsonObject> Tile = MakeShared<FJsonObject>();
            Tile->SetNumberField(TEXT("edge_average_delta"), TileAverageDelta);
            Tile->SetNumberField(TEXT("edge_max_delta"), TileMaxDelta);
            Validation->SetObjectField(TEXT("tile"), Tile);
        }
        Validation->SetArrayField(TEXT("warnings"), Warnings);
        return Validation;
    }
} // namespace MonolithAsset::TextureIngestInternal

FMonolithActionResult MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithAsset::TextureIngestInternal;

    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
    }

    // --- Required params ---
    FString Destination;
    if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: destination"), -32602);
    }
    if (!Destination.StartsWith(TEXT("/")))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("destination must be a long package path like /Game/Foo/Bar (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT(".uasset")))
    {
        Destination = Destination.LeftChop(7);
    }

    FString RequestedAssetPath = Destination;
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(RequestedAssetPath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, -32602);
    }

    const FString RequestedAssetName = FPackageName::GetLongPackageAssetName(RequestedAssetPath);
    if (RequestedAssetName.IsEmpty())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("destination must include an asset name (got '%s')"), *RequestedAssetPath),
            -32602);
    }

    ETextureConflictPolicy ConflictPolicy = ETextureConflictPolicy::Fail;
    FString ConflictPolicyName;
    FString ConflictPolicyError;
    if (!ParseConflictPolicy(Params, ConflictPolicy, ConflictPolicyName, ConflictPolicyError))
    {
        return FMonolithActionResult::Error(ConflictPolicyError, -32602);
    }

    const bool bRequestedPackageExistsOnDisk = FPackageName::DoesPackageExist(RequestedAssetPath);
    const bool bRequestedPackageIsLoaded = FindPackage(nullptr, *RequestedAssetPath) != nullptr;
    const bool bRequestedPackageExists = bRequestedPackageExistsOnDisk || bRequestedPackageIsLoaded;

    UTexture2D* ExistingTexture = nullptr;
    if (ConflictPolicy == ETextureConflictPolicy::Fail && bRequestedPackageExists)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Asset package '%s' already exists; conflict_policy=fail never changes the requested path"),
                *RequestedAssetPath),
            -32602);
    }

    if (ConflictPolicy == ETextureConflictPolicy::Replace && bRequestedPackageExists)
    {
        UObject* ExistingObject = FindOrLoadAssetAtPackagePath(RequestedAssetPath, bRequestedPackageExistsOnDisk);
        if (!ExistingObject)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Cannot replace '%s': the package exists but its exact top-level asset could not be loaded"),
                    *RequestedAssetPath),
                -32602);
        }

        ExistingTexture = Cast<UTexture2D>(ExistingObject);
        if (!ExistingTexture)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Cannot replace '%s': existing object is '%s', not UTexture2D"),
                    *RequestedAssetPath,
                    *ExistingObject->GetClass()->GetName()),
                -32602);
        }

        if (ExistingTexture->GetOutermost()->GetName() != RequestedAssetPath
            || ExistingTexture->GetName() != RequestedAssetName)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Cannot replace '%s': loading resolved to '%s' instead of the exact requested asset identity"),
                    *RequestedAssetPath,
                    *ExistingTexture->GetPathName()),
                -32602);
        }
    }

    FString BytesB64;
    if (!Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) || BytesB64.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: bytes_b64"), -32602);
    }

    FString FormatHint;
    if (!Params->TryGetStringField(TEXT("format_hint"), FormatHint) || FormatHint.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: format_hint"), -32602);
    }

    const EImageFormat Format = ParseFormatHint(FormatHint);
    if (Format == EImageFormat::Invalid)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unknown format_hint '%s' (supported: png, jpg, jpeg, bmp, tga, tif, tiff, dds)"), *FormatHint),
            -32602);
    }

    // This importer decodes through GetRaw(BGRA, 8) and stores TSF_BGRA8, which
    // cannot represent the floating-point range these formats exist to carry.
    // Accepting them produced a successful import whose values were silently
    // clamped and quantised, giving wrong lighting and emissive data. Reject them
    // rather than report success over destroyed precision.
    if (Format == EImageFormat::EXR || Format == EImageFormat::HDR)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("format_hint '%s' is a high-dynamic-range format; this action decodes to 8-bit BGRA and would silently lose precision. Import HDR sources through the Interchange pipeline instead."),
                *FormatHint),
            -32602);
    }

    bool bSave = true;
    if (Params->HasField(TEXT("save"))
        && (!Params->HasTypedField<EJson::Boolean>(TEXT("save"))
            || !Params->TryGetBoolField(TEXT("save"), bSave)))
    {
        return FMonolithActionResult::Error(TEXT("save must be a boolean"), -32602);
    }
    bool bReturnProcessedPng = false;
    if (Params->HasField(TEXT("return_processed_png"))
        && (!Params->HasTypedField<EJson::Boolean>(TEXT("return_processed_png"))
            || !Params->TryGetBoolField(TEXT("return_processed_png"), bReturnProcessedPng)))
    {
        return FMonolithActionResult::Error(TEXT("return_processed_png must be a boolean"), -32602);
    }

    TextureCompressionSettings Compression = TC_Default;
    bool bSRGB = true;
    TextureMipGenSettings MipGen = TMGS_NoMipmaps;
    TextureGroup LODGroup = TEXTUREGROUP_UI;
    TextureAddress AddressX = TA_Clamp;
    TextureAddress AddressY = TA_Clamp;
    bool bAlphaBleed = false;
    bool bAlphaFromEdgeBackground = false;
    bool bHarmonizeTileEdges = false;

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (Params->HasField(TEXT("settings"))
        && (!Params->TryGetObjectField(TEXT("settings"), SettingsObj)
            || !SettingsObj
            || !SettingsObj->IsValid()))
    {
        return FMonolithActionResult::Error(TEXT("settings must be an object"), -32602);
    }

    if (SettingsObj && SettingsObj->IsValid())
    {
        static const TSet<FString> AllowedSettings = {
            TEXT("compression_settings"),
            TEXT("srgb"),
            TEXT("mip_gen_settings"),
            TEXT("lod_group"),
            TEXT("address_x"),
            TEXT("address_y"),
            TEXT("alpha_bleed"),
            TEXT("alpha_from_edge_background"),
            TEXT("tile_seam_harmonize"),
            TEXT("seam_harmonize"),
            TEXT("texture_role")
        };
        for (const auto& Pair : (*SettingsObj)->Values)
        {
            const FString Key = MonolithKeyToString(Pair.Key);
            if (!AllowedSettings.Contains(Key))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("Unknown texture setting '%s'"), *Key),
                    -32602);
            }
        }
        if ((*SettingsObj)->HasField(TEXT("tile_seam_harmonize"))
            && (*SettingsObj)->HasField(TEXT("seam_harmonize")))
        {
            return FMonolithActionResult::Error(
                TEXT("Use exactly one of settings.tile_seam_harmonize or settings.seam_harmonize"),
                -32602);
        }
    }

    FString TextureRole;
    FString TextureRoleError;
    if (!GetTextureRole(Params, SettingsObj, TextureRole, TextureRoleError))
    {
        return FMonolithActionResult::Error(TextureRoleError, -32602);
    }
    FTextureRolePreset RolePreset = BuildRolePreset(TextureRole);
    if (!TextureRole.IsEmpty())
    {
        Compression = RolePreset.Compression;
        bSRGB = RolePreset.bSRGB;
        MipGen = RolePreset.MipGen;
        LODGroup = RolePreset.LODGroup;
        AddressX = RolePreset.AddressX;
        AddressY = RolePreset.AddressY;
        bAlphaBleed = RolePreset.bAlphaBleed;
        bAlphaFromEdgeBackground = RolePreset.bAlphaFromEdgeBackground;
        bHarmonizeTileEdges = RolePreset.bHarmonizeTileEdges;
    }

    if (SettingsObj && SettingsObj->IsValid())
    {
        FString CompressionStr;
        if ((*SettingsObj)->HasField(TEXT("compression_settings"))
            && (!(*SettingsObj)->TryGetStringField(TEXT("compression_settings"), CompressionStr)
                || !ParseCompression(CompressionStr, Compression)))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("settings.compression_settings is invalid: '%s'"),
                    *CompressionStr),
                -32602);
        }

        bool bSRGBValue = true;
        if ((*SettingsObj)->HasField(TEXT("srgb")))
        {
            if (!(*SettingsObj)->HasTypedField<EJson::Boolean>(TEXT("srgb"))
                || !(*SettingsObj)->TryGetBoolField(TEXT("srgb"), bSRGBValue))
            {
                return FMonolithActionResult::Error(TEXT("settings.srgb must be a boolean"), -32602);
            }
            bSRGB = bSRGBValue;
        }

        FString MipGenStr;
        if ((*SettingsObj)->HasField(TEXT("mip_gen_settings"))
            && (!(*SettingsObj)->TryGetStringField(TEXT("mip_gen_settings"), MipGenStr)
                || !ParseMipGen(MipGenStr, MipGen)))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("settings.mip_gen_settings is invalid: '%s'"), *MipGenStr),
                -32602);
        }

        FString LODGroupStr;
        if ((*SettingsObj)->HasField(TEXT("lod_group"))
            && (!(*SettingsObj)->TryGetStringField(TEXT("lod_group"), LODGroupStr)
                || !ParseLODGroup(LODGroupStr, LODGroup)))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("settings.lod_group is invalid: '%s'"), *LODGroupStr),
                -32602);
        }

        FString AddressXStr;
        if ((*SettingsObj)->HasField(TEXT("address_x"))
            && (!(*SettingsObj)->TryGetStringField(TEXT("address_x"), AddressXStr)
                || !ParseAddress(AddressXStr, AddressX)))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("settings.address_x is invalid: '%s'"), *AddressXStr),
                -32602);
        }

        FString AddressYStr;
        if ((*SettingsObj)->HasField(TEXT("address_y"))
            && (!(*SettingsObj)->TryGetStringField(TEXT("address_y"), AddressYStr)
                || !ParseAddress(AddressYStr, AddressY)))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("settings.address_y is invalid: '%s'"), *AddressYStr),
                -32602);
        }

        bool bAlphaBleedValue = false;
        if ((*SettingsObj)->HasField(TEXT("alpha_bleed")))
        {
            if (!(*SettingsObj)->HasTypedField<EJson::Boolean>(TEXT("alpha_bleed"))
                || !(*SettingsObj)->TryGetBoolField(TEXT("alpha_bleed"), bAlphaBleedValue))
            {
                return FMonolithActionResult::Error(TEXT("settings.alpha_bleed must be a boolean"), -32602);
            }
            bAlphaBleed = bAlphaBleedValue;
        }

        bool bAlphaFromBackgroundValue = false;
        if ((*SettingsObj)->HasField(TEXT("alpha_from_edge_background")))
        {
            if (!(*SettingsObj)->HasTypedField<EJson::Boolean>(TEXT("alpha_from_edge_background"))
                || !(*SettingsObj)->TryGetBoolField(TEXT("alpha_from_edge_background"), bAlphaFromBackgroundValue))
            {
                return FMonolithActionResult::Error(TEXT("settings.alpha_from_edge_background must be a boolean"), -32602);
            }
            bAlphaFromEdgeBackground = bAlphaFromBackgroundValue;
        }

        bool bHarmonizeTileEdgesValue = false;
        const TCHAR* HarmonizeField = (*SettingsObj)->HasField(TEXT("tile_seam_harmonize"))
            ? TEXT("tile_seam_harmonize")
            : ((*SettingsObj)->HasField(TEXT("seam_harmonize")) ? TEXT("seam_harmonize") : nullptr);
        if (HarmonizeField)
        {
            if (!(*SettingsObj)->HasTypedField<EJson::Boolean>(HarmonizeField)
                || !(*SettingsObj)->TryGetBoolField(HarmonizeField, bHarmonizeTileEdgesValue))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("settings.%s must be a boolean"), HarmonizeField),
                    -32602);
            }
            bHarmonizeTileEdges = bHarmonizeTileEdgesValue;
        }
    }

    // Reject the payload before FBase64 allocates its output buffer. The exact
    // size helper accounts for '=' padding and divides before multiplying, so
    // it neither rejects an in-budget padded payload nor overflows for a large
    // FString length like GetMaxDecodedDataSize's uint32 multiplication can.
    const uint32 DecodedCompressedSize = FBase64::GetDecodedDataSize(BytesB64);
    if (static_cast<int64>(DecodedCompressedSize) > MaxCompressedImageBytes)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("bytes_b64 exceeds the compressed image byte limit of %lld"),
                MaxCompressedImageBytes),
            -32602);
    }

    // --- Base64 decode ---
    TArray<uint8> CompressedBytes;
    if (!FBase64::Decode(BytesB64, CompressedBytes) || CompressedBytes.Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("Base64 decode of bytes_b64 failed or produced empty buffer"), -32602);
    }
    if (static_cast<int64>(CompressedBytes.Num()) > MaxCompressedImageBytes)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Decoded bytes_b64 exceeds the compressed image byte limit of %lld"),
                MaxCompressedImageBytes),
            -32602);
    }

    // --- Image wrapper: decode compressed bytes to raw BGRA8 ---
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

    TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
    if (!Wrapper.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create IImageWrapper for format '%s'"), *FormatHint),
            -32603);
    }

    if (!Wrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("IImageWrapper::SetCompressed failed for '%s' bytes"), *FormatHint),
            -32603);
    }

    // IImageWrapper guarantees that header information is available after
    // SetCompressed and that decompression is deferred until GetRaw. Bound the
    // allocation using the header before requesting any raw pixels.
    const int64 HeaderWidth = Wrapper->GetWidth();
    const int64 HeaderHeight = Wrapper->GetHeight();
    int64 ExpectedBytes = 0;
    FString DecodeBoundsError;
    if (!ValidateDecodedImageBounds(HeaderWidth, HeaderHeight, ExpectedBytes, DecodeBoundsError))
    {
        return FMonolithActionResult::Error(DecodeBoundsError, -32602);
    }

    const int32 W = static_cast<int32>(HeaderWidth);
    const int32 H = static_cast<int32>(HeaderHeight);

    // GetRaw(BGRA, 8) is the documented happy path for 8-bit PNG/JPEG/BMP input;
    // wrapper implementations handle the RGBA<->BGRA swizzle internally.
    TArray<uint8> RawBgra;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, /*BitDepth=*/8, RawBgra) || RawBgra.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("IImageWrapper::GetRaw(ERGBFormat::BGRA, 8) failed or produced empty buffer"),
            -32603);
    }

    if (static_cast<int64>(RawBgra.Num()) != ExpectedBytes)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Decoded pixel buffer size mismatch: got %d, expected %lld for %dx%d BGRA8"),
                RawBgra.Num(), ExpectedBytes, W, H),
            -32603);
    }

    const int32 AlphaFromBackgroundPixels = bAlphaFromEdgeBackground ? ApplyEdgeBackgroundAlpha(RawBgra, W, H) : 0;
    const int32 AlphaBleedPixels = bAlphaBleed ? ApplyAlphaBleed(RawBgra, W, H) : 0;
    const int32 TileSeamPixels = bHarmonizeTileEdges ? ApplyTileSeamHarmonization(RawBgra, W, H) : 0;
    TSharedPtr<FJsonObject> Validation = BuildTextureValidationJson(
        TextureRole, RawBgra, W, H, RolePreset, AlphaFromBackgroundPixels, AlphaBleedPixels, TileSeamPixels);

    // Every operation that can fail independently of UObject mutation is completed
    // before touching an existing texture. This keeps replace atomic even when the
    // caller requests the processed PNG payload.
    TArray<uint8> ProcessedPngBytes;
    if (bReturnProcessedPng && !EncodeRawBgraAsPng(RawBgra, W, H, ProcessedPngBytes))
    {
        return FMonolithActionResult::Error(TEXT("Failed to encode postprocessed texture pixels as PNG"), -32603);
    }

    // Resolve the output name. Only the explicit unique policy is allowed to
    // change the caller's requested package path.
    FString ResolvedPackageName = RequestedAssetPath;
    FString ResolvedAssetName = RequestedAssetName;
    if (ConflictPolicy == ETextureConflictPolicy::Unique)
    {
        FAssetToolsModule& AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        AssetToolsModule.Get().CreateUniqueAssetName(
            RequestedAssetPath,
            /*Suffix=*/FString(),
            /*out*/ ResolvedPackageName,
            /*out*/ ResolvedAssetName);
    }

    if (const FString ValidationError = MonolithCore::ValidatePackagePath(ResolvedPackageName); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, -32603);
    }

    // Build replacement platform data completely before mutating an existing
    // texture. SetPlatformData owns the released allocation and deletes the old
    // platform data when replacing in place in both UE 5.7 and UE 5.8.
    TUniquePtr<FTexturePlatformData> PlatformData = MakeUnique<FTexturePlatformData>();
    PlatformData->SizeX = W;
    PlatformData->SizeY = H;
    PlatformData->PixelFormat = PF_B8G8R8A8;
    PlatformData->SetNumSlices(1);

    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    Mip->SizeX = W;
    Mip->SizeY = H;
    PlatformData->Mips.Add(Mip);

    Mip->BulkData.Lock(LOCK_READ_WRITE);
    void* MipData = Mip->BulkData.Realloc(ExpectedBytes);
    FMemory::Memcpy(MipData, RawBgra.GetData(), ExpectedBytes);
    Mip->BulkData.Unlock();

    const bool bReplacing = ExistingTexture != nullptr;
    const bool bCreated = !bReplacing;

    UPackage* Package = nullptr;
    UTexture2D* Texture = ExistingTexture;
    TUniquePtr<FTextureReplacementSnapshot> ReplacementSnapshot;
    if (bReplacing)
    {
        Package = ExistingTexture->GetOutermost();
        Package->FullyLoad();
        ReplacementSnapshot = MakeUnique<FTextureReplacementSnapshot>();
        FString SnapshotError;
        if (!ReplacementSnapshot->Capture(Texture, SnapshotError))
        {
            return FMonolithActionResult::Error(SnapshotError, -32603);
        }
    }
    else
    {
        Package = CreatePackage(*ResolvedPackageName);
        if (!Package)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to create package '%s'"), *ResolvedPackageName),
                -32603);
        }
        Package->FullyLoad();

        if (UObject* ConflictingObject = FindObject<UObject>(Package, *ResolvedAssetName))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Asset '%s' appeared while resolving conflict_policy=%s"),
                    *ConflictingObject->GetPathName(),
                    *ConflictPolicyName),
                -32603);
        }

        Texture = NewObject<UTexture2D>(
            Package,
            FName(*ResolvedAssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Texture)
        {
            return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D object"), -32603);
        }
    }

    Texture->PreEditChange(nullptr);
    if (ReplacementSnapshot.IsValid())
    {
        ReplacementSnapshot->InstallReplacementPlatformData(PlatformData);
    }
    else
    {
        Texture->SetPlatformData(PlatformData.Release());
    }

    // --- Source data (editor side -- required for save-to-disk) ---
#if WITH_EDITOR
    Texture->Source.Init(
        W,
        H,
        /*NumSlices=*/1,
        /*NumMips=*/1,
        TSF_BGRA8,
        RawBgra.GetData());
#endif // WITH_EDITOR
    Texture->PostEditChange();

    // Dispatch each reflected setting as its own edit. UTexture and UTexture2D
    // have property-specific material/resource invalidation paths that a single
    // generic callback does not faithfully exercise.
    ApplyTextureSettings(
        Texture,
        Compression,
        bSRGB,
        MipGen,
        LODGroup,
        AddressX,
        AddressY);

    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Texture);
    }

    Texture->MarkPackageDirty();

    if (bSave)
    {
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);
        if (!bSaved)
        {
            FString RollbackError;
            if (ReplacementSnapshot.IsValid())
            {
                ReplacementSnapshot->Restore(RollbackError);
            }
            else
            {
                RollbackCreatedTexture(Texture, ResolvedPackageName, RollbackError);
            }
            const FString RollbackSuffix = RollbackError.IsEmpty()
                ? FString()
                : FString::Printf(TEXT("; %s"), *RollbackError);
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("UPackage::SavePackage failed for '%s'%s"),
                    *PackageFilename,
                    *RollbackSuffix),
                -32603);
        }
    }

    if (ReplacementSnapshot.IsValid())
    {
        ReplacementSnapshot->Commit();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("requested_asset_path"), RequestedAssetPath);
    ResultObj->SetStringField(TEXT("asset_path"), ResolvedPackageName);
    ResultObj->SetBoolField(TEXT("created"), bCreated);
    ResultObj->SetBoolField(TEXT("replaced"), bReplacing);
    ResultObj->SetStringField(TEXT("conflict_policy"), ConflictPolicyName);
    ResultObj->SetNumberField(TEXT("width"), (double)W);
    ResultObj->SetNumberField(TEXT("height"), (double)H);
    ResultObj->SetNumberField(TEXT("size_bytes"), (double)ExpectedBytes);
    ResultObj->SetStringField(TEXT("texture_role"), TextureRole.IsEmpty() ? TEXT("default") : TextureRole);
    ResultObj->SetObjectField(
        TEXT("settings_applied"),
        BuildAppliedSettingsJson(
            Texture->CompressionSettings,
            Texture->SRGB,
            Texture->MipGenSettings,
            Texture->LODGroup,
            Texture->AddressX,
            Texture->AddressY));
    ResultObj->SetObjectField(TEXT("validation"), Validation);
    if (bReturnProcessedPng)
    {
        ResultObj->SetStringField(TEXT("processed_png_b64"), FBase64::Encode(ProcessedPngBytes));
        ResultObj->SetNumberField(TEXT("processed_png_bytes"), ProcessedPngBytes.Num());
        ResultObj->SetStringField(TEXT("processed_png_hash"), HashBytes(ProcessedPngBytes));
    }
    return FMonolithActionResult::Success(ResultObj);
}

void MonolithAsset::FTextureIngestActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("asset"),
        TEXT("import_texture_from_bytes"),
        TEXT("Decode a base64-encoded image (PNG / JPEG / BMP / TGA / TIFF / DDS) and import it as a UTexture2D asset. "
             "High-dynamic-range EXR and HDR inputs are rejected because this path decodes to 8-bit BGRA. "
             "Params: destination (string, required, /Game/... path without .uasset), "
             "bytes_b64 (string, required, base64 image bytes), "
             "format_hint (string, required, one of png|jpg|jpeg|bmp|tga|tif|tiff|dds), "
             "texture_role (string, optional: ui_icon|sprite|decal|basecolor|world_tile|normal|orm_mask|height|emissive), "
             "settings (object, optional: compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed, alpha_from_edge_background, tile_seam_harmonize), "
             "conflict_policy (string, optional: fail|replace|unique, default fail), "
             "save (bool, optional, default true), return_processed_png (bool, optional)."),
        FMonolithActionHandler::CreateStatic(&MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes),
        FParamSchemaBuilder()
            .Required(TEXT("destination"), TEXT("string"), TEXT("Output texture path without .uasset"))
            .Required(TEXT("bytes_b64"), TEXT("string"), TEXT("Base64-encoded image bytes"))
            .Required(TEXT("format_hint"), TEXT("string"), TEXT("png, jpg, jpeg, bmp, tga, tif, tiff, or dds"))
            .Optional(
                TEXT("texture_role"),
                TEXT("string"),
                TEXT("Unreal texture role preset: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive"),
                { TEXT("role") })
            .Optional(TEXT("settings"), TEXT("object"), TEXT("Texture settings such as compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed, alpha_from_edge_background, tile_seam_harmonize"))
            .Optional(TEXT("conflict_policy"), TEXT("string"), TEXT("Existing destination handling: fail, replace, or unique"), TEXT("fail"))
            .Optional(TEXT("save"), TEXT("bool"), TEXT("Save the texture asset"), TEXT("true"))
            .Optional(TEXT("return_processed_png"), TEXT("bool"), TEXT("Return postprocessed imported pixels re-encoded as PNG"), TEXT("false"))
            .StrictComplexTypes()
            .Build());
}
