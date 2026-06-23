#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetEmitterPropertyTest, "Monolith.ParamGuard.Niagara.GetEmitterProperty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetEmitterPropertyTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for emitter (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);
        Params->SetStringField(TEXT("property"), TEXT("Visibility"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetEmitterProperty(Params);
        TestFalse(TEXT("GetEmitterProperty should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter param issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 2: Wrong type for property (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("property"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetEmitterProperty(Params);
        TestFalse(TEXT("GetEmitterProperty should fail gracefully with wrong-type property"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention property param issue"), Result.ErrorMessage.Contains(TEXT("property")));
    }

    return true;
}
