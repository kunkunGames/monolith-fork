// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

// Image fixture generation
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

// Texture verification
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
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

    FString MakeSolidPngB64(const FColor& Color)
    {
        TArray<FColor> Pixels;
        Pixels.Init(Color, 4);
        return EncodePngB64(2, 2, Pixels);
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

#endif // WITH_DEV_AUTOMATION_TESTS
