#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardClearModulesTest, "Monolith.ParamGuard.Niagara.ClearModules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardClearModulesTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for emitter (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleClearEmitterModules(Params);
        TestFalse(TEXT("ClearEmitterModules should fail gracefully with wrong-type emitter number"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 2: Wrong type for usage (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetBoolField(TEXT("usage"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleClearEmitterModules(Params);
        TestFalse(TEXT("ClearEmitterModules should fail gracefully with wrong-type usage bool"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention usage type issue"), Result.ErrorMessage.Contains(TEXT("usage")));
    }

    return true;
}
