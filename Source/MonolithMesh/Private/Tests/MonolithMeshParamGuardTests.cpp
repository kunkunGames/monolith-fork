
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "Dom/JsonObject.h"

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
