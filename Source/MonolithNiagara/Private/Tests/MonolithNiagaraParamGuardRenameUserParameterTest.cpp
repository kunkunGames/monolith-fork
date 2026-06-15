#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardRenameUserParameterTest, "Monolith.ParamGuard.Niagara.RenameUserParameter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardRenameUserParameterTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for old_name (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("old_name"), 12345);
        Params->SetStringField(TEXT("new_name"), TEXT("NewName"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRenameUserParameter(Params);
        TestFalse(TEXT("RenameUserParameter should fail gracefully with wrong-type old_name"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention old_name type issue"), Result.ErrorMessage.Contains(TEXT("old_name")));
    }

    // Test 2: Wrong type for new_name (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("old_name"), TEXT("OldName"));
        Params->SetBoolField(TEXT("new_name"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRenameUserParameter(Params);
        TestFalse(TEXT("RenameUserParameter should fail gracefully with wrong-type new_name boolean"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention new_name type issue"), Result.ErrorMessage.Contains(TEXT("new_name")));
    }

    return true;
}
