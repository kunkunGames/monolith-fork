#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardListDynamicInputsTest, "Monolith.ParamGuard.Niagara.ListDynamicInputsTest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardListDynamicInputsTest::RunTest(const FString& Parameters)
{
    // Test HandleListDynamicInputs with missing or invalid parameters
    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));

    // Missing 'emitter' parameter
    FMonolithActionResult ResultMissingEmitter = FMonolithNiagaraActions::HandleListDynamicInputs(Params);
    TestFalse(TEXT("HandleListDynamicInputs should reject missing emitter"), ResultMissingEmitter.bSuccess);
    TestTrue(TEXT("Error message should mention emitter type/presence"), ResultMissingEmitter.ErrorMessage.Contains(TEXT("emitter must be a string")));

    // Invalid 'emitter' parameter type
    Params->SetNumberField(TEXT("emitter"), 123);
    FMonolithActionResult ResultInvalidEmitter = FMonolithNiagaraActions::HandleListDynamicInputs(Params);
    TestFalse(TEXT("HandleListDynamicInputs should reject non-string emitter"), ResultInvalidEmitter.bSuccess);
    TestTrue(TEXT("Error message should mention emitter type"), ResultInvalidEmitter.ErrorMessage.Contains(TEXT("emitter must be a string")));

    // Fix 'emitter', missing 'module_node'
    Params->SetStringField(TEXT("emitter"), TEXT("ValidEmitterStr"));
    FMonolithActionResult ResultMissingModuleNode = FMonolithNiagaraActions::HandleListDynamicInputs(Params);
    TestFalse(TEXT("HandleListDynamicInputs should reject missing module_node"), ResultMissingModuleNode.bSuccess);
    TestTrue(TEXT("Error message should mention module_node type/presence"), ResultMissingModuleNode.ErrorMessage.Contains(TEXT("module_node must be a string")));

    // Invalid 'module_node' parameter type
    Params->SetNumberField(TEXT("module_node"), 123);
    FMonolithActionResult ResultInvalidModuleNode = FMonolithNiagaraActions::HandleListDynamicInputs(Params);
    TestFalse(TEXT("HandleListDynamicInputs should reject non-string module_node"), ResultInvalidModuleNode.bSuccess);
    TestTrue(TEXT("Error message should mention module_node type"), ResultInvalidModuleNode.ErrorMessage.Contains(TEXT("module_node must be a string")));

    return true;
}
