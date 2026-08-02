// Copyright tumourlove. All Rights Reserved.
#include "MonolithAssetFontIngestActions.h"
#include "MonolithAssetFontIngestInternal.h"

// Monolith registry
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

// Core / JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/UnrealMemory.h"
#include "HAL/FileManager.h"                 // IFileManager
#include "Misc/PackageName.h"                   // FPackageName::LongPackageNameToFilename / GetAssetPackageExtension
#include "Misc/Paths.h"                         // FPaths path helpers
#include "Serialization/Archive.h"              // FArchive
#include "UObject/Package.h"                    // UPackage, SavePackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/UObjectGlobals.h"             // CreatePackage, NewObject

// Font core
#include "Engine/Font.h"                        // UFont, EFontCacheType, ERuntimeFontSource
#include "Engine/FontFace.h"                    // UFontFace
#include "Fonts/CompositeFont.h"                // FFontFaceData, FFontData, FTypefaceEntry, FCompositeFont, EFontHinting, EFontLoadingPolicy

// Asset registry + asset tools (unique naming)
#include "AssetRegistry/AssetRegistryModule.h"  // FAssetRegistryModule::AssetCreated
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"                   // FAssetToolsModule
#include "IAssetTools.h"                        // IAssetTools::CreateUniqueAssetName
#include "Modules/ModuleManager.h"              // FModuleManager::LoadModuleChecked
#include "UObject/SoftObjectPath.h"

// Slate (headless guard)
#include "Framework/Application/SlateApplication.h" // FSlateApplication::IsInitialized

namespace MonolithAsset::FontIngestInternal
{
    bool ValidateFontFaceCount(int32 FaceCount, FString& OutError)
    {
        if (FaceCount <= 0)
        {
            OutError = TEXT("faces must be a non-empty array");
            return false;
        }
        if (FaceCount > MaxFontFacesPerFamily)
        {
            OutError = FString::Printf(
                TEXT("faces contains %d entries; at most %d font faces are allowed per family"),
                FaceCount,
                MaxFontFacesPerFamily);
            return false;
        }
        return true;
    }

    bool AccumulateFontSourceSize(
        int64 SourceSize,
        int64& InOutFamilySourceBytes,
        FString& OutError)
    {
        if (SourceSize <= 0)
        {
            OutError = TEXT("font source file is empty");
            return false;
        }
        if (SourceSize > MaxFontFaceSourceBytes)
        {
            OutError = FString::Printf(
                TEXT("font source file is %lld bytes; the per-face limit is %lld bytes"),
                SourceSize,
                MaxFontFaceSourceBytes);
            return false;
        }
        if (InOutFamilySourceBytes > MaxFontFamilySourceBytes - SourceSize)
        {
            OutError = FString::Printf(
                TEXT("font family source payload exceeds the %lld-byte aggregate limit"),
                MaxFontFamilySourceBytes);
            return false;
        }

        InOutFamilySourceBytes += SourceSize;
        return true;
    }

    bool LoadBoundedFontFile(
        const FString& SourcePath,
        int64& InOutFamilySourceBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        OutBytes.Reset();

        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*SourcePath, FILEREAD_Silent));
        if (!Reader)
        {
            OutError = TEXT("file does not exist or is unreadable");
            return false;
        }

        const int64 SourceSize = Reader->TotalSize();
        if (!AccumulateFontSourceSize(SourceSize, InOutFamilySourceBytes, OutError))
        {
            return false;
        }

        OutBytes.SetNumUninitialized(static_cast<int32>(SourceSize));
        Reader->Serialize(OutBytes.GetData(), SourceSize);
        const bool bClosed = Reader->Close();
        if (Reader->IsError() || !bClosed)
        {
            InOutFamilySourceBytes -= SourceSize;
            OutBytes.Reset();
            OutError = TEXT("file could not be read completely");
            return false;
        }
        return true;
    }

    /**
     * Validates the sfnt container header of a font payload.
     *
     * A .ttf suffix and a nonzero byte count are not evidence that a file is a
     * font: arbitrary data renamed to .ttf would otherwise be wrapped in
     * FFontFaceData and saved as a family whose faces cannot render. This checks
     * the sfnt version tag and that the declared table directory actually fits in
     * the payload, which rejects truncated and non-font files before any package
     * is created.
     */
    static bool IsSupportedFontPayload(const TArray<uint8>& Bytes, FString& OutError)
    {
        // sfnt header: 4-byte version tag, uint16 numTables, then 6 reserved
        // bytes, followed by numTables * 16-byte table records.
        constexpr int32 SfntHeaderSize = 12;
        constexpr int32 SfntTableRecordSize = 16;
        if (Bytes.Num() < SfntHeaderSize)
        {
            OutError = TEXT("file is smaller than an sfnt header");
            return false;
        }

        auto ReadU32 = [&Bytes](int32 Offset)
        {
            return (static_cast<uint32>(Bytes[Offset]) << 24)
                | (static_cast<uint32>(Bytes[Offset + 1]) << 16)
                | (static_cast<uint32>(Bytes[Offset + 2]) << 8)
                | static_cast<uint32>(Bytes[Offset + 3]);
        };

        const uint32 Version = ReadU32(0);
        const bool bTrueType = Version == 0x00010000u;            // TrueType outlines
        const bool bTrueTypeTag = Version == 0x74727565u;         // 'true'
        const bool bOpenType = Version == 0x4F54544Fu;            // 'OTTO', CFF outlines
        if (Version == 0x74746366u)                               // 'ttcf'
        {
            OutError = TEXT("TrueType Collection (.ttc) payloads are not supported");
            return false;
        }
        if (!bTrueType && !bTrueTypeTag && !bOpenType)
        {
            OutError = TEXT("missing a TrueType or OpenType sfnt version tag");
            return false;
        }

        const int32 NumTables =
            (static_cast<int32>(Bytes[4]) << 8) | static_cast<int32>(Bytes[5]);
        if (NumTables <= 0)
        {
            OutError = TEXT("sfnt header declares no tables");
            return false;
        }
        const int64 RequiredSize =
            static_cast<int64>(SfntHeaderSize)
            + static_cast<int64>(NumTables) * SfntTableRecordSize;
        if (static_cast<int64>(Bytes.Num()) < RequiredSize)
        {
            OutError = FString::Printf(
                TEXT("file is truncated: %d tables need at least %lld bytes, file has %d"),
                NumTables,
                RequiredSize,
                Bytes.Num());
            return false;
        }
        return true;
    }

    /**
     * Removes every package this invocation created unless Commit() runs.
     *
     * A multi-face import creates and optionally saves each face before the
     * family exists, so a later face failure - or a family create/save failure -
     * previously left a partial font family behind on disk and in the Asset
     * Registry. Registering each created package here makes every failure path
     * clean up without threading rollback code through a dozen early returns.
     */
    class FFontIngestRollbackScope
    {
    public:
        void Track(UPackage* Package, UObject* Asset)
        {
            if (Package && Asset)
            {
                CreatedPackages.Add(Package);
                CreatedAssets.Add(Asset);
            }
        }

        void Commit() { bCommitted = true; }

        ~FFontIngestRollbackScope()
        {
            if (bCommitted)
            {
                return;
            }

            for (int32 Index = CreatedAssets.Num() - 1; Index >= 0; --Index)
            {
                UObject* Asset = CreatedAssets[Index];
                UPackage* Package = CreatedPackages[Index];
                if (!IsValid(Asset) || !IsValid(Package))
                {
                    continue;
                }

                FAssetRegistryModule::AssetDeleted(Asset);
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->MarkAsGarbage();

                // Remove any file this invocation already wrote, so a partially
                // saved family does not survive on disk.
                FString Filename;
                if (FPackageName::TryConvertLongPackageNameToFilename(
                        Package->GetName(),
                        Filename,
                        FPackageName::GetAssetPackageExtension())
                    && IFileManager::Get().FileExists(*Filename))
                {
                    IFileManager::Get().Delete(*Filename, false, true);
                }

                Package->SetDirtyFlag(false);
                Package->MarkAsGarbage();
            }
        }

    private:
        TArray<UPackage*> CreatedPackages;
        TArray<UObject*> CreatedAssets;
        bool bCommitted = false;
    };

    /** Map a loading-policy string to the enum without accepting unknown values. */
    static bool ParseLoadingPolicy(const FString& S, EFontLoadingPolicy& Out)
    {
        if (S == TEXT("LazyLoad")) { Out = EFontLoadingPolicy::LazyLoad; return true; }
        if (S == TEXT("Stream"))   { Out = EFontLoadingPolicy::Stream;   return true; }
        if (S == TEXT("Inline"))   { Out = EFontLoadingPolicy::Inline;   return true; }
        return false;
    }

    /** Map a hinting string to the enum without accepting unknown values. */
    static bool ParseHinting(const FString& S, EFontHinting& Out)
    {
        if (S == TEXT("Default"))     { Out = EFontHinting::Default;     return true; }
        if (S == TEXT("Auto"))        { Out = EFontHinting::Auto;        return true; }
        if (S == TEXT("AutoLight"))   { Out = EFontHinting::AutoLight;   return true; }
        if (S == TEXT("Monochrome"))  { Out = EFontHinting::Monochrome;  return true; }
        if (S == TEXT("None"))        { Out = EFontHinting::None;        return true; }
        return false;
    }

    /** Parsed face spec. */
    struct FFaceSpec
    {
        FString Typeface;   // "Regular", "Bold", ...
        FString SourcePath; // Absolute path to TTF on disk
        TArray<uint8> SourceBytes;
    };

    /** Per-face import outputs, including saved asset path for the result payload. */
    struct FFaceResult
    {
        UFontFace* Face = nullptr;
        FString    AssetPath;   // Long package name, e.g. /Game/UI/Fonts/Example/F_Regular
        FString    Typeface;    // Passed through for typeface-entry construction
    };

    static bool PackageOrAssetExists(IAssetRegistry& AssetRegistry, const FString& PackageName, const FString& AssetName)
    {
        if (FindPackage(nullptr, *PackageName))
        {
            return true;
        }

        FString ExistingPackageFilename;
        if (FPackageName::DoesPackageExist(PackageName, &ExistingPackageFilename))
        {
            return true;
        }

        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
    }
} // namespace MonolithAsset::FontIngestInternal

FMonolithActionResult MonolithAsset::FFontIngestActions::HandleImportFontFamily(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithAsset::FontIngestInternal;

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
            FString::Printf(TEXT("destination must be a /Game/... directory path (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT("/")))
    {
        Destination = Destination.LeftChop(1);
    }

    FString FamilyName;
    if (!Params->TryGetStringField(TEXT("family_name"), FamilyName) || FamilyName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: family_name"), -32602);
    }

    const TArray<TSharedPtr<FJsonValue>>* FacesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("faces"), FacesArr) || !FacesArr || FacesArr->Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("faces must be a non-empty array"), -32602);
    }
    FString FaceCountError;
    if (!ValidateFontFaceCount(FacesArr->Num(), FaceCountError))
    {
        return FMonolithActionResult::Error(FaceCountError, -32602);
    }

    // --- Optional params ---
    EFontLoadingPolicy LoadingPolicy = EFontLoadingPolicy::LazyLoad;
    {
        FString LoadingPolicyStr;
        if (Params->TryGetStringField(TEXT("loading_policy"), LoadingPolicyStr) && !LoadingPolicyStr.IsEmpty())
        {
            if (!ParseLoadingPolicy(LoadingPolicyStr, LoadingPolicy))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("Unknown loading_policy '%s'. Supported: LazyLoad|Stream|Inline"),
                        *LoadingPolicyStr),
                    -32602);
            }
        }
        else if (Params->HasField(TEXT("loading_policy")))
        {
            return FMonolithActionResult::Error(
                TEXT("loading_policy must be a non-empty string"),
                -32602);
        }
    }

    EFontHinting Hinting = EFontHinting::Default;
    {
        FString HintingStr;
        if (Params->TryGetStringField(TEXT("hinting"), HintingStr) && !HintingStr.IsEmpty())
        {
            if (!ParseHinting(HintingStr, Hinting))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("Unknown hinting '%s'. Supported: Default|Auto|AutoLight|Monochrome|None"),
                        *HintingStr),
                    -32602);
            }
        }
        else if (Params->HasField(TEXT("hinting")))
        {
            return FMonolithActionResult::Error(
                TEXT("hinting must be a non-empty string"),
                -32602);
        }
    }

    bool bSave = true;
    if (Params->HasField(TEXT("save"))
        && (!Params->HasTypedField<EJson::Boolean>(TEXT("save"))
            || !Params->TryGetBoolField(TEXT("save"), bSave)))
    {
        return FMonolithActionResult::Error(TEXT("save must be a boolean"), -32602);
    }

    bool bAllowUniqueNames = false;
    if (Params->HasField(TEXT("allow_unique_names"))
        && (!Params->HasTypedField<EJson::Boolean>(TEXT("allow_unique_names"))
            || !Params->TryGetBoolField(TEXT("allow_unique_names"), bAllowUniqueNames)))
    {
        return FMonolithActionResult::Error(TEXT("allow_unique_names must be a boolean"), -32602);
    }

    // --- Parse face specs (fail fast on malformed entries before touching disk / packages) ---
    TArray<FFaceSpec> FaceSpecs;
    FaceSpecs.Reserve(FacesArr->Num());
    for (int32 i = 0; i < FacesArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& Entry = (*FacesArr)[i];
        const TSharedPtr<FJsonObject>* FaceObjPtr = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(FaceObjPtr) || !FaceObjPtr || !FaceObjPtr->IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d] must be an object with {typeface, source_path}"), i),
                -32602);
        }
        const TSharedPtr<FJsonObject>& FaceObj = *FaceObjPtr;

        FFaceSpec Spec;
        if (!FaceObj->TryGetStringField(TEXT("typeface"), Spec.Typeface) || Spec.Typeface.IsEmpty())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d].typeface must be a non-empty string"), i),
                -32602);
        }
        if (!FaceObj->TryGetStringField(TEXT("source_path"), Spec.SourcePath) || Spec.SourcePath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d].source_path must be a non-empty string"), i),
                -32602);
        }
        FaceSpecs.Add(MoveTemp(Spec));
    }

    // Guard against duplicate typeface names up-front (two entries both "Regular"
    // would create colliding F_Regular assets and a composite with duplicate key).
    {
        TSet<FString> Seen;
        for (const FFaceSpec& Spec : FaceSpecs)
        {
            bool bAlreadyIn = false;
            Seen.Add(Spec.Typeface, &bAlreadyIn);
            if (bAlreadyIn)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("Duplicate typeface '%s' in faces[] -- each entry must be unique"), *Spec.Typeface),
                    -32602);
            }
        }
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FString DesiredFamilyPackageBase = Destination / FamilyName;
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(DesiredFamilyPackageBase);
        !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, -32602);
    }

    for (const FFaceSpec& Spec : FaceSpecs)
    {
        const FString DesiredFaceAssetName = FString::Printf(TEXT("F_%s"), *Spec.Typeface);
        FString DesiredFacePackageBase = Destination / DesiredFaceAssetName;
        if (const FString ValidationError = MonolithCore::ValidatePackagePath(DesiredFacePackageBase);
            !ValidationError.IsEmpty())
        {
            return FMonolithActionResult::Error(ValidationError, -32602);
        }
    }

    if (!bAllowUniqueNames)
    {
        if (const FString ValidationError = MonolithCore::ValidatePackagePath(DesiredFamilyPackageBase); !ValidationError.IsEmpty())
        {
            return FMonolithActionResult::Error(ValidationError, -32602);
        }
        if (PackageOrAssetExists(AssetRegistry, DesiredFamilyPackageBase, FamilyName))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Asset already exists at '%s'. Set allow_unique_names=true explicitly to create a suffixed family."),
                    *DesiredFamilyPackageBase),
                -32602);
        }

        for (const FFaceSpec& Spec : FaceSpecs)
        {
            const FString DesiredFaceAssetName = FString::Printf(TEXT("F_%s"), *Spec.Typeface);
            FString DesiredFacePackageBase = Destination / DesiredFaceAssetName;
            if (const FString ValidationError = MonolithCore::ValidatePackagePath(DesiredFacePackageBase); !ValidationError.IsEmpty())
            {
                return FMonolithActionResult::Error(ValidationError, -32602);
            }
            if (PackageOrAssetExists(AssetRegistry, DesiredFacePackageBase, DesiredFaceAssetName))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("Asset already exists at '%s'. Set allow_unique_names=true explicitly to create a suffixed face."),
                        *DesiredFacePackageBase),
                    -32602);
            }
        }
    }

    // Validate every source before creating any package. Invalid or unreadable
    // faces fail the entire request instead of producing a partial family.
    int64 FamilySourceBytes = 0;
    for (int32 i = 0; i < FaceSpecs.Num(); ++i)
    {
        FFaceSpec& Spec = FaceSpecs[i];
        if (FPaths::IsRelative(Spec.SourcePath))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s').source_path must be absolute: '%s'"),
                    i,
                    *Spec.Typeface,
                    *Spec.SourcePath),
                -32602);
        }
        if (!FPaths::GetExtension(Spec.SourcePath).Equals(TEXT("ttf"), ESearchCase::IgnoreCase))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s').source_path must reference a .ttf file: '%s'"),
                    i,
                    *Spec.Typeface,
                    *Spec.SourcePath),
                -32602);
        }
        FString FontLoadError;
        if (!LoadBoundedFontFile(
                Spec.SourcePath,
                FamilySourceBytes,
                Spec.SourceBytes,
                FontLoadError))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s').source_path is not an acceptable font source: %s ('%s')"),
                    i,
                    *Spec.Typeface,
                    *FontLoadError,
                    *Spec.SourcePath),
                -32602);
        }

        // A .ttf suffix and a nonzero length were the only checks, so arbitrary
        // data renamed to .ttf was wrapped in FFontFaceData and saved as a family
        // whose faces cannot render. Validate the sfnt container header before any
        // package is created.
        FString FontFormatError;
        if (!IsSupportedFontPayload(Spec.SourceBytes, FontFormatError))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s').source_path is not a valid font payload: %s ('%s')"),
                    i,
                    *Spec.Typeface,
                    *FontFormatError,
                    *Spec.SourcePath),
                -32602);
        }
    }

    // --- Per-face import ---
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    // Cleans up every package created below unless the whole import succeeds.
    FontIngestInternal::FFontIngestRollbackScope RollbackScope;

    TArray<FFaceResult> FaceResults;
    FaceResults.Reserve(FaceSpecs.Num());

    for (int32 i = 0; i < FaceSpecs.Num(); ++i)
    {
        FFaceSpec& Spec = FaceSpecs[i];
        TArray<uint8> TtfBytes = MoveTemp(Spec.SourceBytes);

        const FString DesiredFaceAssetName = FString::Printf(TEXT("F_%s"), *Spec.Typeface);
        const FString DesiredFacePackageBase = Destination / DesiredFaceAssetName;

        FString UniqueFacePackageName;
        FString UniqueFaceAssetName;
        if (bAllowUniqueNames)
        {
            AssetToolsModule.Get().CreateUniqueAssetName(
                DesiredFacePackageBase, /*Suffix=*/FString(),
                /*out*/ UniqueFacePackageName, /*out*/ UniqueFaceAssetName);
        }
        else
        {
            UniqueFacePackageName = DesiredFacePackageBase;
            UniqueFaceAssetName = DesiredFaceAssetName;
        }

        if (const FString ValidationError = MonolithCore::ValidatePackagePath(UniqueFacePackageName); !ValidationError.IsEmpty())
        {
            return FMonolithActionResult::Error(ValidationError, -32603);
        }

        UPackage* FacePackage = CreatePackage(*UniqueFacePackageName);
        if (!FacePackage)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s'): CreatePackage failed for '%s'"),
                    i,
                    *Spec.Typeface,
                    *UniqueFacePackageName),
                -32603);
        }
        FacePackage->FullyLoad();

        UFontFace* FaceAsset = NewObject<UFontFace>(
            FacePackage, FName(*UniqueFaceAssetName), RF_Public | RF_Standalone);
        if (!FaceAsset)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("faces[%d] ('%s'): NewObject<UFontFace> failed"),
                    i,
                    *Spec.Typeface),
                -32603);
        }

        // FontFaceData is a FFontFaceDataRef (TSharedRef) -- construct via the
        // static factory so the internal refcount lifecycle is correct.
        FaceAsset->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(TtfBytes));
        FaceAsset->SourceFilename = Spec.SourcePath;
        FaceAsset->Hinting = Hinting;
        FaceAsset->LoadingPolicy = LoadingPolicy;

#if WITH_EDITORONLY_DATA
        FaceAsset->CacheSubFaces();
#endif // WITH_EDITORONLY_DATA

        // UFontFace::PostEditChangeProperty flushes the Slate font cache through
        // FSlateApplication::Get(), which asserts in headless commandlets where no
        // Slate application exists. The flush only refreshes live editor previews,
        // so skip PostEditChange when Slate is not initialized.
        if (FSlateApplication::IsInitialized())
        {
            FaceAsset->PostEditChange();
        }
        FAssetRegistryModule::AssetCreated(FaceAsset);
        RollbackScope.Track(FacePackage, FaceAsset);
        FacePackage->MarkPackageDirty();

        if (bSave)
        {
            const FString FacePackageFilename = FPackageName::LongPackageNameToFilename(
                FacePackage->GetName(), FPackageName::GetAssetPackageExtension());

            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            const bool bSaved = UPackage::SavePackage(
                FacePackage, FaceAsset, *FacePackageFilename, SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("faces[%d] ('%s'): SavePackage failed for '%s'"),
                        i,
                        *Spec.Typeface,
                        *FacePackageFilename),
                    -32603);
            }
        }

        FFaceResult R;
        R.Face = FaceAsset;
        R.AssetPath = UniqueFacePackageName;
        R.Typeface = Spec.Typeface;
        FaceResults.Add(MoveTemp(R));
    }

    // --- Create UFont composite ---
    FString UniqueFamilyPackageName;
    FString UniqueFamilyAssetName;
    if (bAllowUniqueNames)
    {
        AssetToolsModule.Get().CreateUniqueAssetName(
            DesiredFamilyPackageBase, /*Suffix=*/FString(),
            /*out*/ UniqueFamilyPackageName, /*out*/ UniqueFamilyAssetName);
    }
    else
    {
        UniqueFamilyPackageName = DesiredFamilyPackageBase;
        UniqueFamilyAssetName = FamilyName;
    }

    if (const FString ValidationError = MonolithCore::ValidatePackagePath(UniqueFamilyPackageName); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, -32603);
    }

    UPackage* FamilyPackage = CreatePackage(*UniqueFamilyPackageName);
    if (!FamilyPackage)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create family package '%s'"), *UniqueFamilyPackageName),
            -32603);
    }
    FamilyPackage->FullyLoad();

    UFont* FamilyFont = NewObject<UFont>(
        FamilyPackage, FName(*UniqueFamilyAssetName), RF_Public | RF_Standalone);
    if (!FamilyFont)
    {
        return FMonolithActionResult::Error(TEXT("NewObject<UFont> failed for family asset"), -32603);
    }

    FamilyFont->FontCacheType = EFontCacheType::Runtime;
    FamilyFont->LegacyFontName = FName(*UniqueFamilyAssetName);

    // UE 5.7: direct public write to UFont::CompositeFont is UE_DEPRECATED -- the
    // header instructs callers to go through GetMutableInternalCompositeFont().
    FCompositeFont& Composite = FamilyFont->GetMutableInternalCompositeFont();
    Composite.DefaultTypeface.Fonts.Reset();

    for (const FFaceResult& R : FaceResults)
    {
        FTypefaceEntry Entry;
        Entry.Name = FName(*R.Typeface);
        Entry.Font = FFontData(R.Face);
        Composite.DefaultTypeface.Fonts.Add(MoveTemp(Entry));
    }

#if WITH_EDITORONLY_DATA
    Composite.MakeDirty();
#endif // WITH_EDITORONLY_DATA

    // Same headless guard as the per-face import: UFont::PostEditChange routes into
    // Slate font-cache flushes that assert without a Slate application.
    if (FSlateApplication::IsInitialized())
    {
        FamilyFont->PostEditChange();
    }
    FAssetRegistryModule::AssetCreated(FamilyFont);
    RollbackScope.Track(FamilyPackage, FamilyFont);
    FamilyPackage->MarkPackageDirty();

    if (bSave)
    {
        const FString FamilyPackageFilename = FPackageName::LongPackageNameToFilename(
            FamilyPackage->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(
            FamilyPackage, FamilyFont, *FamilyPackageFilename, SaveArgs);
        if (!bSaved)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UPackage::SavePackage failed for family '%s'"), *FamilyPackageFilename),
                -32603);
        }
    }

    // Every package created above is now final; keep them.
    RollbackScope.Commit();

    // --- Success payload ---
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("family_asset_path"), UniqueFamilyPackageName);

    TArray<TSharedPtr<FJsonValue>> FacePathsJson;
    FacePathsJson.Reserve(FaceResults.Num());
    for (const FFaceResult& R : FaceResults)
    {
        FacePathsJson.Add(MakeShared<FJsonValueString>(R.AssetPath));
    }
    Result->SetArrayField(TEXT("face_asset_paths"), FacePathsJson);
    Result->SetNumberField(TEXT("faces_imported"), (double)FaceResults.Num());
    Result->SetNumberField(TEXT("faces_requested"), (double)FaceSpecs.Num());

    return FMonolithActionResult::Success(Result);
}

void MonolithAsset::FFontIngestActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("asset"),
        TEXT("import_font_family"),
        TEXT("Import a font family (one-or-more TTF files) as a UFont composite asset plus one UFontFace per typeface entry. "
             "Params: destination (string, required, /Game/... output directory), "
             "family_name (string, required, UFont composite asset name), "
             "faces (array<object>, required, 1-64 entries, each { typeface: string (e.g. 'Regular','Bold'), source_path: string (absolute TTF path, up to 64 MiB) }), "
             "loading_policy (string, optional, default 'LazyLoad', one of LazyLoad|Stream|Inline), "
             "hinting (string, optional, default 'Default', one of Default|Auto|AutoLight|Monochrome|None), "
             "save (bool, optional, default true), "
             "allow_unique_names (bool, optional, default false; true explicitly opts into suffixed package names when requested paths exist). "
             "All source files and exact output paths are preflighted before package creation; aggregate source bytes are limited to 256 MiB. "
             "Returns { family_asset_path, face_asset_paths[], faces_imported, faces_requested }."),
        FMonolithActionHandler::CreateStatic(&MonolithAsset::FFontIngestActions::HandleImportFontFamily),
        FParamSchemaBuilder()
            .Required(TEXT("destination"), TEXT("string"), TEXT("Output directory, e.g. /Game/UI/Fonts"))
            .Required(TEXT("family_name"), TEXT("string"), TEXT("Composite UFont asset name"))
            .Required(TEXT("faces"), TEXT("array"), TEXT("1-64 typeface specs with typeface and an absolute source_path up to 64 MiB; aggregate source payload is limited to 256 MiB"))
            .Optional(TEXT("loading_policy"), TEXT("string"), TEXT("LazyLoad, Stream, or Inline"), TEXT("LazyLoad"))
            .Optional(TEXT("hinting"), TEXT("string"), TEXT("Default, Auto, AutoLight, Monochrome, or None"), TEXT("Default"))
            .Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported font assets"), TEXT("true"))
            .Optional(TEXT("allow_unique_names"), TEXT("bool"), TEXT("Explicitly allow suffixed package names when requested paths exist"), TEXT("false"))
            .StrictComplexTypes()
            .Build());
}
