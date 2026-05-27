
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "MonolithMeshOperationActions.h"
#include "Dom/JsonObject.h"
#include "MonolithMeshProceduralActions.h"

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
