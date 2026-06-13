#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetEmitterParentTest, "Monolith.ParamGuard.Niagara.GetEmitterParent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetEmitterParentTest::RunTest(const FString& Parameters)
{
    // Test 1: Missing params should fail before dereferencing.
    {
        TSharedPtr<FJsonObject> Params;

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetEmitterParent(Params);
        TestFalse(TEXT("GetEmitterParent should fail gracefully with missing params"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention required asset/emitter params"), Result.ErrorMessage.Contains(TEXT("asset_path")) && Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 2: Wrong type for emitter (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetEmitterParent(Params);
        TestFalse(TEXT("GetEmitterParent should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 3: Wrong type for emitter (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetBoolField(TEXT("emitter"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetEmitterParent(Params);
        TestFalse(TEXT("GetEmitterParent should fail gracefully with wrong-type emitter boolean"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    return true;
}
