
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "Dom/JsonObject.h"
#include "MonolithMeshInterchangeActions.h"
#include "MonolithMeshDecalActions.h"
#include "MonolithMeshProceduralActions.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshTerrainActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshTerrainSampleMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.TerrainSampleRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshTerrainSampleMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("analyze_building_site action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("analyze_building_site")));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    // Minimal footprint polygon
    TArray<TSharedPtr<FJsonValue>> Poly;
    TArray<TSharedPtr<FJsonValue>> P0; P0.Add(MakeShared<FJsonValueNumber>(0)); P0.Add(MakeShared<FJsonValueNumber>(0));
    Poly.Add(MakeShared<FJsonValueArray>(P0));
    TArray<TSharedPtr<FJsonValue>> P1; P1.Add(MakeShared<FJsonValueNumber>(100)); P1.Add(MakeShared<FJsonValueNumber>(0));
    Poly.Add(MakeShared<FJsonValueArray>(P1));
    TArray<TSharedPtr<FJsonValue>> P2; P2.Add(MakeShared<FJsonValueNumber>(0)); P2.Add(MakeShared<FJsonValueNumber>(100));
    Poly.Add(MakeShared<FJsonValueArray>(P2));
    Params->SetArrayField(TEXT("footprint_polygon"), Poly);

    // Minimal terrain samples
    TSharedPtr<FJsonObject> TerrainObj = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Row;
    Row.Add(MakeShared<FJsonValueNumber>(0.0));
    TArray<TSharedPtr<FJsonValue>> Samples;
    Samples.Add(MakeShared<FJsonValueArray>(Row));
    TerrainObj->SetArrayField(TEXT("samples"), Samples);

    // Add malformed min_z
    TerrainObj->SetStringField(TEXT("min_z"), TEXT("zero"));
    Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("analyze_building_site"), Params);

    TestFalse(TEXT("analyze_building_site rejects malformed min_z parameter"), Result.bSuccess);
    TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("min_z")));

    // Test malformed bool all_hit
    TerrainObj->RemoveField(TEXT("min_z"));
    TerrainObj->SetStringField(TEXT("all_hit"), TEXT("true"));
    Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("analyze_building_site"), Params);

    TestFalse(TEXT("analyze_building_site rejects malformed all_hit parameter"), Result.bSuccess);
    TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("all_hit")));

    return true;
}
#endif // WITH_GEOMETRYSCRIPT

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshInspectionMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.InspectionRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshInspectionMalformedParamsTest::RunTest(const FString& Parameters)
{
    // Test GetMeshInfo with missing asset_path
    {
        FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry::Get());
        TestTrue(TEXT("get_mesh_info action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_mesh_info")));
        TestTrue(TEXT("get_pcg_graph_asset action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_pcg_graph_asset")));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // No asset_path
        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_mesh_info"), Params);
        TestFalse(TEXT("GetMeshInfo rejects missing asset_path"), Result.bSuccess);
        TestTrue(TEXT("GetMeshInfo reports the missing asset_path validation error"), Result.ErrorMessage.Contains(TEXT("asset_path is required")));
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PCGGraph.uasset"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_pcg_graph_asset"), Params);
        TestFalse(TEXT("GetPcgGraphAsset rejects out-of-project paths"), Result.bSuccess);
        TestTrue(TEXT("GetPcgGraphAsset reports the /Game boundary"), Result.ErrorMessage.Contains(TEXT("/Game")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardInterchangeImportMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.InterchangeImportRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardInterchangeImportMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshInterchangeActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("interchange.import_asset action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("import_asset")));
    TestTrue(TEXT("interchange.import_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("import_assets")));
    TestTrue(TEXT("interchange.export_asset action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("export_asset")));

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
        Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
        Params->SetBoolField(TEXT("dry_run"), true);

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
        TestFalse(TEXT("import_asset rejects missing source_file"), Result.bSuccess);
        TestTrue(TEXT("import_asset reports missing source_file"), Result.ErrorMessage.Contains(TEXT("source_file")));
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("source_file"), TEXT("missing.fbx"));
        Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
        Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
        TestTrue(TEXT("import_asset returns structured row for guarded mutation failure"), Result.bSuccess);
        TestTrue(TEXT("import_asset response object is valid"), Result.Result.IsValid());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshDecalMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.DecalActionsRejectMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshDecalMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshDecalActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("place_storytelling_scene action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("place_storytelling_scene")));

    // Test place_storytelling_scene with a malformed location parameter (string instead of array)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("location"), TEXT("not_an_array"));
        Params->SetStringField(TEXT("pattern"), TEXT("violence"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("place_storytelling_scene"), Params);
        TestFalse(TEXT("place_storytelling_scene rejects malformed location parameter"), Result.bSuccess);
        TestTrue(TEXT("place_storytelling_scene reports the validation error"), Result.ErrorMessage.Contains(TEXT("location")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshProceduralMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.ProceduralFinalizeRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshProceduralMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_parametric_mesh action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_parametric_mesh")));

    // Test with malformed boolean (use_cache as string instead of bool)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("cube"));
        Params->SetStringField(TEXT("use_cache"), TEXT("yes"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_parametric_mesh"), Params);
        TestFalse(TEXT("FinalizeProceduralMesh rejects malformed use_cache parameter"), Result.bSuccess);
        TestTrue(TEXT("FinalizeProceduralMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected boolean")));
    }

    // Test with malformed number (overwrite as string instead of bool)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("cube"));
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestCube"));
        Params->SetStringField(TEXT("overwrite"), TEXT("true")); // wrong type

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_parametric_mesh"), Params);
        TestFalse(TEXT("FinalizeProceduralMesh rejects malformed overwrite parameter"), Result.bSuccess);
        TestTrue(TEXT("FinalizeProceduralMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected boolean")));
    }

    return true;
}

#include "MonolithMeshRoofActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshRoofMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.GenerateRoofRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshRoofMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshRoofActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("generate_roof action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("generate_roof")));

    // Test with malformed pitch_degrees (string instead of number)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // Required fields
        TArray<TSharedPtr<FJsonValue>> Poly;
        TArray<TSharedPtr<FJsonValue>> P0;
        P0.Add(MakeShared<FJsonValueNumber>(0.0));
        P0.Add(MakeShared<FJsonValueNumber>(0.0));
        Poly.Add(MakeShared<FJsonValueArray>(P0));

        TArray<TSharedPtr<FJsonValue>> P1;
        P1.Add(MakeShared<FJsonValueNumber>(100.0));
        P1.Add(MakeShared<FJsonValueNumber>(0.0));
        Poly.Add(MakeShared<FJsonValueArray>(P1));

        TArray<TSharedPtr<FJsonValue>> P2;
        P2.Add(MakeShared<FJsonValueNumber>(100.0));
        P2.Add(MakeShared<FJsonValueNumber>(100.0));
        Poly.Add(MakeShared<FJsonValueArray>(P2));

        TArray<TSharedPtr<FJsonValue>> P3;
        P3.Add(MakeShared<FJsonValueNumber>(0.0));
        P3.Add(MakeShared<FJsonValueNumber>(100.0));
        Poly.Add(MakeShared<FJsonValueArray>(P3));
        Params->SetArrayField(TEXT("footprint_polygon"), Poly);
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestRoof"));

        // Malformed optional field
        Params->SetStringField(TEXT("pitch_degrees"), TEXT("steep"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("generate_roof"), Params);
        TestFalse(TEXT("GenerateRoof rejects malformed pitch_degrees parameter"), Result.bSuccess);
        TestTrue(TEXT("GenerateRoof reports the validation error"), Result.ErrorMessage.Contains(TEXT("pitch_degrees must be a number")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshFragmentsMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.CreateFragmentsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshFragmentsMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_fragments action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_fragments")));

    // Test with malformed noise (string instead of number)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("source_handle"), TEXT("mesh_123"));
        Params->SetStringField(TEXT("noise"), TEXT("high"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_fragments"), Params);
        TestFalse(TEXT("CreateFragments rejects malformed noise parameter"), Result.bSuccess);
        TestTrue(TEXT("CreateFragments reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'noise'. Expected number.")));
    }

    return true;
}
