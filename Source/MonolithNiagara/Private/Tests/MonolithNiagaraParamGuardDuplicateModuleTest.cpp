#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardDuplicateModuleTest, "Monolith.ParamGuard.Niagara.DuplicateModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardDuplicateModuleTest::RunTest(const FString& Parameters)
{
    // Test 0: Valid parameters
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetStringField(TEXT("target_emitter"), TEXT("TargetEmitter"));
        Params->SetNumberField(TEXT("target_index"), 1);
        Params->SetStringField(TEXT("target_usage"), TEXT("ParticleSpawn"));
        Params->SetStringField(TEXT("target_stage_name"), TEXT("MyStage"));
        Params->SetStringField(TEXT("usage_id"), TEXT("GUID-1234"));
        Params->SetNumberField(TEXT("stage_index"), 2);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("Should not fail with param validation error on valid params"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test 1: Wrong types for core fields (target_emitter, source_emitter, source_module_node, target_index)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("source_emitter"), 123);
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type source_emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention source_emitter type issue"), Result.ErrorMessage.Contains(TEXT("source_emitter")));
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("source_module_node"), 123);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type source_module_node"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention source_module_node type issue"), Result.ErrorMessage.Contains(TEXT("source_module_node")));
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetNumberField(TEXT("target_emitter"), 123);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type target_emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention target_emitter type issue"), Result.ErrorMessage.Contains(TEXT("target_emitter")));
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetStringField(TEXT("source_emitter"), TEXT("TestEmitter"));
        Params->SetStringField(TEXT("source_module_node"), TEXT("TestModule"));
        Params->SetStringField(TEXT("target_index"), TEXT("abc"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateModule(Params);
        TestFalse(TEXT("DuplicateModule should fail gracefully with wrong-type target_index"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention target_index type issue"), Result.ErrorMessage.Contains(TEXT("target_index")));
    }


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
