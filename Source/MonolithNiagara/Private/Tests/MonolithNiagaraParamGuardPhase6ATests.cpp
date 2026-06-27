#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithActionRegistry.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardPhase6ATests, "Monolith.Niagara.ParamGuard.Phase6A", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardPhase6ATests::RunTest(const FString& Parameters)
{
    // Test get_event_handlers
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetNumberField(TEXT("emitter"), 123);

        FMonolithActionResult Result = FMonolithActionRegistry::Get().ExecuteAction(TEXT("niagara"), TEXT("get_event_handlers"), Params);
        TestFalse(TEXT("Should fail validation"), Result.bSuccess);
        TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test set_event_handler_property
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));
        Params->SetStringField(TEXT("property"), TEXT("MyProp"));
        Params->SetStringField(TEXT("value"), TEXT("Val"));
        Params->SetStringField(TEXT("handler_index"), TEXT("NotANumber"));

        FMonolithActionResult Result = FMonolithActionRegistry::Get().ExecuteAction(TEXT("niagara"), TEXT("set_event_handler_property"), Params);
        TestFalse(TEXT("Should fail validation"), Result.bSuccess);
        TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test get_module_output_parameters
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));
        Params->SetBoolField(TEXT("module_node"), true);

        FMonolithActionResult Result = FMonolithActionRegistry::Get().ExecuteAction(TEXT("niagara"), TEXT("get_module_output_parameters"), Params);
        TestFalse(TEXT("Should fail validation"), Result.bSuccess);
        TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    }

    return true;
}
