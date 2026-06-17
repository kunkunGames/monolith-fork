#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardDuplicateModuleTest, "Monolith.ParamGuard.Niagara.DuplicateModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardDuplicateModuleTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for target_emitter (number instead of string) - This is handled by TryGetStringField from another PR/fix potentially, but testing the ones we fix:

    // Test 2: Wrong type for target_usage (boolean instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetBoolField(TEXT("target_usage"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type target_usage boolean"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention target_usage type issue"), Result.ErrorMessage.Contains(TEXT("target_usage")));
    }

    // Test 3: Wrong type for target_stage_name (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetNumberField(TEXT("target_stage_name"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type target_stage_name number"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention target_stage_name type issue"), Result.ErrorMessage.Contains(TEXT("target_stage_name")));
    }

    // Test 4: Wrong type for usage_id (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetNumberField(TEXT("usage_id"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type usage_id number"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention usage_id type issue"), Result.ErrorMessage.Contains(TEXT("usage_id")));
    }

    // Test 5: Wrong type for stage_index (string instead of number)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetStringField(TEXT("stage_index"), TEXT("abc"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type stage_index string"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention stage_index type issue"), Result.ErrorMessage.Contains(TEXT("stage_index")));
    }

    return true;
}
