#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetEmitterPropertyTest, "Monolith.ParamGuard.Niagara.SetEmitterProperty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetEmitterPropertyTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for emitter (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);
        Params->SetStringField(TEXT("property"), TEXT("SimTarget"));
        Params->SetStringField(TEXT("value"), TEXT("GPU"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetEmitterProperty(Params);
        TestFalse(TEXT("SetEmitterProperty should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 2: Wrong type for property (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("property"), 12345);
        Params->SetStringField(TEXT("value"), TEXT("GPU"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetEmitterProperty(Params);
        TestFalse(TEXT("SetEmitterProperty should fail gracefully with wrong-type property"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention property type issue"), Result.ErrorMessage.Contains(TEXT("property")));
    }

    // Test 3: Fallback property_name wrong type
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("property_name"), 12345);
        Params->SetStringField(TEXT("value"), TEXT("GPU"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetEmitterProperty(Params);
        TestFalse(TEXT("SetEmitterProperty should fail gracefully with wrong-type property_name"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention property_name type issue"), Result.ErrorMessage.Contains(TEXT("property_name")));
    }

    return true;
}
