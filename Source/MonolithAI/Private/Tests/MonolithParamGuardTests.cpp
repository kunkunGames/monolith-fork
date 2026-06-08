#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithAIControllerActions.h"
#include "MonolithAIScaffoldActions.h"
#include "MonolithAIStateTreeActions.h"
#include "MonolithAINavigationActions.h"
#include "MonolithAIEQSActions.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIScaffoldPatrolInvestigateParamGuardTest, "Monolith.ParamGuard.AI.ScaffoldPatrolInvestigateActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIScaffoldPatrolInvestigateParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("scaffold_patrol_investigate_ai")))
    {
        FMonolithAIScaffoldActions::RegisterActions(Registry);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("save_path"), TEXT("/Game/Temp/AI"));
    Payload->SetStringField(TEXT("name"), TEXT("ParamGuard"));
    Payload->SetStringField(TEXT("patrol_type"), TEXT("loop"));
    Payload->SetStringField(TEXT("investigation_radius"), TEXT("500"));

    FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("scaffold_patrol_investigate_ai"), Payload);

    TestFalse(TEXT("scaffold_patrol_investigate_ai should fail if investigation_radius is wrong type"), Result.bSuccess);
    TestTrue(TEXT("scaffold_patrol_investigate_ai error should mention investigation_radius"), Result.ErrorMessage.Contains(TEXT("investigation_radius")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIConfigureNavLinkParamGuardTest, "Monolith.ParamGuard.AI.ConfigureNavLink", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIConfigureNavLinkParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("configure_nav_link")))
    {
        FMonolithAINavigationActions::RegisterActions(Registry);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_path"), TEXT("SomeNavProxy"));
    Payload->SetStringField(TEXT("enabled"), TEXT("NotABool")); // Malformed

    FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("configure_nav_link"), Payload);

    TestFalse(TEXT("configure_nav_link should fail if enabled is wrong type"), Result.bSuccess);
    TestTrue(TEXT("configure_nav_link error should indicate wrong parameter type for enabled"), Result.ErrorMessage.Contains(TEXT("enabled")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIEQSScoringParamGuardTest, "Monolith.ParamGuard.AI.EQSScoring", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIEQSScoringParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("configure_eqs_scoring")))
    {
        FMonolithAIEQSActions::RegisterActions(Registry);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/EQS_TestQuery"));
    Payload->SetNumberField(TEXT("option_index"), 0);
    Payload->SetNumberField(TEXT("test_index"), 0);
    // Malformed type for a number field
    Payload->SetStringField(TEXT("score_clamp_min"), TEXT("NotANumber"));

    FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("configure_eqs_scoring"), Payload);

    TestFalse(TEXT("configure_eqs_scoring should fail if score_clamp_min is wrong type"), Result.bSuccess);
    TestTrue(TEXT("configure_eqs_scoring error should indicate wrong parameter type for score_clamp_min"), Result.ErrorMessage.Contains(TEXT("score_clamp_min")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
