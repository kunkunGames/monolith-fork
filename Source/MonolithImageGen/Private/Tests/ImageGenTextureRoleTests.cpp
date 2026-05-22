// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

namespace
{
    void EnsureImageGenModuleLoaded()
    {
        FModuleManager::Get().LoadModule(TEXT("MonolithImageGen"));
    }

    bool JsonArrayHasString(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& Expected)
    {
        for (const TSharedPtr<FJsonValue>& Value : Values)
        {
            if (Value.IsValid() && Value->AsString() == Expected)
            {
                return true;
            }
        }
        return false;
    }

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

    FString MakeEdgeBackgroundIconPngB64()
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(245, 245, 245, 255), 16);
        Pixels[5] = FColor(220, 30, 30, 255);
        Pixels[6] = FColor(220, 30, 30, 255);
        Pixels[9] = FColor(220, 30, 30, 255);
        Pixels[10] = FColor(220, 30, 30, 255);
        return EncodePngB64(4, 4, Pixels);
    }

    UTexture2D* FindTextureAtPackagePath(const FString& AssetPath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
        return FindObject<UTexture2D>(nullptr, *(AssetPath + TEXT(".") + AssetName));
    }

    FString ExpectedSourcePngPath(const FString& AssetPath)
    {
        FString RelativePath = AssetPath;
        const FString GeneratedRoot = TEXT("/Game/GeneratedImages/");
        if (RelativePath.StartsWith(GeneratedRoot))
        {
            RelativePath.RightChopInline(GeneratedRoot.Len());
        }
        else if (RelativePath.StartsWith(TEXT("/Game/")))
        {
            RelativePath.RightChopInline(6);
        }
        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectDir(), TEXT("GeneratedImages"), RelativePath + TEXT(".png")));
    }

    FString ReferenceRootPath()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("GeneratedImages")));
    }

    TSet<FString> ListArchivedReferencePngFiles()
    {
        TSet<FString> Paths;
        TArray<FString> Filenames;
        const FString Pattern = FPaths::Combine(ReferenceRootPath(), TEXT("Ref_*.png"));
        IFileManager::Get().FindFiles(Filenames, *Pattern, true, false);
        for (const FString& Filename : Filenames)
        {
            Paths.Add(FPaths::Combine(ReferenceRootPath(), Filename));
        }
        return Paths;
    }

    bool HasPngSignature(const FString& FilePath)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() < 8)
        {
            return false;
        }
        return Bytes[0] == 0x89 && Bytes[1] == 0x50 && Bytes[2] == 0x4e && Bytes[3] == 0x47
            && Bytes[4] == 0x0d && Bytes[5] == 0x0a && Bytes[6] == 0x1a && Bytes[7] == 0x0a;
    }

    bool PngFileHasTransparentPixel(const FString& FilePath)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() == 0)
        {
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
        {
            return false;
        }

        TArray<uint8> RawBgra;
        if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, RawBgra) || RawBgra.Num() == 0)
        {
            return false;
        }
        for (int32 Index = 3; Index < RawBgra.Num(); Index += 4)
        {
            if (RawBgra[Index] < 255)
            {
                return true;
            }
        }
        return false;
    }

    UTexture2D* MakeReferenceTextureFixture(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        UTexture2D* Texture = FindObject<UTexture2D>(Package, *AssetName);
        if (!Texture)
        {
            Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
        }
        if (!Texture)
        {
            return nullptr;
        }

        Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8);
        uint8* MipData = Texture->Source.LockMip(0);
        if (!MipData)
        {
            return nullptr;
        }

        const FColor Pixels[] = {
            FColor(255, 0, 0, 255),
            FColor(0, 255, 0, 192),
            FColor(0, 0, 255, 128),
            FColor(255, 255, 255, 64)
        };
        for (int32 Index = 0; Index < 4; ++Index)
        {
            MipData[Index * 4 + 0] = Pixels[Index].B;
            MipData[Index * 4 + 1] = Pixels[Index].G;
            MipData[Index * 4 + 2] = Pixels[Index].R;
            MipData[Index * 4 + 3] = Pixels[Index].A;
        }
        Texture->Source.UnlockMip(0);
        return Texture;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithImageGenTextureRolesDefaultsTest,
    "MonolithImageGen.TextureRoles.Defaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenTextureRolesDefaultsTest::RunTest(const FString& Parameters)
{
    EnsureImageGenModuleLoaded();

    const FMonolithActionResult Defaults = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("get_image_generation_defaults"), MakeShared<FJsonObject>());

    TestTrue(TEXT("get_image_generation_defaults succeeds"), Defaults.bSuccess);
    if (!Defaults.bSuccess || !Defaults.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Defaults error: %s (code %d)"),
            *Defaults.ErrorMessage, Defaults.ErrorCode));
        return false;
    }

    FString Model;
    Defaults.Result->TryGetStringField(TEXT("model"), Model);
    TestEqual(TEXT("default model is gpt-5.5"), Model, FString(TEXT("gpt-5.5")));

    FString AssetPath;
    Defaults.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
    TestEqual(TEXT("default asset path"), AssetPath, FString(TEXT("/Game/GeneratedImages")));

    FString TextureRole;
    Defaults.Result->TryGetStringField(TEXT("texture_role"), TextureRole);
    TestEqual(TEXT("default texture_role"), TextureRole, FString(TEXT("basecolor")));

    FString Provider;
    Defaults.Result->TryGetStringField(TEXT("ima2_provider"), Provider);
    TestEqual(TEXT("default ima2 provider"), Provider, FString(TEXT("oauth")));

    const TArray<TSharedPtr<FJsonValue>>* Roles = nullptr;
    TestTrue(TEXT("texture_roles returned"), Defaults.Result->TryGetArrayField(TEXT("texture_roles"), Roles) && Roles);
    if (Roles)
    {
        const FString ExpectedRoles[] = {
            TEXT("ui_icon"), TEXT("sprite"), TEXT("decal"), TEXT("basecolor"), TEXT("world_tile"),
            TEXT("normal"), TEXT("orm_mask"), TEXT("height"), TEXT("emissive")
        };
        for (const FString& ExpectedRole : ExpectedRoles)
        {
            TestTrue(FString::Printf(TEXT("texture_roles contains %s"), *ExpectedRole),
                JsonArrayHasString(*Roles, ExpectedRole));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* ReferenceFields = nullptr;
    TestTrue(TEXT("reference_input_fields returned"),
        Defaults.Result->TryGetArrayField(TEXT("reference_input_fields"), ReferenceFields) && ReferenceFields);
    if (ReferenceFields)
    {
        const FString ExpectedFields[] = {
            TEXT("references"),
            TEXT("reference_images"),
            TEXT("reference_image_paths"),
            TEXT("reference_png_paths"),
            TEXT("reference_asset_paths")
        };
        for (const FString& ExpectedField : ExpectedFields)
        {
            TestTrue(FString::Printf(TEXT("reference_input_fields contains %s"), *ExpectedField),
                JsonArrayHasString(*ReferenceFields, ExpectedField));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Ima2Formats = nullptr;
    TestTrue(TEXT("ima2_formats returned"),
        Defaults.Result->TryGetArrayField(TEXT("ima2_formats"), Ima2Formats) && Ima2Formats);
    if (Ima2Formats)
    {
        TestTrue(TEXT("ima2_formats contains png"), JsonArrayHasString(*Ima2Formats, TEXT("png")));
        TestFalse(TEXT("ima2_formats excludes unsupported jpeg"), JsonArrayHasString(*Ima2Formats, TEXT("jpeg")));
        TestFalse(TEXT("ima2_formats excludes unsupported webp"), JsonArrayHasString(*Ima2Formats, TEXT("webp")));
    }

    bool bComposePrompt = false;
    TestTrue(TEXT("compose_prompt default returned"), Defaults.Result->TryGetBoolField(TEXT("compose_prompt"), bComposePrompt));
    TestTrue(TEXT("compose_prompt defaults true"), bComposePrompt);

    const TSharedPtr<FJsonObject>* Presets = nullptr;
    TestTrue(TEXT("texture_role_presets returned"),
        Defaults.Result->TryGetObjectField(TEXT("texture_role_presets"), Presets) && Presets && Presets->IsValid());
    if (Presets && Presets->IsValid())
    {
        FString NormalPreset;
        TestTrue(TEXT("normal preset returned"),
            (*Presets)->TryGetStringField(TEXT("normal"), NormalPreset) && !NormalPreset.IsEmpty());
        FString UiPreset;
        TestTrue(TEXT("ui_icon preset returned"),
            (*Presets)->TryGetStringField(TEXT("ui_icon"), UiPreset) && !UiPreset.IsEmpty());
    }

    const FMonolithActionResult Models = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("list_image_models"), MakeShared<FJsonObject>());
    TestTrue(TEXT("list_image_models succeeds"), Models.bSuccess);
    if (Models.bSuccess && Models.Result.IsValid())
    {
        FString DefaultModel;
        Models.Result->TryGetStringField(TEXT("default_model"), DefaultModel);
        TestEqual(TEXT("list_image_models default_model"), DefaultModel, FString(TEXT("gpt-5.5")));

        FString DefaultPath;
        Models.Result->TryGetStringField(TEXT("default_asset_path"), DefaultPath);
        TestEqual(TEXT("list_image_models default path"), DefaultPath, FString(TEXT("/Game/GeneratedImages")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithImageGenValidationTest,
    "MonolithImageGen.TextureRoles.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenValidationTest::RunTest(const FString& Parameters)
{
    EnsureImageGenModuleLoaded();

    TSharedPtr<FJsonObject> WebpParams = MakeShared<FJsonObject>();
    WebpParams->SetStringField(TEXT("prompt"), TEXT("unsupported webp validation smoke"));
    WebpParams->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
    WebpParams->SetStringField(TEXT("format"), TEXT("webp"));
    WebpParams->SetNumberField(TEXT("timeout_seconds"), 1.0);
    WebpParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult WebpResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), WebpParams);
    TestFalse(TEXT("generate_image_via_ima2 rejects WebP before bridge call"), WebpResult.bSuccess);
    TestTrue(TEXT("WebP rejection explains Texture2D import support"),
        WebpResult.ErrorMessage.Contains(TEXT("WebP")));

    TSharedPtr<FJsonObject> JpegParams = MakeShared<FJsonObject>();
    JpegParams->SetStringField(TEXT("prompt"), TEXT("unsupported jpeg validation smoke"));
    JpegParams->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
    JpegParams->SetStringField(TEXT("format"), TEXT("jpeg"));
    JpegParams->SetNumberField(TEXT("timeout_seconds"), 1.0);
    JpegParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult JpegResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), JpegParams);
    TestFalse(TEXT("generate_image_via_ima2 rejects JPEG before bridge call"), JpegResult.bSuccess);
    TestTrue(TEXT("JPEG rejection explains PNG-only contract"),
        JpegResult.ErrorMessage.Contains(TEXT("JPEG")) && JpegResult.ErrorMessage.Contains(TEXT("png")));

    TSharedPtr<FJsonObject> RoleParams = MakeShared<FJsonObject>();
    RoleParams->SetStringField(TEXT("prompt"), TEXT("invalid role validation smoke"));
    RoleParams->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
    RoleParams->SetStringField(TEXT("format"), TEXT("png"));
    RoleParams->SetStringField(TEXT("texture_role"), TEXT("mystery_role"));
    RoleParams->SetBoolField(TEXT("compose_prompt"), true);
    RoleParams->SetNumberField(TEXT("timeout_seconds"), 1.0);
    RoleParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult RoleResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), RoleParams);
    TestFalse(TEXT("generate_image_via_ima2 rejects invalid texture_role before bridge call"), RoleResult.bSuccess);
    TestTrue(TEXT("texture_role rejection names the field"),
        RoleResult.ErrorMessage.Contains(TEXT("texture_role")));

    TSharedPtr<FJsonObject> NormalTransparentParams = MakeShared<FJsonObject>();
    NormalTransparentParams->SetStringField(TEXT("prompt"), TEXT("normal map transparent validation smoke"));
    NormalTransparentParams->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
    NormalTransparentParams->SetStringField(TEXT("format"), TEXT("png"));
    NormalTransparentParams->SetStringField(TEXT("texture_role"), TEXT("normal"));
    NormalTransparentParams->SetStringField(TEXT("background"), TEXT("transparent"));
    NormalTransparentParams->SetBoolField(TEXT("compose_prompt"), true);
    NormalTransparentParams->SetNumberField(TEXT("timeout_seconds"), 1.0);
    NormalTransparentParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult NormalTransparentResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), NormalTransparentParams);
    TestFalse(TEXT("generate_image_via_ima2 rejects transparent background for normal maps"), NormalTransparentResult.bSuccess);
    TestTrue(TEXT("normal transparent rejection explains background"),
        NormalTransparentResult.ErrorMessage.Contains(TEXT("transparent")));

    TSharedPtr<FJsonObject> QueryUrlParams = MakeShared<FJsonObject>();
    QueryUrlParams->SetStringField(TEXT("prompt"), TEXT("server url query validation smoke"));
    QueryUrlParams->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9?token=do-not-store"));
    QueryUrlParams->SetBoolField(TEXT("save"), false);
    const FMonolithActionResult QueryUrlResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), QueryUrlParams);
    TestFalse(TEXT("generate_image_via_ima2 rejects server_url query strings"), QueryUrlResult.bSuccess);
    TestTrue(TEXT("query rejection does not echo query secret"),
        !QueryUrlResult.ErrorMessage.Contains(TEXT("do-not-store")));

    const FString PngB64 = MakeSolidPngB64(FColor(16, 48, 96, 255));
    TestFalse(TEXT("data URL PNG fixture encoded"), PngB64.IsEmpty());
    if (!PngB64.IsEmpty())
    {
        TSharedPtr<FJsonObject> DataUrlParams = MakeShared<FJsonObject>();
        DataUrlParams->SetStringField(TEXT("bytes_b64"), FString(TEXT("data:image/png;base64,")) + PngB64);
        DataUrlParams->SetStringField(TEXT("format_hint"), TEXT("jpeg"));
        DataUrlParams->SetStringField(TEXT("prompt"), TEXT("data url format override"));
        DataUrlParams->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_ImportDataUrlFormatOverride"));
        DataUrlParams->SetStringField(TEXT("texture_role"), TEXT("basecolor"));
        DataUrlParams->SetBoolField(TEXT("save"), false);
        DataUrlParams->SetBoolField(TEXT("save_source_png"), false);

        const FMonolithActionResult DataUrlResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("imagegen"), TEXT("import_generated_image"), DataUrlParams);
        TestTrue(TEXT("import_generated_image trusts data URL MIME over stale format_hint"), DataUrlResult.bSuccess);
        if (!DataUrlResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Data URL import error: %s (code %d)"),
                *DataUrlResult.ErrorMessage, DataUrlResult.ErrorCode));
        }
        else if (DataUrlResult.Result.IsValid())
        {
            FString FormatHint;
            DataUrlResult.Result->TryGetStringField(TEXT("format_hint"), FormatHint);
            TestEqual(TEXT("data URL format_hint becomes png"), FormatHint, FString(TEXT("png")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithImageGenTextureRolesGenerateLocalTest,
    "MonolithImageGen.TextureRoles.GenerateLocalForwardsRole",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenTextureRolesGenerateLocalTest::RunTest(const FString& Parameters)
{
    EnsureImageGenModuleLoaded();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("prompt"), TEXT("automation test icon with crisp alpha-safe edge"));
    Params->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_GenerateLocalUiRole"));
    Params->SetStringField(TEXT("resolution"), TEXT("64x32"));
    Params->SetStringField(TEXT("texture_role"), TEXT("ui_icon"));
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image"), Params);

    TestTrue(TEXT("generate_image succeeds"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Generate error: %s (code %d)"),
            *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double Width = 0.0;
    double Height = 0.0;
    Result.Result->TryGetNumberField(TEXT("width"), Width);
    Result.Result->TryGetNumberField(TEXT("height"), Height);
    TestEqual(TEXT("explicit resolution width"), static_cast<int32>(Width), 64);
    TestEqual(TEXT("explicit resolution height"), static_cast<int32>(Height), 32);

    FString TextureRole;
    Result.Result->TryGetStringField(TEXT("texture_role"), TextureRole);
    TestEqual(TEXT("generated texture_role"), TextureRole, FString(TEXT("ui_icon")));

    const TSharedPtr<FJsonObject>* Validation = nullptr;
    TestTrue(TEXT("generated validation returned"),
        Result.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());

    const TSharedPtr<FJsonObject>* Provenance = nullptr;
    TestTrue(TEXT("generated provenance returned"),
        Result.Result->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance && Provenance->IsValid());
    if (Provenance && Provenance->IsValid())
    {
        FString ProvenanceRole;
        (*Provenance)->TryGetStringField(TEXT("texture_role"), ProvenanceRole);
        TestEqual(TEXT("provenance texture_role"), ProvenanceRole, FString(TEXT("ui_icon")));
        FString FormatHint;
        (*Provenance)->TryGetStringField(TEXT("format_hint"), FormatHint);
        TestEqual(TEXT("local generated format_hint"), FormatHint, FString(TEXT("png")));
        FString LocalModel;
        (*Provenance)->TryGetStringField(TEXT("model"), LocalModel);
        TestEqual(TEXT("local generated model"), LocalModel, FString(TEXT("monolith/local-gradient-png-v1")));
    }

    FString AssetPath;
    Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
    UTexture2D* Tex = FindTextureAtPackagePath(AssetPath);
    TestNotNull(TEXT("generated UTexture2D exists"), Tex);
    if (Tex)
    {
        TestEqual(TEXT("generated UI compression"), Tex->CompressionSettings, TC_Default);
        TestTrue(TEXT("generated UI sRGB"), Tex->SRGB != 0);
        TestEqual(TEXT("generated UI mip gen"), Tex->MipGenSettings, TMGS_NoMipmaps);
        TestEqual(TEXT("generated UI LOD group"), Tex->LODGroup, TEXTUREGROUP_UI);
        TestEqual(TEXT("generated UI AddressX"), Tex->AddressX, TA_Clamp);
        TestEqual(TEXT("generated UI AddressY"), Tex->AddressY, TA_Clamp);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithImageGenTextureRolesImportExternalTest,
    "MonolithImageGen.TextureRoles.ImportGeneratedImageForwardsRole",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenTextureRolesImportExternalTest::RunTest(const FString& Parameters)
{
    EnsureImageGenModuleLoaded();

    const FString PngB64 = MakeSolidPngB64(FColor(64, 128, 192, 255));
    TestFalse(TEXT("external PNG fixture encoded"), PngB64.IsEmpty());
    if (PngB64.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("bytes_b64"), PngB64);
    Params->SetStringField(TEXT("format_hint"), TEXT("png"));
    Params->SetStringField(TEXT("prompt"), TEXT("external generated orm packed mask"));
    Params->SetStringField(TEXT("provider"), TEXT("external-test"));
    Params->SetStringField(TEXT("model"), TEXT("test-model"));
    Params->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_ImportExternalOrmRole"));
    Params->SetStringField(TEXT("texture_role"), TEXT("orm"));
    Params->SetBoolField(TEXT("save"), false);
    Params->SetBoolField(TEXT("save_source_png"), true);

    const FString RequestedPngPath = ExpectedSourcePngPath(TEXT("/Game/Tests/Monolith/ImageGen/T_ImportExternalOrmRole"));
    IFileManager::Get().Delete(*RequestedPngPath, false, true);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("import_generated_image"), Params);

    TestTrue(TEXT("import_generated_image succeeds"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("Import external error: %s (code %d)"),
            *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    FString TextureRole;
    Result.Result->TryGetStringField(TEXT("texture_role"), TextureRole);
    TestEqual(TEXT("orm synonym normalizes to orm_mask"), TextureRole, FString(TEXT("orm_mask")));

    FString AssetPath;
    Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
    const FString ExpectedPngPath = ExpectedSourcePngPath(AssetPath);

    FString SourcePngPath;
    Result.Result->TryGetStringField(TEXT("source_png_path"), SourcePngPath);
    TestEqual(TEXT("source PNG path mirrors asset path"), SourcePngPath, ExpectedPngPath);
    TestTrue(TEXT("source PNG file exists"), FPaths::FileExists(SourcePngPath));
    TestTrue(TEXT("source PNG has PNG signature"), HasPngSignature(SourcePngPath));

    const TSharedPtr<FJsonObject>* Provenance = nullptr;
    TestTrue(TEXT("external provenance returned"),
        Result.Result->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance && Provenance->IsValid());
    if (Provenance && Provenance->IsValid())
    {
        FString ProvenanceRole;
        (*Provenance)->TryGetStringField(TEXT("texture_role"), ProvenanceRole);
        TestEqual(TEXT("external provenance texture_role"), ProvenanceRole, FString(TEXT("orm_mask")));

        FString ProvenanceSourcePngPath;
        (*Provenance)->TryGetStringField(TEXT("source_png_path"), ProvenanceSourcePngPath);
        TestEqual(TEXT("external provenance source PNG path"), ProvenanceSourcePngPath, ExpectedPngPath);
    }

    UTexture2D* Tex = FindTextureAtPackagePath(AssetPath);
    TestNotNull(TEXT("external UTexture2D exists"), Tex);
    if (Tex)
    {
        TestEqual(TEXT("external ORM compression"), Tex->CompressionSettings, TC_Masks);
        TestFalse(TEXT("external ORM disables sRGB"), Tex->SRGB != 0);
        TestEqual(TEXT("external ORM LOD group"), Tex->LODGroup, TEXTUREGROUP_WorldSpecular);
        TestEqual(TEXT("external ORM AddressX"), Tex->AddressX, TA_Wrap);
        TestEqual(TEXT("external ORM AddressY"), Tex->AddressY, TA_Wrap);
    }

    IFileManager::Get().Delete(*ExpectedPngPath, false, true);

    const FString IconPngB64 = MakeEdgeBackgroundIconPngB64();
    TestFalse(TEXT("external icon PNG fixture encoded"), IconPngB64.IsEmpty());
    if (!IconPngB64.IsEmpty())
    {
        TSharedPtr<FJsonObject> IconParams = MakeShared<FJsonObject>();
        IconParams->SetStringField(TEXT("bytes_b64"), IconPngB64);
        IconParams->SetStringField(TEXT("format_hint"), TEXT("png"));
        IconParams->SetStringField(TEXT("prompt"), TEXT("external generated icon on edge background"));
        IconParams->SetStringField(TEXT("provider"), TEXT("external-test"));
        IconParams->SetStringField(TEXT("model"), TEXT("test-model"));
        IconParams->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_ImportExternalIconPostprocessedMirror"));
        IconParams->SetStringField(TEXT("texture_role"), TEXT("ui_icon"));
        IconParams->SetBoolField(TEXT("save"), false);
        IconParams->SetBoolField(TEXT("save_source_png"), true);

        const FString RequestedIconPngPath = ExpectedSourcePngPath(TEXT("/Game/Tests/Monolith/ImageGen/T_ImportExternalIconPostprocessedMirror"));
        IFileManager::Get().Delete(*RequestedIconPngPath, false, true);

        const FMonolithActionResult IconResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("imagegen"), TEXT("import_generated_image"), IconParams);
        TestTrue(TEXT("postprocessed mirror icon import succeeds"), IconResult.bSuccess);
        if (IconResult.bSuccess && IconResult.Result.IsValid())
        {
            FString IconSourcePngPath;
            TestTrue(TEXT("postprocessed mirror path returned"),
                IconResult.Result->TryGetStringField(TEXT("source_png_path"), IconSourcePngPath) && !IconSourcePngPath.IsEmpty());
            TestTrue(TEXT("postprocessed mirror PNG file exists"), FPaths::FileExists(IconSourcePngPath));
            TestTrue(TEXT("postprocessed mirror has transparent pixel"), PngFileHasTransparentPixel(IconSourcePngPath));

            FString SourcePngKind;
            IconResult.Result->TryGetStringField(TEXT("source_png_kind"), SourcePngKind);
            TestEqual(TEXT("source PNG mirror kind"), SourcePngKind, FString(TEXT("postprocessed")));

            IFileManager::Get().Delete(*IconSourcePngPath, false, true);
        }
    }

    TArray<uint8> FileImportBytes;
    TestTrue(TEXT("external PNG fixture decodes for file_path import"), FBase64::Decode(PngB64, FileImportBytes));
    const FString FixtureDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithImageGenTests")));
    IFileManager::Get().MakeDirectory(*FixtureDir, true);
    const FString FixturePath = FPaths::Combine(FixtureDir, TEXT("import-generated-file-path.png"));
    TestTrue(TEXT("external PNG fixture file saved"), FFileHelper::SaveArrayToFile(FileImportBytes, *FixturePath));
    if (!FileImportBytes.IsEmpty() && FPaths::FileExists(FixturePath))
    {
        TSharedPtr<FJsonObject> FileParams = MakeShared<FJsonObject>();
        FileParams->SetStringField(TEXT("file_path"), FixturePath);
        FileParams->SetStringField(TEXT("format_hint"), TEXT("png"));
        FileParams->SetStringField(TEXT("prompt"), TEXT("external generated height file path"));
        FileParams->SetStringField(TEXT("provider"), TEXT("external-file-test"));
        FileParams->SetStringField(TEXT("model"), TEXT("test-model"));
        FileParams->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_ImportExternalFilePathHeightRole"));
        FileParams->SetStringField(TEXT("texture_role"), TEXT("height"));
        FileParams->SetBoolField(TEXT("save"), false);
        FileParams->SetBoolField(TEXT("save_source_png"), false);

        const FMonolithActionResult FileResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("imagegen"), TEXT("import_generated_image"), FileParams);
        TestTrue(TEXT("import_generated_image accepts file_path"), FileResult.bSuccess);
        if (!FileResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Import file_path error: %s (code %d)"),
                *FileResult.ErrorMessage, FileResult.ErrorCode));
        }
        else if (FileResult.Result.IsValid())
        {
            FString FileRole;
            FileResult.Result->TryGetStringField(TEXT("texture_role"), FileRole);
            TestEqual(TEXT("file_path import texture_role height"), FileRole, FString(TEXT("height")));

            const TSharedPtr<FJsonObject>* FileProvenance = nullptr;
            TestTrue(TEXT("file_path provenance returned"),
                FileResult.Result->TryGetObjectField(TEXT("provenance"), FileProvenance) && FileProvenance && FileProvenance->IsValid());
            if (FileProvenance && FileProvenance->IsValid())
            {
                FString SourceKind;
                (*FileProvenance)->TryGetStringField(TEXT("source"), SourceKind);
                TestEqual(TEXT("file_path provenance source"), SourceKind, FString(TEXT("external_file")));
            }
        }
    }
    IFileManager::Get().Delete(*FixturePath, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithImageGenReferenceInputsArchiveTest,
    "MonolithImageGen.TextureRoles.ReferenceInputsArchive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenReferenceInputsArchiveTest::RunTest(const FString& Parameters)
{
    EnsureImageGenModuleLoaded();

    const FString FixtureDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithImageGenTests")));
    IFileManager::Get().MakeDirectory(*FixtureDir, true);

    const FString PngB64 = MakeSolidPngB64(FColor(12, 34, 56, 255));
    TArray<uint8> PngBytes;
    TestTrue(TEXT("reference PNG fixture decodes"), FBase64::Decode(PngB64, PngBytes));
    const FString ReferencePngPath = FPaths::Combine(FixtureDir, TEXT("reference-png-path.png"));
    TestTrue(TEXT("reference PNG fixture saved"), FFileHelper::SaveArrayToFile(PngBytes, *ReferencePngPath));

    const FString ReferenceAssetPath = TEXT("/Game/Tests/Monolith/ImageGen/T_ReferenceAssetPathSource");
    UTexture2D* ReferenceTexture = MakeReferenceTextureFixture(ReferenceAssetPath);
    TestNotNull(TEXT("reference Texture2D fixture created"), ReferenceTexture);
    if (!ReferenceTexture || !FPaths::FileExists(ReferencePngPath))
    {
        IFileManager::Get().Delete(*ReferencePngPath, false, true);
        return false;
    }

    const TSet<FString> BeforeFiles = ListArchivedReferencePngFiles();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("prompt"), TEXT("reference input archive smoke"));
    Params->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
    Params->SetStringField(TEXT("destination"), TEXT("/Game/Tests/Monolith/ImageGen/T_ReferenceArchiveBridgeFailure"));
    Params->SetNumberField(TEXT("timeout_seconds"), 1.0);
    Params->SetBoolField(TEXT("save"), false);

    TArray<TSharedPtr<FJsonValue>> ReferencePngPaths;
    ReferencePngPaths.Add(MakeShared<FJsonValueString>(ReferencePngPath));
    Params->SetArrayField(TEXT("reference_png_paths"), ReferencePngPaths);

    TArray<TSharedPtr<FJsonValue>> ReferenceAssetPaths;
    ReferenceAssetPaths.Add(MakeShared<FJsonValueString>(ReferenceAssetPath));
    Params->SetArrayField(TEXT("reference_asset_paths"), ReferenceAssetPaths);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("imagegen"), TEXT("generate_image_via_ima2"), Params);
    TestFalse(TEXT("closed local bridge port should fail after references are archived"), Result.bSuccess);

    const TSet<FString> AfterFiles = ListArchivedReferencePngFiles();
    TArray<FString> NewFiles;
    for (const FString& FilePath : AfterFiles)
    {
        if (!BeforeFiles.Contains(FilePath))
        {
            NewFiles.Add(FilePath);
        }
    }

    TestTrue(TEXT("reference_png_paths and reference_asset_paths archive PNG files before bridge call"), NewFiles.Num() >= 2);
    for (const FString& FilePath : NewFiles)
    {
        TestTrue(FString::Printf(TEXT("archived reference has PNG signature: %s"), *FilePath), HasPngSignature(FilePath));
        IFileManager::Get().Delete(*FilePath, false, true);
    }

    IFileManager::Get().Delete(*ReferencePngPath, false, true);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
