#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetModuleInputValueTest, "Monolith.ParamGuard.Niagara.SetModuleInputValueTest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetModuleInputValueTest::RunTest(const FString& Parameters)
{
    // Test HandleSetModuleInputValue with malformed vector params (non-numeric 'x')
    TSharedRef<FJsonObject> ParamsX = MakeShared<FJsonObject>();
    ParamsX->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
    ParamsX->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
    ParamsX->SetStringField(TEXT("module"), TEXT("TestModule"));
    ParamsX->SetStringField(TEXT("input"), TEXT("TestInput"));

    TSharedRef<FJsonObject> ValueObj = MakeShared<FJsonObject>();
    ValueObj->SetStringField(TEXT("x"), TEXT("not_a_number"));
    ValueObj->SetNumberField(TEXT("y"), 1.0);

    ParamsX->SetObjectField(TEXT("value"), ValueObj);

    // It should gracefully reject it and NOT crash or trigger an assert
    FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetModuleInputValue(ParamsX);

    // Check it fails properly because the parameters are invalid
    TestFalse(TEXT("HandleSetModuleInputValue should fail gracefully with non-numeric x field"), Result.bSuccess);
    TestTrue(TEXT("Error message should mention numeric requirement"), Result.ErrorMessage.Contains(TEXT("must be numeric")));

    // Test HandleSetModuleInputValue with malformed color params (non-numeric 'r')
    TSharedRef<FJsonObject> ValueObjColor = MakeShared<FJsonObject>();
    ValueObjColor->SetStringField(TEXT("r"), TEXT("not_a_number"));
    ValueObjColor->SetNumberField(TEXT("g"), 1.0);
    ValueObjColor->SetNumberField(TEXT("b"), 1.0);
    ParamsX->SetObjectField(TEXT("value"), ValueObjColor);

    FMonolithActionResult ResultColor = FMonolithNiagaraActions::HandleSetModuleInputValue(ParamsX);
    TestFalse(TEXT("HandleSetModuleInputValue should fail gracefully with non-numeric r field"), ResultColor.bSuccess);
    TestTrue(TEXT("Error message should mention numeric requirement for colors"), ResultColor.ErrorMessage.Contains(TEXT("must be numeric")));

    // Optional vector components must not silently fall back to zero when present.
    TSharedRef<FJsonObject> ValueObjBadZ = MakeShared<FJsonObject>();
    ValueObjBadZ->SetNumberField(TEXT("x"), 1.0);
    ValueObjBadZ->SetNumberField(TEXT("y"), 1.0);
    ValueObjBadZ->SetStringField(TEXT("z"), TEXT("not_a_number"));
    ParamsX->SetObjectField(TEXT("value"), ValueObjBadZ);

    FMonolithActionResult ResultBadZ = FMonolithNiagaraActions::HandleSetModuleInputValue(ParamsX);
    TestFalse(TEXT("HandleSetModuleInputValue should reject non-numeric optional z field"), ResultBadZ.bSuccess);
    TestTrue(TEXT("Error message should mention z numeric requirement"), ResultBadZ.ErrorMessage.Contains(TEXT("z")));

    // Optional color alpha must not silently fall back to one when present.
    TSharedRef<FJsonObject> ValueObjBadAlpha = MakeShared<FJsonObject>();
    ValueObjBadAlpha->SetNumberField(TEXT("r"), 1.0);
    ValueObjBadAlpha->SetNumberField(TEXT("g"), 1.0);
    ValueObjBadAlpha->SetNumberField(TEXT("b"), 1.0);
    ValueObjBadAlpha->SetStringField(TEXT("a"), TEXT("not_a_number"));
    ParamsX->SetObjectField(TEXT("value"), ValueObjBadAlpha);

    FMonolithActionResult ResultBadAlpha = FMonolithNiagaraActions::HandleSetModuleInputValue(ParamsX);
    TestFalse(TEXT("HandleSetModuleInputValue should reject non-numeric optional a field"), ResultBadAlpha.bSuccess);
    TestTrue(TEXT("Error message should mention a numeric requirement"), ResultBadAlpha.ErrorMessage.Contains(TEXT("a")));

    return true;
}
