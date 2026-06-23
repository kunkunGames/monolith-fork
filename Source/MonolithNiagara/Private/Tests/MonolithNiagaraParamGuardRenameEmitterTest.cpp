#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardRenameEmitterTest, "Monolith.ParamGuard.Niagara.RenameEmitter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardRenameEmitterTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for emitter (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);
        Params->SetStringField(TEXT("name"), TEXT("NewEmitterName"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRenameEmitter(Params);
        TestFalse(TEXT("RenameEmitter should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter param issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test 2: Wrong type for name (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("name"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRenameEmitter(Params);
        TestFalse(TEXT("RenameEmitter should fail gracefully with wrong-type name"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention name param issue"), Result.ErrorMessage.Contains(TEXT("name")));
    }

    return true;
}
