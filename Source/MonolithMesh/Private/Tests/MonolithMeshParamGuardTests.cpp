
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "MonolithMeshTechArtActions.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "MonolithLevelInstanceActions.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshOperationActions.h"
#include "MonolithMeshProceduralActions.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshInspectionMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.InspectionRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshInspectionMalformedParamsTest::RunTest(const FString& Parameters)
{
    // Test GetMeshInfo with missing asset_path
    {
        FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry::Get());
        TestTrue(TEXT("get_mesh_info action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_mesh_info")));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // No asset_path
        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_mesh_info"), Params);
        TestFalse(TEXT("GetMeshInfo rejects missing asset_path"), Result.bSuccess);
        TestTrue(TEXT("GetMeshInfo reports the missing asset_path validation error"), Result.ErrorMessage.Contains(TEXT("asset_path is required")));
    }

    return true;
}

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshOperationMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.OperationRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshOperationMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshOperationActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("geometry_smooth action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("geometry_smooth")));

    // Parameter validation should happen before pool lookups for malformed requests.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("handle"), TEXT("mesh_123")); // Fails pool lookup if param parsing succeeds, but parsing should fail first
        Params->SetStringField(TEXT("iterations"), TEXT("many")); // Malformed

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("geometry_smooth"), Params);
        TestFalse(TEXT("GeometrySmooth rejects malformed iterations parameter"), Result.bSuccess);
        TestTrue(TEXT("GeometrySmooth reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'iterations'. Expected number.")));
    }

    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshTechArtMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.TechArtRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshTechArtMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshTechArtActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("import_mesh action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("import_mesh")));

    const FString TempFbxPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithParamGuard"), TEXT(".fbx"));
    TestTrue(TEXT("temporary FBX file is created"), FFileHelper::SaveStringToFile(TEXT("placeholder"), *TempFbxPath));

    TArray<TSharedPtr<FJsonValue>> Files;
    Files.Add(MakeShared<FJsonValueString>(TempFbxPath));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("files"), Files);
    Params->SetStringField(TEXT("destination"), TEXT("/Game/Temp"));
    Params->SetNumberField(TEXT("material_import"), 1);

    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("import_mesh"), Params);
    TestFalse(TEXT("ImportMesh rejects malformed material_import parameter"), Result.bSuccess);
    TestTrue(TEXT("ImportMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'material_import'. Expected string.")));

    IFileManager::Get().Delete(*TempFbxPath);
    return true;
}

#if WITH_GEOMETRYSCRIPT
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
#endif

#if WITH_GEOMETRYSCRIPT
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
#endif

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshStructureMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.CreateStructureRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshStructureMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_structure action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_structure")));

    // Test with malformed wall_thickness (string instead of number)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("room"));
        Params->SetStringField(TEXT("wall_thickness"), TEXT("thick"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_structure"), Params);
        TestFalse(TEXT("CreateStructure rejects malformed wall_thickness parameter"), Result.bSuccess);
        TestTrue(TEXT("CreateStructure reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'wall_thickness'. Expected number.")));
    }

    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLevelInstanceMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.LevelInstanceAliasesRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLevelInstanceMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithLevelInstanceActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("list_child_instances action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("level_instance"), TEXT("list_child_instances")));

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor_name"), TEXT("NonExistentTestActor"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("level_instance"), TEXT("list_child_instances"), Params);
        // It shouldn't fail due to "Unknown parameter: actor_name"
        TestFalse(TEXT("list_child_instances does not reject valid alias 'actor_name' as unknown parameter"), Result.ErrorMessage.Contains(TEXT("Unknown parameter")));
    }

    return true;
}
