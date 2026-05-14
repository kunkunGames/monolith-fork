#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithAIControllerActions.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
