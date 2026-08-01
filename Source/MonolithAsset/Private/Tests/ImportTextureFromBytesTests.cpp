// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "MonolithAssetTextureIngestInternal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Modules/ModuleManager.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Image fixture generation
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

// Texture verification
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    struct FExpectedTextureRoleSettings
    {
        const TCHAR* Role = TEXT("");
        TextureCompressionSettings Compression = TC_Default;
        bool bSRGB = true;
        TextureMipGenSettings MipGen = TMGS_FromTextureGroup;
        TextureGroup LODGroup = TEXTUREGROUP_World;
        TextureAddress AddressX = TA_Wrap;
        TextureAddress AddressY = TA_Wrap;
    };

    FString EncodePngB64(int32 Width, int32 Height, const TArray<FColor>& Pixels)
    {
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid() || Pixels.Num() != Width * Height)
        {
            return FString();
        }

        if (!Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
        {
            return FString();
        }

        const TArray64<uint8> PngBytes64 = Wrapper->GetCompressed(100);
        TArray<uint8> PngBytes;
        PngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
        return FBase64::Encode(PngBytes);
    }

    FString MakeSolidPngB64(const FColor& Color, int32 Width = 2, int32 Height = 2)
    {
        TArray<FColor> Pixels;
        Pixels.Init(Color, Width * Height);
        return EncodePngB64(Width, Height, Pixels);
    }

    void WritePngUint32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
    {
        Bytes[Offset + 0] = static_cast<uint8>((Value >> 24) & 0xff);
        Bytes[Offset + 1] = static_cast<uint8>((Value >> 16) & 0xff);
        Bytes[Offset + 2] = static_cast<uint8>((Value >> 8) & 0xff);
        Bytes[Offset + 3] = static_cast<uint8>(Value & 0xff);
    }

    FString MakePngWithHeaderDimensionsB64(uint32 Width, uint32 Height)
    {
        TArray<uint8> PngBytes;
        if (!FBase64::Decode(MakeSolidPngB64(FColor::Red), PngBytes)
            || PngBytes.Num() < 33
            || PngBytes[12] != 'I'
            || PngBytes[13] != 'H'
            || PngBytes[14] != 'D'
            || PngBytes[15] != 'R')
        {
            return FString();
        }

        // A PNG IHDR stores width/height as big-endian uint32 values. Recompute
        // its CRC so SetCompressed accepts the header, while leaving the tiny
        // IDAT payload untouched; the action must reject before GetRaw sees it.
        WritePngUint32(PngBytes, 16, Width);
        WritePngUint32(PngBytes, 20, Height);
        WritePngUint32(PngBytes, 29, FCrc::MemCrc32(PngBytes.GetData() + 12, 17));
        return FBase64::Encode(PngBytes);
    }

    FString MakeUniqueTestAssetPath(const TCHAR* Prefix)
    {
        return FString::Printf(
            TEXT("/Game/Tests/Monolith/Asset/Textures/%s_%s_Asset"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString MakeTransparentEdgePngB64()
    {
        TArray<FColor> Pixels;
        Pixels.Add(FColor(255, 0, 0, 255));
        Pixels.Add(FColor(0, 0, 0, 0));
        Pixels.Add(FColor(255, 0, 0, 255));
        Pixels.Add(FColor(0, 0, 0, 0));
        return EncodePngB64(2, 2, Pixels);
    }

    FString MakeEdgeBackgroundIconPngB64()
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(245, 245, 245, 255), 25);
        for (int32 Y = 1; Y <= 3; ++Y)
        {
            for (int32 X = 1; X <= 3; ++X)
            {
                Pixels[Y * 5 + X] = FColor(220, 30, 30, 255);
            }
        }
        Pixels[2 * 5 + 2] = FColor(245, 245, 245, 255);
        return EncodePngB64(5, 5, Pixels);
    }

    FString MakeMismatchedTilePngB64()
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(64, 96, 128, 255), 16);
        for (int32 Y = 0; Y < 4; ++Y)
        {
            Pixels[Y * 4 + 0] = FColor(0, 0, 0, 255);
            Pixels[Y * 4 + 3] = FColor(255, 255, 255, 255);
        }
        return EncodePngB64(4, 4, Pixels);
    }

    bool DecodePngB64(const FString& PngB64, int32& OutW, int32& OutH, TArray<uint8>& OutRawBgra)
    {
        TArray<uint8> PngBytes;
        if (!FBase64::Decode(PngB64, PngBytes) || PngBytes.Num() == 0)
        {
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid() || !Wrapper->SetCompressed(PngBytes.GetData(), PngBytes.Num()))
        {
            return false;
        }

        OutW = Wrapper->GetWidth();
        OutH = Wrapper->GetHeight();
        return Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutRawBgra) && OutRawBgra.Num() > 0;
    }

    uint8 AlphaAt(const TArray<uint8>& RawBgra, int32 W, int32 X, int32 Y)
    {
        return RawBgra[(Y * W + X) * 4 + 3];
    }

    UTexture2D* FindTextureAtPackagePath(const FString& AssetPath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
        return FindObject<UTexture2D>(nullptr, *(AssetPath + TEXT(".") + AssetName));
    }

    TArray<FString> GetTexturePackageFilenames(const FString& AssetPath)
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
                AssetPath,
                HeaderFilename,
                FPackageName::GetAssetPackageExtension()))
        {
            AddCandidate(MoveTemp(HeaderFilename));
        }
        FPackagePath PackagePath;
        if (FPackagePath::TryFromPackageName(AssetPath, PackagePath))
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

    void CleanupSavedTextureAtPackagePath(const FString& AssetPath)
    {
        FString Filename = FPackageName::LongPackageNameToFilename(
            AssetPath,
            FPackageName::GetAssetPackageExtension());
        Filename = FPaths::ConvertRelativePathToFull(Filename);
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, false);

        if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            UEditorAssetLibrary::DeleteAsset(AssetPath);
        }
        if (UPackage* Package = FindPackage(nullptr, *AssetPath))
        {
            ResetLoaders(Package);
            Package->SetDirtyFlag(false);
            Package->MarkAsGarbage();
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        for (const FString& PackageFilename : GetTexturePackageFilenames(AssetPath))
        {
            IFileManager::Get().Delete(
                *PackageFilename,
                /*RequireExists=*/false,
                /*EvenReadOnly=*/true,
                /*Quiet=*/true);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetTextureDecodeBoundsTest,
    "MonolithAsset.ImportTextureFromBytes.DecodeBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetTextureDecodeBoundsTest::RunTest(const FString& Parameters)
{
    using namespace MonolithAsset::TextureIngestInternal;

    TestEqual(
        TEXT("exact base64 size accounts for padding"),
        FBase64::GetDecodedDataSize(TEXT("TQ==")),
        uint32{1});
    TestEqual(
        TEXT("maximum base64 size remains an intentionally looser upper bound"),
        FBase64::GetMaxDecodedDataSize(4),
        uint32{3});

    int64 ExpectedBytes = 0;
    FString Error;
    TestTrue(
        TEXT("2x2 BGRA8 is accepted"),
        ValidateDecodedImageBounds(2, 2, ExpectedBytes, Error));
    TestEqual(TEXT("2x2 BGRA8 byte count"), ExpectedBytes, int64{16});
    TestTrue(TEXT("valid dimensions have no error"), Error.IsEmpty());

    TestTrue(
        TEXT("the largest square within the byte budget is accepted"),
        ValidateDecodedImageBounds(11585, 11585, ExpectedBytes, Error));
    TestTrue(TEXT("accepted square stays within byte budget"), ExpectedBytes <= MaxDecodedImageBytes);

    TestFalse(
        TEXT("16K square is rejected before decode because BGRA8 exceeds the byte budget"),
        ValidateDecodedImageBounds(16384, 16384, ExpectedBytes, Error));
    TestTrue(TEXT("byte-budget rejection is explicit"), Error.Contains(TEXT("byte limit")));

    TestFalse(
        TEXT("per-axis limit is enforced before decode"),
        ValidateDecodedImageBounds(MaxDecodedImageDimension + 1, 1, ExpectedBytes, Error));
    TestTrue(TEXT("dimension rejection is explicit"), Error.Contains(TEXT("per-axis limit")));

    TestFalse(
        TEXT("zero width is rejected"),
        ValidateDecodedImageBounds(0, 1, ExpectedBytes, Error));
    TestTrue(TEXT("invalid dimensions are explicit"), Error.Contains(TEXT("invalid dimensions")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetTextureDecodeBoundsActionTest,
    "MonolithAsset.ImportTextureFromBytes.DecodeBoundsAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetTextureDecodeBoundsActionTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueTestAssetPath(TEXT("DecodeBounds"));
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"), AssetPath);
    Params->SetStringField(TEXT("format_hint"), TEXT("png"));
    Params->SetStringField(
        TEXT("bytes_b64"),
        MakePngWithHeaderDimensionsB64(16384, 16384));

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), Params);

    TestFalse(TEXT("oversized decoded surface is rejected"), Result.bSuccess);
    TestEqual(TEXT("decoded bounds use invalid-params code"), Result.ErrorCode, -32602);
    TestTrue(
        *FString::Printf(
            TEXT("decoded byte-budget error is explicit (actual: %s)"),
            *Result.ErrorMessage),
        Result.ErrorMessage.Contains(TEXT("decoded BGRA8 byte limit")));
    TestNull(TEXT("oversized header does not create a texture"), FindTextureAtPackagePath(AssetPath));
    TestFalse(TEXT("oversized header does not save a package"), UEditorAssetLibrary::DoesAssetExist(AssetPath));
    CleanupSavedTextureAtPackagePath(AssetPath);
    return true;
}

/**
 * MonolithAsset.ImportTextureFromBytes.BasicPNG
 *
 * Dispatches `asset.import_texture_from_bytes` through the Monolith registry with
 * a 2x2 red PNG (base64). Asserts:
 *  - Action succeeds
 *  - Result payload exposes asset_path / width / height
 *  - Width/Height match the PNG header (2x2)
 *  - The UTexture2D actually materialises in-memory at the reported path
 *
 * Uses save=false so the test does not pollute /Content/ on disk.
 *
 * Test fixture path: /Game/Tests/Monolith/Asset/Textures/T_ImportBytesTest
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureFromBytesBasicTest,
    "MonolithAsset.ImportTextureFromBytes.BasicPNG",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureFromBytesBasicTest::RunTest(const FString& Parameters)
{
    // 2x2 red PNG, base64-encoded.
    const FString RedPngB64 = TEXT(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GB"
        "gYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC");

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"),
        TEXT("/Game/Tests/Monolith/Asset/Textures/T_ImportBytesTest"));
    Params->SetStringField(TEXT("bytes_b64"), RedPngB64);
    Params->SetStringField(TEXT("format_hint"), TEXT("png"));
    Params->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), Params);

    TestTrue(TEXT("import_texture_from_bytes bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"),
            *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    if (!Result.Result.IsValid())
    {
        AddError(TEXT("Result payload was null on success"));
        return false;
    }

    FString AssetPath;
    TestTrue(TEXT("asset_path in result"),
        Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath));

    double W = 0.0, H = 0.0, SizeBytes = 0.0;
    Result.Result->TryGetNumberField(TEXT("width"), W);
    Result.Result->TryGetNumberField(TEXT("height"), H);
    Result.Result->TryGetNumberField(TEXT("size_bytes"), SizeBytes);
    TestEqual(TEXT("width 2"), (int32)W, 2);
    TestEqual(TEXT("height 2"), (int32)H, 2);
    TestEqual(TEXT("size_bytes 2*2*4=16"), (int32)SizeBytes, 16);

    if (!AssetPath.IsEmpty())
    {
        FString AssetName;
        {
            int32 SlashIdx = INDEX_NONE;
            if (AssetPath.FindLastChar(TEXT('/'), SlashIdx))
            {
                AssetName = AssetPath.Mid(SlashIdx + 1);
            }
        }
        const FString DottedPath = AssetPath + TEXT(".") + AssetName;

        UTexture2D* Tex = FindObject<UTexture2D>(nullptr, *DottedPath);
        if (!Tex)
        {
            Tex = LoadObject<UTexture2D>(nullptr, *DottedPath);
        }
        TestNotNull(TEXT("UTexture2D exists at returned asset_path"), Tex);
        if (Tex)
        {
            // GetSizeX/Y return LODGroup-mutated platform-data size; assert on
            // Source.GetSizeX/Y instead so TEXTUREGROUP_UI's MinLODSize=32
            // doesn't pad the 2x2 source up to 32x32 at build time.
#if WITH_EDITOR
            TestEqual(TEXT("Tex->Source SizeX == 2"), Tex->Source.GetSizeX(), (int64)2);
            TestEqual(TEXT("Tex->Source SizeY == 2"), Tex->Source.GetSizeY(), (int64)2);
#endif
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureFromBytesInvalidSettingsTest,
    "MonolithAsset.ImportTextureFromBytes.InvalidSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureFromBytesInvalidSettingsTest::RunTest(const FString& Parameters)
{
    const auto MakeBaseParams = []()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(
            TEXT("destination"),
            TEXT("/Game/Tests/Monolith/Asset/Textures/T_InvalidSettings"));
        P->SetStringField(TEXT("bytes_b64"), TEXT("AA=="));
        P->SetStringField(TEXT("format_hint"), TEXT("png"));
        P->SetBoolField(TEXT("save"), false);
        return P;
    };

    const TArray<TPair<FString, FString>> InvalidStringSettings = {
        { TEXT("compression_settings"), TEXT("TC_NotReal") },
        { TEXT("mip_gen_settings"), TEXT("TMGS_NotReal") },
        { TEXT("lod_group"), TEXT("TEXTUREGROUP_NotReal") },
        { TEXT("address_x"), TEXT("TA_NotReal") },
        { TEXT("address_y"), TEXT("TA_NotReal") }
    };
    for (const TPair<FString, FString>& InvalidSetting : InvalidStringSettings)
    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(InvalidSetting.Key, InvalidSetting.Value);
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetObjectField(TEXT("settings"), Settings);

        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"),
            TEXT("import_texture_from_bytes"),
            P);
        TestFalse(
            *FString::Printf(TEXT("invalid %s is rejected"), *InvalidSetting.Key),
            R.bSuccess);
        TestEqual(
            *FString::Printf(TEXT("invalid %s uses invalid-params code"), *InvalidSetting.Key),
            R.ErrorCode,
            -32602);
    }

    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(TEXT("srgb"), TEXT("true"));
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetObjectField(TEXT("settings"), Settings);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(TEXT("non-boolean settings.srgb is rejected"), R.bSuccess);
        TestEqual(TEXT("non-boolean settings.srgb uses invalid-params code"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetStringField(TEXT("save"), TEXT("false"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(TEXT("string-valued save is rejected"), R.bSuccess);
        TestEqual(TEXT("string-valued save uses invalid-params code"), R.ErrorCode, -32602);
    }

    for (const FString& BooleanSetting : {
        FString(TEXT("alpha_bleed")),
        FString(TEXT("alpha_from_edge_background")),
        FString(TEXT("tile_seam_harmonize"))
    })
    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(BooleanSetting, TEXT("false"));
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetObjectField(TEXT("settings"), Settings);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(
            *FString::Printf(TEXT("string-valued settings.%s is rejected"), *BooleanSetting),
            R.bSuccess);
        TestEqual(
            *FString::Printf(TEXT("string-valued settings.%s uses invalid-params code"), *BooleanSetting),
            R.ErrorCode,
            -32602);
    }

    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetBoolField(TEXT("tile_seam_harmonize"), true);
        Settings->SetBoolField(TEXT("seam_harmonize"), true);
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetObjectField(TEXT("settings"), Settings);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(TEXT("duplicate seam aliases are rejected"), R.bSuccess);
        TestEqual(TEXT("duplicate seam aliases use invalid-params code"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetStringField(TEXT("settings"), TEXT("{}"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(TEXT("string-encoded settings are rejected"), R.bSuccess);
        TestEqual(TEXT("string-encoded settings use invalid-params code"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeBaseParams();
        P->SetStringField(TEXT("texture_role"), TEXT("normal"));
        P->SetStringField(TEXT("role"), TEXT("normal"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), P);
        TestFalse(TEXT("duplicate texture-role aliases are rejected"), R.bSuccess);
        TestEqual(TEXT("duplicate texture-role aliases use invalid-params code"), R.ErrorCode, -32602);
    }

    return true;
}

/**
 * MonolithAsset.ImportTextureFromBytes.TextureRoleNormal
 *
 * Verifies that the role-specific import pipeline applies Unreal data-texture
 * settings before save/import finalization and returns validation metadata.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureFromBytesTextureRoleNormalTest,
    "MonolithAsset.ImportTextureFromBytes.TextureRoleNormal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureFromBytesTextureRoleNormalTest::RunTest(const FString& Parameters)
{
    const FString RedPngB64 = TEXT(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GB"
        "gYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC");

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"),
        TEXT("/Game/Tests/Monolith/Asset/Textures/T_ImportBytesNormalRoleTest"));
    Params->SetStringField(TEXT("bytes_b64"), RedPngB64);
    Params->SetStringField(TEXT("format_hint"), TEXT("png"));
    Params->SetStringField(TEXT("texture_role"), TEXT("normal"));
    Params->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), Params);

    TestTrue(TEXT("normal role import bSuccess"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"),
            *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    FString TextureRole;
    TestTrue(TEXT("texture_role returned"),
        Result.Result->TryGetStringField(TEXT("texture_role"), TextureRole));
    TestEqual(TEXT("texture_role normal"), TextureRole, FString(TEXT("normal")));

    const TSharedPtr<FJsonObject>* Validation = nullptr;
    TestTrue(TEXT("validation returned"),
        Result.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());

    FString AssetPath;
    Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    UTexture2D* Tex = FindObject<UTexture2D>(nullptr, *(AssetPath + TEXT(".") + AssetName));
    TestNotNull(TEXT("normal role UTexture2D exists"), Tex);
    if (Tex)
    {
        TestFalse(TEXT("normal role disables sRGB"), Tex->SRGB != 0);
        TestEqual(TEXT("normal role compression"), Tex->CompressionSettings, TC_Normalmap);
        TestEqual(TEXT("normal role LOD group"), Tex->LODGroup, TEXTUREGROUP_WorldNormalMap);
        TestEqual(TEXT("normal role AddressX"), Tex->AddressX, TA_Wrap);
        TestEqual(TEXT("normal role AddressY"), Tex->AddressY, TA_Wrap);
    }

    return true;
}

/**
 * MonolithAsset.ImportTextureFromBytes.TextureRolePresetMatrix
 *
 * Exercises every Unreal texture_role preset and verifies that the imported
 * UTexture2D receives role-specific compression, sRGB, mip, LOD, and address
 * settings. Also validates alpha-bleed post-processing on transparent UI PNGs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureFromBytesTextureRolePresetMatrixTest,
    "MonolithAsset.ImportTextureFromBytes.TextureRolePresetMatrix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureFromBytesTextureRolePresetMatrixTest::RunTest(const FString& Parameters)
{
    const FString SolidPngB64 = MakeSolidPngB64(FColor(128, 128, 255, 255));
    TestFalse(TEXT("solid PNG fixture encoded"), SolidPngB64.IsEmpty());
    if (SolidPngB64.IsEmpty())
    {
        return false;
    }

    const FExpectedTextureRoleSettings Expected[] = {
        { TEXT("ui_icon"), TC_Default, true, TMGS_NoMipmaps, TEXTUREGROUP_UI, TA_Clamp, TA_Clamp },
        { TEXT("sprite"), TC_Default, true, TMGS_NoMipmaps, TEXTUREGROUP_UI, TA_Clamp, TA_Clamp },
        { TEXT("decal"), TC_Default, true, TMGS_FromTextureGroup, TEXTUREGROUP_Effects, TA_Clamp, TA_Clamp },
        { TEXT("basecolor"), TC_Default, true, TMGS_FromTextureGroup, TEXTUREGROUP_World, TA_Wrap, TA_Wrap },
        { TEXT("world_tile"), TC_Default, true, TMGS_FromTextureGroup, TEXTUREGROUP_World, TA_Wrap, TA_Wrap },
        { TEXT("normal"), TC_Normalmap, false, TMGS_FromTextureGroup, TEXTUREGROUP_WorldNormalMap, TA_Wrap, TA_Wrap },
        { TEXT("orm_mask"), TC_Masks, false, TMGS_FromTextureGroup, TEXTUREGROUP_WorldSpecular, TA_Wrap, TA_Wrap },
        { TEXT("height"), TC_Grayscale, false, TMGS_FromTextureGroup, TEXTUREGROUP_World, TA_Wrap, TA_Wrap },
        { TEXT("emissive"), TC_Default, true, TMGS_FromTextureGroup, TEXTUREGROUP_Effects, TA_Clamp, TA_Clamp },
    };

    for (const FExpectedTextureRoleSettings& Role : Expected)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("destination"),
            FString::Printf(TEXT("/Game/Tests/Monolith/Asset/Textures/T_Role_%s"), Role.Role));
        Params->SetStringField(TEXT("bytes_b64"), SolidPngB64);
        Params->SetStringField(TEXT("format_hint"), TEXT("png"));
        Params->SetStringField(TEXT("texture_role"), Role.Role);
        Params->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
        Params->SetBoolField(TEXT("save"), false);

        const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), Params);

        TestTrue(FString::Printf(TEXT("%s import succeeds"), Role.Role), Result.bSuccess);
        if (!Result.bSuccess || !Result.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("%s action error: %s (code %d)"),
                Role.Role, *Result.ErrorMessage, Result.ErrorCode));
            continue;
        }

        FString ImportedRole;
        TestTrue(FString::Printf(TEXT("%s returned texture_role"), Role.Role),
            Result.Result->TryGetStringField(TEXT("texture_role"), ImportedRole));
        TestEqual(FString::Printf(TEXT("%s normalized role"), Role.Role), ImportedRole, FString(Role.Role));

        const TSharedPtr<FJsonObject>* Validation = nullptr;
        TestTrue(FString::Printf(TEXT("%s validation returned"), Role.Role),
            Result.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());
        if (Validation && Validation->IsValid() && FString(Role.Role) == TEXT("world_tile"))
        {
            const TSharedPtr<FJsonObject>* Tile = nullptr;
            TestTrue(TEXT("world_tile validation contains tile metrics"),
                (*Validation)->TryGetObjectField(TEXT("tile"), Tile) && Tile && Tile->IsValid());
        }

        FString AssetPath;
        Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
        UTexture2D* Tex = FindTextureAtPackagePath(AssetPath);
        TestNotNull(FString::Printf(TEXT("%s UTexture2D exists"), Role.Role), Tex);
        if (Tex)
        {
            TestEqual(FString::Printf(TEXT("%s compression"), Role.Role), Tex->CompressionSettings, Role.Compression);
            TestEqual(FString::Printf(TEXT("%s sRGB"), Role.Role), Tex->SRGB != 0, Role.bSRGB);
            TestEqual(FString::Printf(TEXT("%s mip gen"), Role.Role), Tex->MipGenSettings, Role.MipGen);
            TestEqual(FString::Printf(TEXT("%s LOD group"), Role.Role), Tex->LODGroup, Role.LODGroup);
            TestEqual(FString::Printf(TEXT("%s AddressX"), Role.Role), Tex->AddressX, Role.AddressX);
            TestEqual(FString::Printf(TEXT("%s AddressY"), Role.Role), Tex->AddressY, Role.AddressY);
        }
    }

    const FString TransparentPngB64 = MakeTransparentEdgePngB64();
    TestFalse(TEXT("transparent PNG fixture encoded"), TransparentPngB64.IsEmpty());
    if (TransparentPngB64.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> AlphaParams = MakeShared<FJsonObject>();
    AlphaParams->SetStringField(TEXT("destination"),
        TEXT("/Game/Tests/Monolith/Asset/Textures/T_Role_AlphaBleed"));
    AlphaParams->SetStringField(TEXT("bytes_b64"), TransparentPngB64);
    AlphaParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    AlphaParams->SetStringField(TEXT("texture_role"), TEXT("ui_icon"));
    AlphaParams->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
    AlphaParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult AlphaResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), AlphaParams);

    TestTrue(TEXT("alpha bleed import succeeds"), AlphaResult.bSuccess);
    if (AlphaResult.bSuccess && AlphaResult.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Validation = nullptr;
        TestTrue(TEXT("alpha bleed validation returned"),
            AlphaResult.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());
        if (Validation && Validation->IsValid())
        {
            const TSharedPtr<FJsonObject>* PostProcess = nullptr;
            TestTrue(TEXT("alpha bleed postprocess returned"),
                (*Validation)->TryGetObjectField(TEXT("postprocess"), PostProcess) && PostProcess && PostProcess->IsValid());
            if (PostProcess && PostProcess->IsValid())
            {
                double AlphaBleedPixels = 0.0;
                (*PostProcess)->TryGetNumberField(TEXT("alpha_bleed_pixels"), AlphaBleedPixels);
                TestTrue(TEXT("alpha bleed filled transparent RGB pixels"), AlphaBleedPixels > 0.0);
            }
        }
    }

    const FString EdgeBackgroundIconPngB64 = MakeEdgeBackgroundIconPngB64();
    TestFalse(TEXT("edge-background icon PNG fixture encoded"), EdgeBackgroundIconPngB64.IsEmpty());
    if (!EdgeBackgroundIconPngB64.IsEmpty())
    {
        TSharedPtr<FJsonObject> EdgeAlphaParams = MakeShared<FJsonObject>();
        EdgeAlphaParams->SetStringField(TEXT("destination"),
            TEXT("/Game/Tests/Monolith/Asset/Textures/T_Role_EdgeBackgroundAlpha"));
        EdgeAlphaParams->SetStringField(TEXT("bytes_b64"), EdgeBackgroundIconPngB64);
        EdgeAlphaParams->SetStringField(TEXT("format_hint"), TEXT("png"));
        EdgeAlphaParams->SetStringField(TEXT("texture_role"), TEXT("ui_icon"));
        EdgeAlphaParams->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
        EdgeAlphaParams->SetBoolField(TEXT("save"), false);
        EdgeAlphaParams->SetBoolField(TEXT("return_processed_png"), true);

        const FMonolithActionResult EdgeAlphaResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), EdgeAlphaParams);

        TestTrue(TEXT("edge-background alpha import succeeds"), EdgeAlphaResult.bSuccess);
        if (EdgeAlphaResult.bSuccess && EdgeAlphaResult.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* Validation = nullptr;
            TestTrue(TEXT("edge-background alpha validation returned"),
                EdgeAlphaResult.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());
            if (Validation && Validation->IsValid())
            {
                bool bHasAlpha = false;
                (*Validation)->TryGetBoolField(TEXT("has_alpha"), bHasAlpha);
                TestTrue(TEXT("edge-background alpha produces alpha"), bHasAlpha);

                const TSharedPtr<FJsonObject>* PostProcess = nullptr;
                TestTrue(TEXT("edge-background alpha postprocess returned"),
                    (*Validation)->TryGetObjectField(TEXT("postprocess"), PostProcess) && PostProcess && PostProcess->IsValid());
                if (PostProcess && PostProcess->IsValid())
                {
                    double EdgeAlphaPixels = 0.0;
                    (*PostProcess)->TryGetNumberField(TEXT("alpha_from_edge_background_pixels"), EdgeAlphaPixels);
                    TestTrue(TEXT("edge-background alpha changed pixels"), EdgeAlphaPixels > 0.0);
                }
            }

            FString ProcessedPngB64;
            TestTrue(TEXT("processed PNG returned"),
                EdgeAlphaResult.Result->TryGetStringField(TEXT("processed_png_b64"), ProcessedPngB64) && !ProcessedPngB64.IsEmpty());
            int32 W = 0;
            int32 H = 0;
            TArray<uint8> RawBgra;
            TestTrue(TEXT("processed PNG decodes"), DecodePngB64(ProcessedPngB64, W, H, RawBgra));
            if (RawBgra.Num() > 0)
            {
                TestEqual(TEXT("edge background corner is transparent"), AlphaAt(RawBgra, W, 0, 0), static_cast<uint8>(0));
                TestEqual(TEXT("edge background foreground is opaque"), AlphaAt(RawBgra, W, 1, 1), static_cast<uint8>(255));
                TestEqual(TEXT("edge background internal color hole stays opaque"), AlphaAt(RawBgra, W, 2, 2), static_cast<uint8>(255));
            }
        }
    }

    const FString MismatchedTilePngB64 = MakeMismatchedTilePngB64();
    TestFalse(TEXT("mismatched tile PNG fixture encoded"), MismatchedTilePngB64.IsEmpty());
    if (!MismatchedTilePngB64.IsEmpty())
    {
        TSharedPtr<FJsonObject> TileParams = MakeShared<FJsonObject>();
        TileParams->SetStringField(TEXT("destination"),
            TEXT("/Game/Tests/Monolith/Asset/Textures/T_Role_TileSeamHarmonize"));
        TileParams->SetStringField(TEXT("bytes_b64"), MismatchedTilePngB64);
        TileParams->SetStringField(TEXT("format_hint"), TEXT("png"));
        TileParams->SetStringField(TEXT("texture_role"), TEXT("world_tile"));
        TileParams->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
        TileParams->SetBoolField(TEXT("save"), false);

        const FMonolithActionResult TileResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_texture_from_bytes"), TileParams);

        TestTrue(TEXT("tile seam harmonize import succeeds"), TileResult.bSuccess);
        if (TileResult.bSuccess && TileResult.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* Validation = nullptr;
            TestTrue(TEXT("tile seam validation returned"),
                TileResult.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());
            if (Validation && Validation->IsValid())
            {
                bool bPassed = false;
                (*Validation)->TryGetBoolField(TEXT("passed"), bPassed);
                TestTrue(TEXT("tile seam validation passes after harmonize"), bPassed);

                const TSharedPtr<FJsonObject>* Tile = nullptr;
                TestTrue(TEXT("tile seam metrics returned"),
                    (*Validation)->TryGetObjectField(TEXT("tile"), Tile) && Tile && Tile->IsValid());
                if (Tile && Tile->IsValid())
                {
                    double EdgeAverageDelta = -1.0;
                    double EdgeMaxDelta = -1.0;
                    (*Tile)->TryGetNumberField(TEXT("edge_average_delta"), EdgeAverageDelta);
                    (*Tile)->TryGetNumberField(TEXT("edge_max_delta"), EdgeMaxDelta);
                    TestEqual(TEXT("tile edge average delta harmonized"), EdgeAverageDelta, 0.0);
                    TestEqual(TEXT("tile edge max delta harmonized"), EdgeMaxDelta, 0.0);
                }
            }
        }
    }

    return true;
}

/**
 * The default fail policy must preserve the requested path contract: the first
 * import creates that exact asset and a second import fails without creating a
 * silently suffixed texture.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureConflictPolicyFailTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.FailPreservesExactPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureConflictPolicyFailTest::RunTest(const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictFail"));
    const FString PngB64 = MakeSolidPngB64(FColor::Red);
    TestFalse(TEXT("fail policy PNG fixture encoded"), PngB64.IsEmpty());
    if (PngB64.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
    ImportParams->SetStringField(TEXT("destination"), RequestedPath);
    ImportParams->SetStringField(TEXT("bytes_b64"), PngB64);
    ImportParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    ImportParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult FirstResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ImportParams);
    TestTrue(TEXT("default fail policy creates an unused exact path"), FirstResult.bSuccess);
    if (!FirstResult.bSuccess || !FirstResult.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("First import error: %s (code %d)"),
            *FirstResult.ErrorMessage, FirstResult.ErrorCode));
        return false;
    }

    FString ReturnedRequestedPath;
    FString ReturnedAssetPath;
    FString ReturnedPolicy;
    bool bCreated = false;
    bool bReplaced = true;
    TestTrue(TEXT("requested_asset_path returned"),
        FirstResult.Result->TryGetStringField(TEXT("requested_asset_path"), ReturnedRequestedPath));
    TestTrue(TEXT("asset_path returned"),
        FirstResult.Result->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath));
    TestTrue(TEXT("conflict_policy returned"),
        FirstResult.Result->TryGetStringField(TEXT("conflict_policy"), ReturnedPolicy));
    TestTrue(TEXT("created returned"), FirstResult.Result->TryGetBoolField(TEXT("created"), bCreated));
    TestTrue(TEXT("replaced returned"), FirstResult.Result->TryGetBoolField(TEXT("replaced"), bReplaced));
    TestEqual(TEXT("default fail requested path is exact"), ReturnedRequestedPath, RequestedPath);
    TestEqual(TEXT("default fail output path is exact"), ReturnedAssetPath, RequestedPath);
    TestEqual(TEXT("default policy reports fail"), ReturnedPolicy, FString(TEXT("fail")));
    TestTrue(TEXT("first exact import reports created"), bCreated);
    TestFalse(TEXT("first exact import does not report replaced"), bReplaced);

    UTexture2D* OriginalTexture = FindTextureAtPackagePath(RequestedPath);
    TestNotNull(TEXT("exact-path texture exists"), OriginalTexture);

    FString WouldBeUniquePackage;
    FString WouldBeUniqueAsset;
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"))
        .Get()
        .CreateUniqueAssetName(RequestedPath, FString(), WouldBeUniquePackage, WouldBeUniqueAsset);
    TestNotEqual(TEXT("unique naming would select a suffix after the first import"),
        WouldBeUniquePackage, RequestedPath);

    const FMonolithActionResult SecondResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ImportParams);
    TestFalse(TEXT("default fail policy rejects the existing path"), SecondResult.bSuccess);
    TestEqual(TEXT("existing-path failure is invalid params"), SecondResult.ErrorCode, -32602);
    TestTrue(TEXT("existing exact texture identity is unchanged"),
        FindTextureAtPackagePath(RequestedPath) == OriginalTexture);
    TestNull(TEXT("fail policy did not create the available suffixed texture"),
        FindTextureAtPackagePath(WouldBeUniquePackage));

    return true;
}

/** Only the explicit unique policy may select and create a suffixed asset. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureConflictPolicyUniqueTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.UniqueCreatesSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureConflictPolicyUniqueTest::RunTest(const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictUnique"));
    const FString PngB64 = MakeSolidPngB64(FColor::Green);
    TestFalse(TEXT("unique policy PNG fixture encoded"), PngB64.IsEmpty());
    if (PngB64.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> FirstParams = MakeShared<FJsonObject>();
    FirstParams->SetStringField(TEXT("destination"), RequestedPath);
    FirstParams->SetStringField(TEXT("bytes_b64"), PngB64);
    FirstParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    FirstParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult FirstResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), FirstParams);
    TestTrue(TEXT("unique policy fixture creates exact base asset"), FirstResult.bSuccess);
    if (!FirstResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("Base import error: %s (code %d)"),
            *FirstResult.ErrorMessage, FirstResult.ErrorCode));
        return false;
    }

    UTexture2D* BaseTexture = FindTextureAtPackagePath(RequestedPath);
    TestNotNull(TEXT("base texture exists"), BaseTexture);

    TSharedPtr<FJsonObject> UniqueParams = MakeShared<FJsonObject>();
    UniqueParams->SetStringField(TEXT("destination"), RequestedPath);
    UniqueParams->SetStringField(TEXT("bytes_b64"), PngB64);
    UniqueParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    UniqueParams->SetStringField(TEXT("conflict_policy"), TEXT("unique"));
    UniqueParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult UniqueResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), UniqueParams);
    TestTrue(TEXT("explicit unique policy succeeds"), UniqueResult.bSuccess);
    if (!UniqueResult.bSuccess || !UniqueResult.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Unique import error: %s (code %d)"),
            *UniqueResult.ErrorMessage, UniqueResult.ErrorCode));
        return false;
    }

    FString ReturnedRequestedPath;
    FString ReturnedAssetPath;
    FString ReturnedPolicy;
    bool bCreated = false;
    bool bReplaced = true;
    UniqueResult.Result->TryGetStringField(TEXT("requested_asset_path"), ReturnedRequestedPath);
    UniqueResult.Result->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath);
    UniqueResult.Result->TryGetStringField(TEXT("conflict_policy"), ReturnedPolicy);
    UniqueResult.Result->TryGetBoolField(TEXT("created"), bCreated);
    UniqueResult.Result->TryGetBoolField(TEXT("replaced"), bReplaced);
    TestEqual(TEXT("unique preserves requested_asset_path"), ReturnedRequestedPath, RequestedPath);
    TestNotEqual(TEXT("unique selects a different output path"), ReturnedAssetPath, RequestedPath);
    TestTrue(TEXT("unique output is derived from the requested path"), ReturnedAssetPath.StartsWith(RequestedPath));
    TestEqual(TEXT("unique reports policy"), ReturnedPolicy, FString(TEXT("unique")));
    TestTrue(TEXT("unique reports created"), bCreated);
    TestFalse(TEXT("unique does not report replaced"), bReplaced);

    UTexture2D* UniqueTexture = FindTextureAtPackagePath(ReturnedAssetPath);
    TestNotNull(TEXT("unique texture exists at returned path"), UniqueTexture);
    TestTrue(TEXT("unique texture has distinct object identity"), UniqueTexture != BaseTexture);
    TestTrue(TEXT("base texture identity remains intact"), FindTextureAtPackagePath(RequestedPath) == BaseTexture);

    return true;
}

/** Replace updates data and settings while preserving the existing asset identity. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureConflictPolicyReplaceTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.ReplacePreservesIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureConflictPolicyReplaceTest::RunTest(const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictReplace"));
    const FString InitialPngB64 = MakeSolidPngB64(FColor::Red, 2, 2);
    const FString ReplacementPngB64 = MakeSolidPngB64(FColor::Blue, 4, 4);
    TestFalse(TEXT("replace initial PNG fixture encoded"), InitialPngB64.IsEmpty());
    TestFalse(TEXT("replace updated PNG fixture encoded"), ReplacementPngB64.IsEmpty());
    if (InitialPngB64.IsEmpty() || ReplacementPngB64.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> InitialSettings = MakeShared<FJsonObject>();
    InitialSettings->SetStringField(TEXT("compression_settings"), TEXT("TC_Default"));
    InitialSettings->SetBoolField(TEXT("srgb"), true);
    InitialSettings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
    InitialSettings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_World"));
    InitialSettings->SetStringField(TEXT("address_x"), TEXT("TA_Clamp"));
    InitialSettings->SetStringField(TEXT("address_y"), TEXT("TA_Clamp"));

    TSharedPtr<FJsonObject> InitialParams = MakeShared<FJsonObject>();
    InitialParams->SetStringField(TEXT("destination"), RequestedPath);
    InitialParams->SetStringField(TEXT("bytes_b64"), InitialPngB64);
    InitialParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    InitialParams->SetObjectField(TEXT("settings"), InitialSettings);
    InitialParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult InitialResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), InitialParams);
    TestTrue(TEXT("replace fixture creates initial exact asset"), InitialResult.bSuccess);
    if (!InitialResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("Initial import error: %s (code %d)"),
            *InitialResult.ErrorMessage, InitialResult.ErrorCode));
        return false;
    }

    UTexture2D* OriginalTexture = FindTextureAtPackagePath(RequestedPath);
    TestNotNull(TEXT("initial texture exists"), OriginalTexture);
    if (!OriginalTexture)
    {
        return false;
    }
    UPackage* OriginalPackage = OriginalTexture->GetOutermost();
    OriginalPackage->SetDirtyFlag(false);
    TestFalse(TEXT("replace fixture package starts clean"), OriginalPackage->IsDirty());

    TSharedPtr<FJsonObject> ReplacementSettings = MakeShared<FJsonObject>();
    ReplacementSettings->SetStringField(TEXT("compression_settings"), TEXT("TC_Masks"));
    ReplacementSettings->SetBoolField(TEXT("srgb"), false);
    ReplacementSettings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
    ReplacementSettings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_WorldSpecular"));
    ReplacementSettings->SetStringField(TEXT("address_x"), TEXT("TA_Wrap"));
    ReplacementSettings->SetStringField(TEXT("address_y"), TEXT("TA_Mirror"));

    TSharedPtr<FJsonObject> ReplacementParams = MakeShared<FJsonObject>();
    ReplacementParams->SetStringField(TEXT("destination"), RequestedPath);
    ReplacementParams->SetStringField(TEXT("bytes_b64"), ReplacementPngB64);
    ReplacementParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    ReplacementParams->SetStringField(TEXT("conflict_policy"), TEXT("replace"));
    ReplacementParams->SetObjectField(TEXT("settings"), ReplacementSettings);
    ReplacementParams->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult ReplacementResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ReplacementParams);
    TestTrue(TEXT("replace policy succeeds"), ReplacementResult.bSuccess);
    if (!ReplacementResult.bSuccess || !ReplacementResult.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Replacement import error: %s (code %d)"),
            *ReplacementResult.ErrorMessage, ReplacementResult.ErrorCode));
        return false;
    }

    FString ReturnedRequestedPath;
    FString ReturnedAssetPath;
    FString ReturnedPolicy;
    bool bCreated = true;
    bool bReplaced = false;
    ReplacementResult.Result->TryGetStringField(TEXT("requested_asset_path"), ReturnedRequestedPath);
    ReplacementResult.Result->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath);
    ReplacementResult.Result->TryGetStringField(TEXT("conflict_policy"), ReturnedPolicy);
    ReplacementResult.Result->TryGetBoolField(TEXT("created"), bCreated);
    ReplacementResult.Result->TryGetBoolField(TEXT("replaced"), bReplaced);
    TestEqual(TEXT("replace requested path remains exact"), ReturnedRequestedPath, RequestedPath);
    TestEqual(TEXT("replace output path remains exact"), ReturnedAssetPath, RequestedPath);
    TestEqual(TEXT("replace reports policy"), ReturnedPolicy, FString(TEXT("replace")));
    TestFalse(TEXT("replace does not report created"), bCreated);
    TestTrue(TEXT("replace reports replaced"), bReplaced);

    UTexture2D* ReplacedTexture = FindTextureAtPackagePath(RequestedPath);
    TestTrue(TEXT("replace preserves UTexture2D object identity"), ReplacedTexture == OriginalTexture);
    TestTrue(TEXT("replace preserves package identity"),
        ReplacedTexture && ReplacedTexture->GetOutermost() == OriginalPackage);
    if (!ReplacedTexture)
    {
        return false;
    }

#if WITH_EDITOR
    TestEqual(TEXT("replace updates source width"), ReplacedTexture->Source.GetSizeX(), (int64)4);
    TestEqual(TEXT("replace updates source height"), ReplacedTexture->Source.GetSizeY(), (int64)4);
    TArray64<uint8> SourceMipData;
    TestTrue(TEXT("replace source mip can be read"), ReplacedTexture->Source.GetMipData(SourceMipData, 0));
    TestEqual(TEXT("replace source mip byte count"), SourceMipData.Num(), (int64)(4 * 4 * 4));
    if (SourceMipData.Num() >= 4)
    {
        TestEqual(TEXT("replace source first pixel blue channel"), SourceMipData[0], (uint8)255);
        TestEqual(TEXT("replace source first pixel green channel"), SourceMipData[1], (uint8)0);
        TestEqual(TEXT("replace source first pixel red channel"), SourceMipData[2], (uint8)0);
        TestEqual(TEXT("replace source first pixel alpha channel"), SourceMipData[3], (uint8)255);
    }
#endif

    const FTexturePlatformData* PlatformData = ReplacedTexture->GetPlatformData();
    TestNotNull(TEXT("replace installs platform data"), PlatformData);
    if (PlatformData)
    {
        TestEqual(TEXT("replace platform width"), PlatformData->SizeX, 4);
        TestEqual(TEXT("replace platform height"), PlatformData->SizeY, 4);
        TestTrue(TEXT("replace platform mip data exists"), PlatformData->Mips.Num() > 0);
    }
    TestEqual(TEXT("replace updates compression"), ReplacedTexture->CompressionSettings, TC_Masks);
    TestFalse(TEXT("replace updates sRGB"), ReplacedTexture->SRGB != 0);
    TestEqual(TEXT("replace updates mip generation"), ReplacedTexture->MipGenSettings, TMGS_NoMipmaps);
    TestEqual(TEXT("replace updates LOD group"), ReplacedTexture->LODGroup, TEXTUREGROUP_WorldSpecular);
    TestEqual(TEXT("replace updates AddressX"), ReplacedTexture->AddressX, TA_Wrap);
    TestEqual(TEXT("replace updates AddressY"), ReplacedTexture->AddressY, TA_Mirror);
    TestTrue(TEXT("replace marks package dirty"), OriginalPackage->IsDirty());

    return true;
}

/** A failed first save must remove the created UObject, registry entry, and every package segment. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureCreateSaveFailureCleanupTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.CreateSaveFailureCleansAllSegments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureCreateSaveFailureCleanupTest::RunTest(const FString& Parameters)
{
    const FString FixtureFolder = FString::Printf(
        TEXT("/Game/Tests/Monolith/Asset/Textures/CreateRollback_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString RequestedPath = FixtureFolder + TEXT("/T_CreateRollback");
    CleanupSavedTextureAtPackagePath(RequestedPath);

    FString HeaderFilename = FPackageName::LongPackageNameToFilename(
        RequestedPath,
        FPackageName::GetAssetPackageExtension());
    HeaderFilename = FPaths::ConvertRelativePathToFull(HeaderFilename);
    const FString BlockingParentFilename = FPaths::GetPath(HeaderFilename);
    IFileManager::Get().MakeDirectory(
        *FPaths::GetPath(BlockingParentFilename),
        /*Tree=*/true);
    if (!TestTrue(
            TEXT("save-failure fixture creates a file where the package directory must be"),
            FFileHelper::SaveStringToFile(TEXT("block package directory creation"), *BlockingParentFilename)))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"), RequestedPath);
    Params->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Yellow, 2, 2));
    Params->SetStringField(TEXT("format_hint"), TEXT("png"));
    Params->SetBoolField(TEXT("save"), true);
    AddExpectedError(
        TEXT("Error moving file"),
        EAutomationExpectedErrorFlags::Contains,
        /*ExpectedNumberOfOccurrences=*/1);
    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), Params);

    IFileManager::Get().Delete(
        *BlockingParentFilename,
        /*RequireExists=*/false,
        /*EvenReadOnly=*/true,
        /*Quiet=*/true);

    TestFalse(TEXT("blocked first save fails"), Result.bSuccess);
    TestTrue(TEXT("blocked first save reports SavePackage"), Result.ErrorMessage.Contains(TEXT("SavePackage failed")));
    TestNull(TEXT("failed first save removes exact texture object"), FindTextureAtPackagePath(RequestedPath));
    TestNull(TEXT("failed first save unloads exact package"), FindPackage(nullptr, *RequestedPath));
    TestFalse(TEXT("failed first save leaves no editor asset"), UEditorAssetLibrary::DoesAssetExist(RequestedPath));

    TArray<FAssetData> RegisteredAssets;
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().GetAssetsByPackageName(
        FName(*RequestedPath),
        RegisteredAssets,
        /*bIncludeOnlyOnDiskAssets=*/false);
    TestEqual(TEXT("failed first save leaves no registry entry"), RegisteredAssets.Num(), 0);

    for (const FString& PackageFilename : GetTexturePackageFilenames(RequestedPath))
    {
        TestFalse(
            *FString::Printf(TEXT("failed first save leaves no package file: %s"), *PackageFilename),
            IFileManager::Get().FileExists(*PackageFilename));
    }

    CleanupSavedTextureAtPackagePath(RequestedPath);
    return true;
}

/** A failed replacement save must restore both UObject state and the persisted file. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureReplaceSaveFailureRollbackTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.ReplaceSaveFailureRollsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureReplaceSaveFailureRollbackTest::RunTest(const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictReplaceRollback"));
    CleanupSavedTextureAtPackagePath(RequestedPath);

    TSharedPtr<FJsonObject> InitialSettings = MakeShared<FJsonObject>();
    InitialSettings->SetStringField(TEXT("compression_settings"), TEXT("TC_Default"));
    InitialSettings->SetBoolField(TEXT("srgb"), true);
    InitialSettings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
    InitialSettings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_World"));
    InitialSettings->SetStringField(TEXT("address_x"), TEXT("TA_Clamp"));
    InitialSettings->SetStringField(TEXT("address_y"), TEXT("TA_Clamp"));

    TSharedPtr<FJsonObject> InitialParams = MakeShared<FJsonObject>();
    InitialParams->SetStringField(TEXT("destination"), RequestedPath);
    InitialParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Red, 2, 2));
    InitialParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    InitialParams->SetObjectField(TEXT("settings"), InitialSettings);
    InitialParams->SetBoolField(TEXT("save"), true);
    const FMonolithActionResult InitialResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), InitialParams);
    if (!TestTrue(TEXT("rollback fixture initial save succeeds"), InitialResult.bSuccess))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    UTexture2D* OriginalTexture = FindTextureAtPackagePath(RequestedPath);
    if (!TestNotNull(TEXT("rollback fixture texture exists"), OriginalTexture))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }
    UPackage* OriginalPackage = OriginalTexture->GetOutermost();
    TestFalse(TEXT("rollback fixture starts clean"), OriginalPackage->IsDirty());
    const TextureFilter OriginalFilter = TF_Nearest;
    const ETexturePowerOfTwoSetting::Type OriginalPowerOfTwoMode =
        ETexturePowerOfTwoSetting::ResizeToSpecificResolution;
    const int32 OriginalResizeX = 2;
    const int32 OriginalResizeY = 2;
    const int32 OriginalMaxTextureSize = 64;
    const int32 OriginalCinematicMipLevels = 1;
    const FGuid OriginalLightingGuid(0x12345678, 0x23456789, 0x3456789A, 0x456789AB);
    UTexture2D* OriginalCPUCopyTexture = NewObject<UTexture2D>(
        GetTransientPackage(),
        NAME_None,
        RF_Transient);
    OriginalTexture->Filter = OriginalFilter;
    OriginalTexture->bUseLegacyGamma = true;
    OriginalTexture->PowerOfTwoMode = OriginalPowerOfTwoMode;
    OriginalTexture->ResizeDuringBuildX = OriginalResizeX;
    OriginalTexture->ResizeDuringBuildY = OriginalResizeY;
    OriginalTexture->MaxTextureSize = OriginalMaxTextureSize;
    OriginalTexture->NumCinematicMipLevels = OriginalCinematicMipLevels;
    OriginalTexture->DeferCompression = true;
    OriginalTexture->SetLightingGuid(OriginalLightingGuid);
    OriginalTexture->CPUCopyTexture = OriginalCPUCopyTexture;
#if WITH_EDITOR
    const FGuid OriginalSourcePersistentId = OriginalTexture->Source.GetPersistentId();
    const FString OriginalSourceIdString = OriginalTexture->Source.GetIdString();
    const ETextureSourceCompressionFormat OriginalSourceCompression =
        OriginalTexture->Source.GetSourceCompression();
#endif
    OriginalTexture->FinishCachePlatformData();
    FTexturePlatformData* OriginalPlatformData = OriginalTexture->GetPlatformData();
    if (!TestNotNull(TEXT("rollback fixture platform data exists"), OriginalPlatformData))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }
    if (!TestTrue(
            TEXT("rollback fixture platform mips inline"),
            OriginalPlatformData->TryInlineMipData(/*FirstMipToLoad=*/0, OriginalTexture->GetPathName())))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }
    const EPixelFormat OriginalPlatformFormat = OriginalPlatformData->PixelFormat;
    OriginalPlatformData->PreEncodeMipsHash = 0x123456789ABCDEF0ull;
    OriginalPlatformData->DerivedDataKey.Emplace<FString>(TEXT("MonolithRollbackDerivedData"));
    OriginalPlatformData->FetchOrBuildDerivedDataKey.Emplace<FString>(TEXT("MonolithRollbackFetchOrBuild"));
    OriginalPlatformData->FetchFirstDerivedDataKey.Emplace<FString>(TEXT("MonolithRollbackFetchFirst"));
    OriginalPlatformData->ResultMetadata.Encoder = TEXT("MonolithRollbackEncoder");
    OriginalPlatformData->ResultMetadata.EncodedFormat = OriginalPlatformFormat;
    OriginalPlatformData->ResultMetadata.bIsValid = true;
    FTexture2DMipMap* OriginalPlatformMipAddress =
        OriginalPlatformData->Mips.Num() > 0 ? &OriginalPlatformData->Mips[0] : nullptr;
    const FString CookedFixtureKey(TEXT("MonolithRollbackCooked"));
    FTexturePlatformData* CookedFixturePlatformData = new FTexturePlatformData();
    CookedFixturePlatformData->SizeX = 7;
    CookedFixturePlatformData->SizeY = 9;
    OriginalTexture->CookedPlatformData.Add(CookedFixtureKey, CookedFixturePlatformData);
    OriginalPackage->SetDirtyFlag(false);
    TArray64<uint8> OriginalPlatformMipBytes;
    if (OriginalPlatformData->Mips.Num() > 0)
    {
        FTexture2DMipMap& OriginalMip = OriginalPlatformData->Mips[0];
        const int64 OriginalMipSize = OriginalMip.BulkData.GetBulkDataSize();
        OriginalPlatformMipBytes.SetNumUninitialized(OriginalMipSize);
        if (OriginalMipSize > 0)
        {
            const void* OriginalMipData = OriginalMip.BulkData.LockReadOnly();
            if (!TestNotNull(TEXT("rollback fixture platform mip is readable"), OriginalMipData))
            {
                OriginalMip.BulkData.Unlock();
                CleanupSavedTextureAtPackagePath(RequestedPath);
                return false;
            }
            FMemory::Memcpy(
                OriginalPlatformMipBytes.GetData(),
                OriginalMipData,
                static_cast<SIZE_T>(OriginalMipSize));
            OriginalMip.BulkData.Unlock();
        }
    }

    FString Filename = FPackageName::LongPackageNameToFilename(
        RequestedPath,
        FPackageName::GetAssetPackageExtension());
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    TArray<uint8> PersistedBefore;
    if (!TestTrue(TEXT("rollback fixture file can be read"), FFileHelper::LoadFileToArray(PersistedBefore, *Filename)))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!TestTrue(TEXT("rollback fixture file was made read-only"), PlatformFile.SetReadOnly(*Filename, true)))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    TSharedPtr<FJsonObject> ReplacementSettings = MakeShared<FJsonObject>();
    ReplacementSettings->SetStringField(TEXT("compression_settings"), TEXT("TC_Masks"));
    ReplacementSettings->SetBoolField(TEXT("srgb"), false);
    ReplacementSettings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
    ReplacementSettings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_WorldSpecular"));
    ReplacementSettings->SetStringField(TEXT("address_x"), TEXT("TA_Wrap"));
    ReplacementSettings->SetStringField(TEXT("address_y"), TEXT("TA_Mirror"));

    TSharedPtr<FJsonObject> ReplacementParams = MakeShared<FJsonObject>();
    ReplacementParams->SetStringField(TEXT("destination"), RequestedPath);
    ReplacementParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Blue, 4, 4));
    ReplacementParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    ReplacementParams->SetStringField(TEXT("conflict_policy"), TEXT("replace"));
    ReplacementParams->SetObjectField(TEXT("settings"), ReplacementSettings);
    ReplacementParams->SetBoolField(TEXT("save"), true);
    AddExpectedError(
        TEXT("Cannot remove"),
        EAutomationExpectedErrorFlags::Contains,
        /*ExpectedNumberOfOccurrences=*/1);
    const FMonolithActionResult ReplacementResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ReplacementParams);
    PlatformFile.SetReadOnly(*Filename, false);

    TestFalse(TEXT("read-only replacement save fails"), ReplacementResult.bSuccess);
    TestTrue(TEXT("save failure is reported"), ReplacementResult.ErrorMessage.Contains(TEXT("SavePackage failed")));
    UTexture2D* RestoredTexture = FindTextureAtPackagePath(RequestedPath);
    TestTrue(TEXT("failed replace preserves UObject identity"), RestoredTexture == OriginalTexture);
    if (RestoredTexture)
    {
#if WITH_EDITOR
        TestEqual(TEXT("failed replace restores source width"), RestoredTexture->Source.GetSizeX(), (int64)2);
        TestEqual(TEXT("failed replace restores source height"), RestoredTexture->Source.GetSizeY(), (int64)2);
        TestEqual(
            TEXT("failed replace preserves source persistent identity"),
            RestoredTexture->Source.GetPersistentId(),
            OriginalSourcePersistentId);
        TestEqual(
            TEXT("failed replace preserves source DDC identity"),
            RestoredTexture->Source.GetIdString(),
            OriginalSourceIdString);
        TestEqual(
            TEXT("failed replace preserves source compression"),
            RestoredTexture->Source.GetSourceCompression(),
            OriginalSourceCompression);
        TArray64<uint8> RestoredPixels;
        TestTrue(TEXT("failed replace restores readable source"), RestoredTexture->Source.GetMipData(RestoredPixels, 0));
        if (RestoredPixels.Num() >= 4)
        {
            TestEqual(TEXT("failed replace restores red pixel blue channel"), RestoredPixels[0], (uint8)0);
            TestEqual(TEXT("failed replace restores red pixel red channel"), RestoredPixels[2], (uint8)255);
        }
#endif
        FTexturePlatformData* RestoredPlatformData = RestoredTexture->GetPlatformData();
        TestNotNull(TEXT("failed replace restores platform data"), RestoredPlatformData);
        if (RestoredPlatformData)
        {
            TestTrue(
                TEXT("failed replace preserves top-level platform-data ownership"),
                RestoredPlatformData == OriginalPlatformData);
            TestEqual(TEXT("failed replace restores platform width"), RestoredPlatformData->SizeX, 2);
            TestEqual(TEXT("failed replace restores platform height"), RestoredPlatformData->SizeY, 2);
            TestEqual(
                TEXT("failed replace restores platform format"),
                RestoredPlatformData->PixelFormat,
                OriginalPlatformFormat);
            if (RestoredPlatformData->Mips.Num() > 0)
            {
                FTexture2DMipMap& RestoredMip = RestoredPlatformData->Mips[0];
                TestTrue(
                    TEXT("failed replace restores exact platform mip ownership"),
                    &RestoredMip == OriginalPlatformMipAddress);
                const int64 RestoredMipSize = RestoredMip.BulkData.GetBulkDataSize();
                TestEqual(
                    TEXT("failed replace restores platform mip byte count"),
                    RestoredMipSize,
                    OriginalPlatformMipBytes.Num());
                if (RestoredMipSize > 0)
                {
                    const uint8* RestoredMipBytes = static_cast<const uint8*>(RestoredMip.BulkData.LockReadOnly());
                    TestNotNull(TEXT("failed replace restores readable platform mip"), RestoredMipBytes);
                    if (RestoredMipBytes)
                    {
                        TestTrue(
                            TEXT("failed replace restores exact platform mip bytes"),
                            RestoredMipSize == OriginalPlatformMipBytes.Num()
                                && FMemory::Memcmp(
                                    RestoredMipBytes,
                                    OriginalPlatformMipBytes.GetData(),
                                    static_cast<SIZE_T>(RestoredMipSize)) == 0);
                    }
                    RestoredMip.BulkData.Unlock();
                }
            }
            TestEqual(
                TEXT("failed replace restores platform pre-encode hash"),
                RestoredPlatformData->PreEncodeMipsHash,
                (uint64)0x123456789ABCDEF0ull);
            TestTrue(
                TEXT("failed replace restores platform derived-data key"),
                RestoredPlatformData->DerivedDataKey.IsType<FString>()
                    && RestoredPlatformData->DerivedDataKey.Get<FString>()
                        == TEXT("MonolithRollbackDerivedData"));
            TestTrue(
                TEXT("failed replace restores platform fetch-or-build key"),
                RestoredPlatformData->FetchOrBuildDerivedDataKey.IsType<FString>()
                    && RestoredPlatformData->FetchOrBuildDerivedDataKey.Get<FString>()
                        == TEXT("MonolithRollbackFetchOrBuild"));
            TestTrue(
                TEXT("failed replace restores platform fetch-first key"),
                RestoredPlatformData->FetchFirstDerivedDataKey.IsType<FString>()
                    && RestoredPlatformData->FetchFirstDerivedDataKey.Get<FString>()
                        == TEXT("MonolithRollbackFetchFirst"));
        }
        FTexturePlatformData* const* RestoredCookedPlatformData =
            RestoredTexture->CookedPlatformData.Find(CookedFixtureKey);
        TestTrue(
            TEXT("failed replace restores exact cooked platform-data ownership"),
            RestoredCookedPlatformData && *RestoredCookedPlatformData == CookedFixturePlatformData);
        TestEqual(TEXT("failed replace restores compression"), RestoredTexture->CompressionSettings, TC_Default);
        TestTrue(TEXT("failed replace restores sRGB"), RestoredTexture->SRGB != 0);
        TestEqual(TEXT("failed replace restores LOD group"), RestoredTexture->LODGroup, TEXTUREGROUP_World);
        TestEqual(TEXT("failed replace restores AddressX"), RestoredTexture->AddressX, TA_Clamp);
        TestEqual(TEXT("failed replace restores AddressY"), RestoredTexture->AddressY, TA_Clamp);
        TestEqual(TEXT("failed replace restores filter"), RestoredTexture->Filter, OriginalFilter);
        TestTrue(TEXT("failed replace restores legacy gamma"), RestoredTexture->bUseLegacyGamma != 0);
        TestEqual(
            TEXT("failed replace restores power-of-two mode"),
            RestoredTexture->PowerOfTwoMode,
            OriginalPowerOfTwoMode);
        TestEqual(TEXT("failed replace restores resize X"), RestoredTexture->ResizeDuringBuildX, OriginalResizeX);
        TestEqual(TEXT("failed replace restores resize Y"), RestoredTexture->ResizeDuringBuildY, OriginalResizeY);
        TestEqual(
            TEXT("failed replace restores max texture size"),
            RestoredTexture->MaxTextureSize,
            OriginalMaxTextureSize);
        TestEqual(
            TEXT("failed replace restores cinematic mip count"),
            RestoredTexture->NumCinematicMipLevels,
            OriginalCinematicMipLevels);
        TestTrue(TEXT("failed replace restores defer-compression flag"), RestoredTexture->DeferCompression != 0);
        TestEqual(
            TEXT("failed replace restores lighting GUID"),
            RestoredTexture->GetLightingGuid(),
            OriginalLightingGuid);
        TestTrue(
            TEXT("failed replace restores CPU-copy texture identity"),
            RestoredTexture->CPUCopyTexture.Get() == OriginalCPUCopyTexture);
        TestFalse(TEXT("failed replace restores package dirty state"), RestoredTexture->GetOutermost()->IsDirty());
    }

    TArray<uint8> PersistedAfter;
    TestTrue(TEXT("persisted file remains readable after failed replace"), FFileHelper::LoadFileToArray(PersistedAfter, *Filename));
    TestTrue(TEXT("failed replace leaves persisted bytes unchanged"), PersistedAfter == PersistedBefore);

    CleanupSavedTextureAtPackagePath(RequestedPath);
    return true;
}

/** Invalid shared ownership between running and cooked platform data must fail before mutation. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureReplaceAliasedCookedDataRejectTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.ReplaceRejectsAliasedCookedData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureReplaceAliasedCookedDataRejectTest::RunTest(
    const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictReplaceAliasedCooked"));
    CleanupSavedTextureAtPackagePath(RequestedPath);

    TSharedPtr<FJsonObject> InitialParams = MakeShared<FJsonObject>();
    InitialParams->SetStringField(TEXT("destination"), RequestedPath);
    InitialParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Red, 2, 2));
    InitialParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    InitialParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult InitialResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), InitialParams);
    if (!TestTrue(TEXT("aliased-cooked fixture creation succeeds"), InitialResult.bSuccess))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    UTexture2D* OriginalTexture = FindTextureAtPackagePath(RequestedPath);
    FTexturePlatformData* OriginalPlatformData = OriginalTexture ? OriginalTexture->GetPlatformData() : nullptr;
    if (!TestNotNull(TEXT("aliased-cooked fixture texture exists"), OriginalTexture)
        || !TestNotNull(TEXT("aliased-cooked fixture platform data exists"), OriginalPlatformData))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    const FString AliasKey(TEXT("MonolithRunningDataAlias"));
    OriginalTexture->CookedPlatformData.Add(AliasKey, OriginalPlatformData);

    TSharedPtr<FJsonObject> ReplacementParams = MakeShared<FJsonObject>();
    ReplacementParams->SetStringField(TEXT("destination"), RequestedPath);
    ReplacementParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Blue, 4, 4));
    ReplacementParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    ReplacementParams->SetStringField(TEXT("conflict_policy"), TEXT("replace"));
    ReplacementParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult ReplacementResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ReplacementParams);

    TestFalse(TEXT("aliased cooked ownership is rejected"), ReplacementResult.bSuccess);
    TestTrue(
        TEXT("aliased cooked rejection is diagnostic"),
        ReplacementResult.ErrorMessage.Contains(TEXT("aliased cooked platform data")));
    TestTrue(
        TEXT("aliased cooked rejection preserves UObject identity"),
        FindTextureAtPackagePath(RequestedPath) == OriginalTexture);
    TestTrue(
        TEXT("aliased cooked rejection preserves running platform identity"),
        OriginalTexture->GetPlatformData() == OriginalPlatformData);
    TestEqual(TEXT("aliased cooked rejection preserves source width"), OriginalTexture->Source.GetSizeX(), (int64)2);
    TestEqual(TEXT("aliased cooked rejection preserves source height"), OriginalTexture->Source.GetSizeY(), (int64)2);

    // Remove only the deliberately invalid second owner before normal fixture cleanup.
    OriginalTexture->CookedPlatformData.Remove(AliasKey);
    CleanupSavedTextureAtPackagePath(RequestedPath);
    return true;
}

/** A texture that had no running platform data must return to that exact state. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportTextureReplaceNullPlatformSaveFailureRollbackTest,
    "MonolithAsset.ImportTextureFromBytes.ConflictPolicy.ReplaceNullPlatformSaveFailureRollsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureReplaceNullPlatformSaveFailureRollbackTest::RunTest(
    const FString& Parameters)
{
    const FString RequestedPath = MakeUniqueTestAssetPath(TEXT("T_ConflictReplaceNullPlatformRollback"));
    CleanupSavedTextureAtPackagePath(RequestedPath);

    TSharedPtr<FJsonObject> InitialParams = MakeShared<FJsonObject>();
    InitialParams->SetStringField(TEXT("destination"), RequestedPath);
    InitialParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Red, 2, 2));
    InitialParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    InitialParams->SetBoolField(TEXT("save"), true);
    const FMonolithActionResult InitialResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), InitialParams);
    if (!TestTrue(TEXT("null-platform fixture initial save succeeds"), InitialResult.bSuccess))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    UTexture2D* OriginalTexture = FindTextureAtPackagePath(RequestedPath);
    if (!TestNotNull(TEXT("null-platform fixture texture exists"), OriginalTexture))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    // Deliberately construct a valid editor UObject state with neither source
    // art nor running platform data. Atomic replace must support this state,
    // not reject it or synthesize data during rollback.
    OriginalTexture->PreEditChange(nullptr);
    OriginalTexture->Source.Reset();
    OriginalTexture->PostEditChange();
    OriginalTexture->BlockOnAnyAsyncBuild();
    OriginalTexture->WaitForPendingInitOrStreaming();
    OriginalTexture->FinishCachePlatformData();
    OriginalTexture->ReleaseResource();
    FlushRenderingCommands();
    OriginalTexture->SetPlatformData(nullptr);
    OriginalTexture->GetOutermost()->SetDirtyFlag(false);
    TestFalse(TEXT("null-platform fixture source is invalid"), OriginalTexture->Source.IsValid());
    TestNull(TEXT("null-platform fixture has no running platform data"), OriginalTexture->GetPlatformData());

    FString Filename = FPackageName::LongPackageNameToFilename(
        RequestedPath,
        FPackageName::GetAssetPackageExtension());
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    TArray<uint8> PersistedBefore;
    if (!TestTrue(
            TEXT("null-platform fixture file can be read"),
            FFileHelper::LoadFileToArray(PersistedBefore, *Filename)))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!TestTrue(
            TEXT("null-platform fixture file was made read-only"),
            PlatformFile.SetReadOnly(*Filename, true)))
    {
        CleanupSavedTextureAtPackagePath(RequestedPath);
        return false;
    }

    TSharedPtr<FJsonObject> ReplacementParams = MakeShared<FJsonObject>();
    ReplacementParams->SetStringField(TEXT("destination"), RequestedPath);
    ReplacementParams->SetStringField(TEXT("bytes_b64"), MakeSolidPngB64(FColor::Blue, 4, 4));
    ReplacementParams->SetStringField(TEXT("format_hint"), TEXT("png"));
    ReplacementParams->SetStringField(TEXT("conflict_policy"), TEXT("replace"));
    ReplacementParams->SetBoolField(TEXT("save"), true);
    AddExpectedError(
        TEXT("Cannot remove"),
        EAutomationExpectedErrorFlags::Contains,
        /*ExpectedNumberOfOccurrences=*/1);
    const FMonolithActionResult ReplacementResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_texture_from_bytes"), ReplacementParams);
    PlatformFile.SetReadOnly(*Filename, false);

    TestFalse(TEXT("null-platform read-only replacement save fails"), ReplacementResult.bSuccess);
    TestTrue(
        TEXT("null-platform save failure is reported"),
        ReplacementResult.ErrorMessage.Contains(TEXT("SavePackage failed")));
    UTexture2D* RestoredTexture = FindTextureAtPackagePath(RequestedPath);
    TestTrue(TEXT("null-platform rollback preserves UObject identity"), RestoredTexture == OriginalTexture);
    if (RestoredTexture)
    {
        TestFalse(TEXT("null-platform rollback restores invalid source"), RestoredTexture->Source.IsValid());
        TestNull(
            TEXT("null-platform rollback restores absent running platform data"),
            RestoredTexture->GetPlatformData());
        TestFalse(
            TEXT("null-platform rollback restores clean package state"),
            RestoredTexture->GetOutermost()->IsDirty());
    }

    TArray<uint8> PersistedAfter;
    TestTrue(
        TEXT("null-platform persisted file remains readable"),
        FFileHelper::LoadFileToArray(PersistedAfter, *Filename));
    TestTrue(
        TEXT("null-platform failed replace leaves persisted bytes unchanged"),
        PersistedAfter == PersistedBefore);

    CleanupSavedTextureAtPackagePath(RequestedPath);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
