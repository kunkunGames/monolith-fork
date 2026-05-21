// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

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

    UTexture2D* FindTextureAtPackagePath(const FString& AssetPath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
        return FindObject<UTexture2D>(nullptr, *(AssetPath + TEXT(".") + AssetName));
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

    const TSharedPtr<FJsonObject>* Provenance = nullptr;
    TestTrue(TEXT("external provenance returned"),
        Result.Result->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance && Provenance->IsValid());
    if (Provenance && Provenance->IsValid())
    {
        FString ProvenanceRole;
        (*Provenance)->TryGetStringField(TEXT("texture_role"), ProvenanceRole);
        TestEqual(TEXT("external provenance texture_role"), ProvenanceRole, FString(TEXT("orm_mask")));
    }

    FString AssetPath;
    Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);
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

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
