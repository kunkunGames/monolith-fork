#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardDuplicateEmitterTest, "Monolith.ParamGuard.Niagara.DuplicateEmitter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardDuplicateEmitterTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for new_name (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("new_name"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateEmitter(Params);
        TestFalse(TEXT("DuplicateEmitter should fail gracefully with wrong-type new_name"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention new_name type issue"), Result.ErrorMessage.Contains(TEXT("new_name")));
    }

    // Test 2: Wrong type for new_name (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetBoolField(TEXT("new_name"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateEmitter(Params);
        TestFalse(TEXT("DuplicateEmitter should fail gracefully with wrong-type new_name boolean"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention new_name type issue"), Result.ErrorMessage.Contains(TEXT("new_name")));
    }

    return true;
}
