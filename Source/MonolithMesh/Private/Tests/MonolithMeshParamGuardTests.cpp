
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "Dom/JsonObject.h"
#include "MonolithMeshDecalActions.h"
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
