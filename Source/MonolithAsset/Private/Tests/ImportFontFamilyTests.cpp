// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithAssetFontIngestInternal.h"
#include "MonolithToolRegistry.h"

// Font verification
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Serialization/Archive.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/**
 * MonolithAsset.ImportFontFamily.Basic
 *
 * Dispatches `asset.import_font_family` with a single-face spec sourced from the
 * project's bundled Atkinson Hyperlegible TTF (SIL OFL). Asserts:
 *  - Action succeeds
 *  - family_asset_path / face_asset_paths / faces_imported returned
 *  - UFont composite asset exists in-memory at the reported path
 *  - Composite has DefaultTypeface.Fonts.Num() == 1 with Name == "Regular"
 *  - UFontFace exists at the reported face path
 *
 * Uses save=false -- assets stay in transient in-memory packages.
 *
 * Test-asset convention: /Game/Tests/Monolith/Asset/Fonts/TestFamily/
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportFontFamilyBasicTest,
    "MonolithAsset.ImportFontFamily.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportFontFamilyBasicTest::RunTest(const FString& Parameters)
{
    // Primary fixture: project-bundled Atkinson at Content/UI/Fonts/Atkinson/.
    const FString RelativeProjectContentFixture =
        FPaths::ProjectContentDir() / TEXT("UI/Fonts/Atkinson/AtkinsonHyperlegible-Regular.ttf");
    const FString ProjectContentFixture =
        IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(
            *RelativeProjectContentFixture);

    FString FixturePath;
    if (FPaths::FileExists(ProjectContentFixture))
    {
        FixturePath = ProjectContentFixture;
    }
    else
    {
        AddWarning(FString::Printf(
            TEXT("TTF fixture not present at '%s' -- skipping. Orchestrator to seed fixture."),
            *ProjectContentFixture));
        return true;
    }

    const FString TestSuffix = FGuid::NewGuid().ToString(EGuidFormats::Short);
    const FString Destination =
        FString::Printf(TEXT("/Game/Tests/Monolith/Asset/Fonts/TestFamily_%s"), *TestSuffix);
    const FString FamilyName = FString::Printf(TEXT("TestFamily_%s"), *TestSuffix);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"), Destination);
    Params->SetStringField(TEXT("family_name"), FamilyName);
    {
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> FaceObj = MakeShared<FJsonObject>();
        FaceObj->SetStringField(TEXT("typeface"), TEXT("Regular"));
        FaceObj->SetStringField(TEXT("source_path"), FixturePath);
        Faces.Add(MakeShared<FJsonValueObject>(FaceObj));
        Params->SetArrayField(TEXT("faces"), Faces);
    }
    Params->SetStringField(TEXT("loading_policy"), TEXT("LazyLoad"));
    Params->SetStringField(TEXT("hinting"), TEXT("Default"));
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("asset"), TEXT("import_font_family"), Params);

    TestTrue(TEXT("import_font_family bSuccess"), Result.bSuccess);
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

    FString FamilyAssetPath;
    TestTrue(TEXT("family_asset_path in result"),
        Result.Result->TryGetStringField(TEXT("family_asset_path"), FamilyAssetPath));
    TestEqual(
        TEXT("default import preserves the exact requested family path"),
        FamilyAssetPath,
        Destination / FamilyName);

    const TArray<TSharedPtr<FJsonValue>>* FacePaths = nullptr;
    TestTrue(TEXT("face_asset_paths in result"),
        Result.Result->TryGetArrayField(TEXT("face_asset_paths"), FacePaths));

    double FacesImported = 0.0;
    Result.Result->TryGetNumberField(TEXT("faces_imported"), FacesImported);
    TestEqual(TEXT("faces_imported == 1"), (int32)FacesImported, 1);
    if (!FacePaths || FacePaths->Num() != 1)
    {
        AddError(FString::Printf(TEXT("Expected 1 face path, got %d"),
            FacePaths ? FacePaths->Num() : 0));
        return false;
    }
    const FString FacePath = (*FacePaths)[0]->AsString();
    TestEqual(
        TEXT("default import preserves the exact requested face path"),
        FacePath,
        Destination / TEXT("F_Regular"));

    if (!FacePath.IsEmpty())
    {
        FString FaceLeaf;
        {
            int32 SlashIdx = INDEX_NONE;
            if (FacePath.FindLastChar(TEXT('/'), SlashIdx))
            {
                FaceLeaf = FacePath.Mid(SlashIdx + 1);
            }
        }
        const FString FaceDotted = FacePath + TEXT(".") + FaceLeaf;
        UFontFace* FaceObj = FindObject<UFontFace>(nullptr, *FaceDotted);
        TestNotNull(TEXT("UFontFace present at returned face_asset_paths[0]"), FaceObj);
    }

    if (!FamilyAssetPath.IsEmpty())
    {
        FString FamilyLeaf;
        {
            int32 SlashIdx = INDEX_NONE;
            if (FamilyAssetPath.FindLastChar(TEXT('/'), SlashIdx))
            {
                FamilyLeaf = FamilyAssetPath.Mid(SlashIdx + 1);
            }
        }
        const FString FamilyDotted = FamilyAssetPath + TEXT(".") + FamilyLeaf;
        UFont* FamilyObj = FindObject<UFont>(nullptr, *FamilyDotted);
        TestNotNull(TEXT("UFont present at returned family_asset_path"), FamilyObj);
        if (FamilyObj)
        {
            TestEqual(TEXT("UFont FontCacheType == Runtime"),
                (int32)FamilyObj->FontCacheType, (int32)EFontCacheType::Runtime);

            // UE 5.7: CompositeFont field is UE_DEPRECATED -- read via the accessor.
            const FCompositeFont& Composite = FamilyObj->GetInternalCompositeFont();
            TestEqual(TEXT("DefaultTypeface.Fonts.Num() == 1"),
                Composite.DefaultTypeface.Fonts.Num(), 1);
            if (Composite.DefaultTypeface.Fonts.Num() == 1)
            {
                const FTypefaceEntry& Entry = Composite.DefaultTypeface.Fonts[0];
                TestEqual(TEXT("Typeface entry Name == 'Regular'"),
                    Entry.Name, FName(TEXT("Regular")));
                TestNotNull(TEXT("Typeface entry references a UFontFace asset"),
                    Entry.Font.GetFontFaceAsset());
            }
        }
    }

    return true;
}

/**
 * MonolithAsset.ImportFontFamily.InvalidParams
 *
 * Negative-path coverage -- does NOT require any on-disk TTF.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithAssetImportFontFamilyInvalidParamsTest,
    "MonolithAsset.ImportFontFamily.InvalidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportFontFamilyInvalidParamsTest::RunTest(const FString& Parameters)
{
    using namespace MonolithAsset::FontIngestInternal;

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("empty params -> failure"), R.bSuccess);
        TestEqual(TEXT("empty params -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("missing family_name -> failure"), R.bSuccess);
        TestEqual(TEXT("missing family_name -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("missing faces -> failure"), R.bSuccess);
        TestEqual(TEXT("missing faces -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Empty;
        P->SetArrayField(TEXT("faces"), Empty);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("empty faces -> failure"), R.bSuccess);
        TestEqual(TEXT("empty faces -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        for (int32 Index = 0; Index < MaxFontFacesPerFamily + 1; ++Index)
        {
            TSharedPtr<FJsonObject> Face = MakeShared<FJsonObject>();
            Face->SetStringField(TEXT("typeface"), FString::Printf(TEXT("Face%d"), Index));
            Face->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
            Faces.Add(MakeShared<FJsonValueObject>(Face));
        }
        P->SetArrayField(TEXT("faces"), Faces);

        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("too many faces -> failure"), R.bSuccess);
        TestEqual(TEXT("too many faces -> -32602"), R.ErrorCode, -32602);
        TestTrue(TEXT("too many faces explains the cap"), R.ErrorMessage.Contains(TEXT("at most 64")));
    }

    {
        int64 TotalBytes = 0;
        FString Error;
        TestTrue(
            TEXT("per-face boundary is accepted"),
            AccumulateFontSourceSize(MaxFontFaceSourceBytes, TotalBytes, Error));
        TestEqual(TEXT("accepted boundary contributes exact bytes"), TotalBytes, MaxFontFaceSourceBytes);

        Error.Reset();
        int64 OversizedTotal = 0;
        TestFalse(
            TEXT("per-face boundary plus one is rejected"),
            AccumulateFontSourceSize(MaxFontFaceSourceBytes + 1, OversizedTotal, Error));
        TestEqual(TEXT("rejected source does not change aggregate bytes"), OversizedTotal, int64(0));
        TestTrue(TEXT("per-face rejection reports the limit"), Error.Contains(TEXT("per-face limit")));

        Error.Reset();
        int64 AggregateTotal = MaxFontFamilySourceBytes - MaxFontFaceSourceBytes + 1;
        const int64 AggregateBefore = AggregateTotal;
        TestFalse(
            TEXT("aggregate boundary plus one is rejected"),
            AccumulateFontSourceSize(MaxFontFaceSourceBytes, AggregateTotal, Error));
        TestEqual(TEXT("aggregate rejection preserves the running total"), AggregateTotal, AggregateBefore);
        TestTrue(TEXT("aggregate rejection reports the limit"), Error.Contains(TEXT("aggregate limit")));
    }

    {
        const FString OversizedDirectory = FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("Automation/MonolithAsset"));
        IFileManager::Get().MakeDirectory(*OversizedDirectory, true);
        const FString OversizedPath = OversizedDirectory /
            FString::Printf(TEXT("OversizedFont_%s.ttf"), *FGuid::NewGuid().ToString(EGuidFormats::Short));

        bool bFixtureCreated = false;
        {
            TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*OversizedPath));
            if (Writer)
            {
                Writer->Seek(MaxFontFaceSourceBytes);
                uint8 LastByte = 0;
                Writer->Serialize(&LastByte, 1);
                bFixtureCreated = !Writer->IsError() && Writer->Close();
            }
        }
        TestTrue(TEXT("oversized sparse font fixture created"), bFixtureCreated);

        if (bFixtureCreated)
        {
            const FString PackageSuffix = FGuid::NewGuid().ToString(EGuidFormats::Short);
            const FString Destination =
                FString::Printf(TEXT("/Game/Tests/Monolith/Asset/Fonts/Oversized_%s"), *PackageSuffix);
            const FString FamilyName = FString::Printf(TEXT("Oversized_%s"), *PackageSuffix);

            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("destination"), Destination);
            P->SetStringField(TEXT("family_name"), FamilyName);
            TArray<TSharedPtr<FJsonValue>> Faces;
            TSharedPtr<FJsonObject> Face = MakeShared<FJsonObject>();
            Face->SetStringField(TEXT("typeface"), TEXT("Regular"));
            Face->SetStringField(TEXT("source_path"), OversizedPath);
            Faces.Add(MakeShared<FJsonValueObject>(Face));
            P->SetArrayField(TEXT("faces"), Faces);
            P->SetBoolField(TEXT("save"), false);

            const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
                TEXT("asset"), TEXT("import_font_family"), P);
            TestFalse(TEXT("oversized source -> failure"), R.bSuccess);
            TestEqual(TEXT("oversized source -> -32602"), R.ErrorCode, -32602);
            TestTrue(
                *FString::Printf(
                    TEXT("oversized source explains the cap (actual error: %s)"),
                    *R.ErrorMessage),
                R.ErrorMessage.Contains(TEXT("per-face limit")));
            TestNull(
                TEXT("oversized source creates no family package"),
                FindPackage(nullptr, *(Destination / FamilyName)));
            TestNull(
                TEXT("oversized source creates no face package"),
                FindPackage(nullptr, *(Destination / TEXT("F_Regular"))));
        }

        TestTrue(
            TEXT("oversized sparse font fixture removed"),
            !IFileManager::Get().FileExists(*OversizedPath) ||
                IFileManager::Get().Delete(*OversizedPath, false, true));
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("face missing source_path -> failure"), R.bSuccess);
        TestEqual(TEXT("face missing source_path -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        for (int32 i = 0; i < 2; ++i)
        {
            TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("typeface"), TEXT("Regular"));
            F->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
            Faces.Add(MakeShared<FJsonValueObject>(F));
        }
        P->SetArrayField(TEXT("faces"), Faces);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("duplicate typeface -> failure"), R.bSuccess);
        TestEqual(TEXT("duplicate typeface -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"),
            TEXT("/Game/Tests/Monolith/Asset/Fonts/AllFailTest"));
        P->SetStringField(TEXT("family_name"), TEXT("AllFail"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        F->SetStringField(TEXT("source_path"), TEXT("C:/definitely/does/not/exist/Example.ttf"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        P->SetBoolField(TEXT("save"), false);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("all faces missing -> failure"), R.bSuccess);
        TestEqual(TEXT("all faces missing -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        F->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        P->SetStringField(TEXT("loading_policy"), TEXT("Maybe"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("unknown loading_policy -> failure"), R.bSuccess);
        TestEqual(TEXT("unknown loading_policy -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        F->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        P->SetStringField(TEXT("hinting"), TEXT("Maybe"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("unknown hinting -> failure"), R.bSuccess);
        TestEqual(TEXT("unknown hinting -> -32602"), R.ErrorCode, -32602);
    }

    {
        const FString CollisionSuffix = FGuid::NewGuid().ToString(EGuidFormats::Short);
        const FString Destination =
            FString::Printf(TEXT("/Game/Tests/Monolith/Asset/Fonts/Collision_%s"), *CollisionSuffix);
        const FString FamilyPackageName = Destination / TEXT("Fam");
        UPackage* ExistingPackage = CreatePackage(*FamilyPackageName);
        TestNotNull(TEXT("collision fixture package created"), ExistingPackage);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), Destination);
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        F->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("asset"), TEXT("import_font_family"), P);
        TestFalse(TEXT("default naming rejects an existing exact family path"), R.bSuccess);
        TestEqual(TEXT("default exact-path collision -> -32602"), R.ErrorCode, -32602);
        TestTrue(
            TEXT("default exact-path collision explains explicit unique-name opt-in"),
            R.ErrorMessage.Contains(TEXT("allow_unique_names=true")));

        if (ExistingPackage)
        {
            ExistingPackage->Rename(
                *FString::Printf(
                    TEXT("/Temp/__monolith_font_collision_%s"),
                    *FGuid::NewGuid().ToString(EGuidFormats::Short)),
                nullptr,
                REN_NonTransactional | REN_DoNotDirty);
            ExistingPackage->MarkAsGarbage();
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
