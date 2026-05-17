#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardImportSystemSpecTest, "Monolith.ParamGuard.Niagara.ImportSystemSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardImportSystemSpecTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for mode (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetObjectField(TEXT("spec"), MakeShared<FJsonObject>());
        Params->SetNumberField(TEXT("mode"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);
        TestFalse(TEXT("ImportSystemSpec should fail gracefully with wrong-type mode"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention mode type issue"), Result.ErrorMessage.Contains(TEXT("mode")));
    }

    // Test 2: Wrong type for template in create_system_from_spec (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/NonExistentSystem"));

        TSharedRef<FJsonObject> SpecObj = MakeShared<FJsonObject>();
        SpecObj->SetNumberField(TEXT("template"), 12345);
        Params->SetObjectField(TEXT("spec"), SpecObj);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleCreateSystemFromSpec(Params);
        TestFalse(TEXT("CreateSystemFromSpec should fail gracefully with wrong-type template"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention template type issue"), Result.ErrorMessage.Contains(TEXT("template")));
    }

    return true;
}
