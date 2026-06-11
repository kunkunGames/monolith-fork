#include "Misc/AutomationTest.h"
#include "MonolithNiagaraTimingActions.h"
#include "MonolithJsonUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Note: Ensure the function we test is exposed. Since HandleSetSimStageIterationCount internally
// calls DispatchSimStageAlias, testing HandleSetSimStageIterationCount directly will exercise the alias code paths.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTimingParamGuardTest, "Monolith.ParamGuard.Niagara.TimingActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTimingParamGuardTest::RunTest(const FString& Parameters)
{
    // HandleSetSimStageIterationCount internally calls DispatchSimStageAlias. We will test sending a wrong-typed stage_index.

    // Test 1: Wrong type for stage_index (string instead of number)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("iterations"), 5);
        Params->SetStringField(TEXT("stage_index"), TEXT("NotANumber")); // Wrong type

        FMonolithActionResult Result = FMonolithNiagaraTimingActions::HandleSetSimStageIterationCount(Params);
        TestFalse(TEXT("HandleSetSimStageIterationCount should fail with wrong-type stage_index"), Result.bSuccess);
        TestEqual(TEXT("Error code should be ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test 2: Wrong type for stage_name (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("iterations"), 5);
        Params->SetBoolField(TEXT("stage_name"), true); // Wrong type

        FMonolithActionResult Result = FMonolithNiagaraTimingActions::HandleSetSimStageIterationCount(Params);
        TestFalse(TEXT("HandleSetSimStageIterationCount should fail with wrong-type stage_name"), Result.bSuccess);
        TestEqual(TEXT("Error code should be ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    }

    return true;
}
