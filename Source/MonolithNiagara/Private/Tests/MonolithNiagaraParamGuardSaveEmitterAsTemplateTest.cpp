#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSaveEmitterAsTemplateTest, "Monolith.Niagara.ParamGuard.SaveEmitterAsTemplate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSaveEmitterAsTemplateTest::RunTest(const FString& Parameters)
{
    FMonolithNiagaraActions Actions;

    // Test missing emitter
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestSavedEmitter"));

        FMonolithActionResult Result = Actions.HandleSaveEmitterAsTemplate(Params);
        TestTrue(TEXT("save_emitter_as_template should fail gracefully on missing/invalid 'emitter' type"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("Missing or invalid field: emitter")));
    }

    // Test invalid emitter type
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset"), TEXT("/Game/TestSystem"));
        Params->SetNumberField(TEXT("emitter"), 123);
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestSavedEmitter"));

        FMonolithActionResult Result = Actions.HandleSaveEmitterAsTemplate(Params);
        TestTrue(TEXT("save_emitter_as_template should fail gracefully on invalid 'emitter' type"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("Missing or invalid field: emitter")));
    }

    // Test missing save_path
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));

        FMonolithActionResult Result = Actions.HandleSaveEmitterAsTemplate(Params);
        TestTrue(TEXT("save_emitter_as_template should fail gracefully on missing/invalid 'save_path' type"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("Missing or invalid field: save_path")));
    }

    // Test invalid save_path type
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));
        Params->SetNumberField(TEXT("save_path"), 123);

        FMonolithActionResult Result = Actions.HandleSaveEmitterAsTemplate(Params);
        TestTrue(TEXT("save_emitter_as_template should fail gracefully on invalid 'save_path' type"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("Missing or invalid field: save_path")));
    }

    return true;
}
