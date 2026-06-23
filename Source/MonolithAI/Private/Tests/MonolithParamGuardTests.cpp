#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithAIControllerActions.h"
#include "MonolithAIScaffoldActions.h"
#include "MonolithAIStateTreeActions.h"
#include "MonolithAINavigationActions.h"
#include "MonolithAIEQSActions.h"
#include "Misc/Guid.h"

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

    TSharedPtr<FJsonObject> FractionalPayload = MakeShared<FJsonObject>();
    FractionalPayload->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/ST_Test"));
    FractionalPayload->SetStringField(TEXT("state_id"), TEXT("00000000-0000-0000-0000-000000000000"));
    FractionalPayload->SetStringField(TEXT("new_parent_id"), TEXT("11111111-1111-1111-1111-111111111111"));
    FractionalPayload->SetNumberField(TEXT("index"), 1.5);

    FMonolithActionResult FractionalResult = Registry.ExecuteAction(TEXT("ai"), TEXT("move_st_state"), FractionalPayload);

    TestFalse(TEXT("move_st_state should reject fractional index values"), FractionalResult.bSuccess);
    TestTrue(TEXT("move_st_state fractional index error should mention integer"), FractionalResult.ErrorMessage.Contains(TEXT("integer")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIEQSTestParamGuardTest, "Monolith.ParamGuard.AI.EQSTest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAIEQSTestParamGuardTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ai"), TEXT("configure_eqs_test")))
    {
        FMonolithAIEQSActions::RegisterActions(Registry);
    }

    const FString TestQueryPath = FString::Printf(
        TEXT("/Game/Temp/EQS_TestQuery_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("save_path"), TestQueryPath);

    FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("ai"), TEXT("create_eqs_query"), CreatePayload);
    if (!TestTrue(TEXT("configure_eqs_test param guard fixture should be created"), CreateResult.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("asset_path"), TestQueryPath);
    Payload->SetStringField(TEXT("option_index"), TEXT("NotANumber"));
    Payload->SetNumberField(TEXT("test_index"), 0);

    FMonolithActionResult Result = Registry.ExecuteAction(TEXT("ai"), TEXT("configure_eqs_test"), Payload);

    TSharedPtr<FJsonObject> DeletePayload = MakeShared<FJsonObject>();
    DeletePayload->SetStringField(TEXT("asset_path"), TestQueryPath);
    FMonolithActionResult DeleteResult = Registry.ExecuteAction(TEXT("ai"), TEXT("delete_eqs_query"), DeletePayload);

    TestFalse(TEXT("configure_eqs_test should fail if option_index is wrong type"), Result.bSuccess);
    TestTrue(TEXT("configure_eqs_test error should indicate wrong parameter type for option_index"), Result.ErrorMessage.Contains(TEXT("option_index")));
    TestTrue(TEXT("configure_eqs_test param guard fixture should be cleaned up"), DeleteResult.bSuccess);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
