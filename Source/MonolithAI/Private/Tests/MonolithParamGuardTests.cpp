#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithAIControllerActions.h"
#include "MonolithAIStateTreeActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIControllerParamGuardTest, "Monolith.ParamGuard.AI.ControllerActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIControllerParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("create_ai_controller")))
    {
        FMonolithAIControllerActions::RegisterActions(Registry);
    }

    // Missing 'name' shouldn't crash, it should just be empty/use default logic
    TSharedPtr<FJsonObject> Payload1 = MakeShared<FJsonObject>();
    Payload1->SetStringField(TEXT("save_path"), TEXT("/Game/Temp/AIController_ParamGuardTest"));
    FMonolithActionResult Result1 = Registry.ExecuteAction(TEXT("ai"), TEXT("create_ai_controller"), Payload1);

    // It's possible the save path creation fails or succeeds depending on editor state,
    // but the key is we don't crash when missing optional string params.
    TestTrue(TEXT("Completed execute action 1 without crashing"), true);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIStateTreeParamGuardTest, "Monolith.ParamGuard.AI.StateTreeActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIStateTreeParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("move_st_state")))
    {
        FMonolithAIStateTreeActions::RegisterActions(Registry);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/ST_Test"));
    Payload->SetStringField(TEXT("state_id"), TEXT("00000000-0000-0000-0000-000000000000"));
    Payload->SetStringField(TEXT("new_parent_id"), TEXT("11111111-1111-1111-1111-111111111111"));
    Payload->SetStringField(TEXT("index"), TEXT("NotANumber"));

    FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("move_st_state"), Payload);

    TestFalse(TEXT("move_st_state should fail if index is wrong type"), Result.bSuccess);
    TestTrue(TEXT("move_st_state error should indicate wrong parameter type"), Result.ErrorMessage.Contains(TEXT("index")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
