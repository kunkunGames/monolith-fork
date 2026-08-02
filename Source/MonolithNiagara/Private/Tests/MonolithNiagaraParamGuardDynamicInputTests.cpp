#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardDynamicInputTests, "Monolith.ParamGuard.Niagara.DynamicInputTests", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardDynamicInputTests::RunTest(const FString& Parameters)
{
    // Test HandleAddDynamicInput
    TSharedRef<FJsonObject> AddParams = MakeShared<FJsonObject>();
    AddParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Sys"));
    AddParams->SetNumberField(TEXT("emitter"), 123);

    FMonolithActionResult ResAdd = FMonolithNiagaraActions::HandleAddDynamicInput(AddParams);
    TestFalse(TEXT("HandleAddDynamicInput should reject non-string emitter"), ResAdd.bSuccess);
    TestTrue(TEXT("Error message for emitter"), ResAdd.ErrorMessage.Contains(TEXT("must be a string")));

    AddParams->SetStringField(TEXT("emitter"), TEXT("Em"));
    AddParams->SetNumberField(TEXT("module_node"), 123);
    ResAdd = FMonolithNiagaraActions::HandleAddDynamicInput(AddParams);
    TestFalse(TEXT("HandleAddDynamicInput should reject non-string module_node"), ResAdd.bSuccess);

    AddParams->SetStringField(TEXT("module_node"), TEXT("Mod"));
    AddParams->SetNumberField(TEXT("input"), 123);
    ResAdd = FMonolithNiagaraActions::HandleAddDynamicInput(AddParams);
    TestFalse(TEXT("HandleAddDynamicInput should reject non-string input"), ResAdd.bSuccess);

    AddParams->SetStringField(TEXT("input"), TEXT("In"));
    AddParams->SetNumberField(TEXT("dynamic_input_script"), 123);
    ResAdd = FMonolithNiagaraActions::HandleAddDynamicInput(AddParams);
    TestFalse(TEXT("HandleAddDynamicInput should reject non-string script"), ResAdd.bSuccess);

    // Test HandleRemoveDynamicInput
    TSharedRef<FJsonObject> RmParams = MakeShared<FJsonObject>();
    RmParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Sys"));
    RmParams->SetNumberField(TEXT("emitter"), 123);
    FMonolithActionResult ResRm = FMonolithNiagaraActions::HandleRemoveDynamicInput(RmParams);
    TestFalse(TEXT("HandleRemoveDynamicInput should reject non-string emitter"), ResRm.bSuccess);

    RmParams->SetStringField(TEXT("emitter"), TEXT("Em"));
    RmParams->SetNumberField(TEXT("module_node"), 123);
    ResRm = FMonolithNiagaraActions::HandleRemoveDynamicInput(RmParams);
    TestFalse(TEXT("HandleRemoveDynamicInput should reject non-string module_node"), ResRm.bSuccess);

    RmParams->SetStringField(TEXT("module_node"), TEXT("Mod"));
    RmParams->SetNumberField(TEXT("input"), 123);
    ResRm = FMonolithNiagaraActions::HandleRemoveDynamicInput(RmParams);
    TestFalse(TEXT("HandleRemoveDynamicInput should reject non-string input"), ResRm.bSuccess);

    RmParams->SetStringField(TEXT("input"), TEXT("In"));
    RmParams->SetNumberField(TEXT("dynamic_input_node"), 123);
    ResRm = FMonolithNiagaraActions::HandleRemoveDynamicInput(RmParams);
    TestFalse(TEXT("HandleRemoveDynamicInput should reject non-string dynnode"), ResRm.bSuccess);

    // Test HandleSetDynamicInputValue
    TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Sys"));
    SetParams->SetNumberField(TEXT("emitter"), 123);
    FMonolithActionResult ResSet = FMonolithNiagaraActions::HandleSetDynamicInputValue(SetParams);
    TestFalse(TEXT("HandleSetDynamicInputValue should reject non-string emitter"), ResSet.bSuccess);

    SetParams->SetStringField(TEXT("emitter"), TEXT("Em"));
    SetParams->SetNumberField(TEXT("dynamic_input_node"), 123);
    ResSet = FMonolithNiagaraActions::HandleSetDynamicInputValue(SetParams);
    TestFalse(TEXT("HandleSetDynamicInputValue should reject non-string dynnode"), ResSet.bSuccess);

    SetParams->SetStringField(TEXT("dynamic_input_node"), TEXT("Dyn"));
    SetParams->SetNumberField(TEXT("input"), 123);
    ResSet = FMonolithNiagaraActions::HandleSetDynamicInputValue(SetParams);
    TestFalse(TEXT("HandleSetDynamicInputValue should reject non-string input"), ResSet.bSuccess);

    // Test HandleGetDynamicInputValue
    TSharedRef<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Sys"));
    GetParams->SetNumberField(TEXT("emitter"), 123);
    FMonolithActionResult ResGet = FMonolithNiagaraActions::HandleGetDynamicInputValue(GetParams);
    TestFalse(TEXT("HandleGetDynamicInputValue should reject non-string emitter"), ResGet.bSuccess);

    GetParams->SetStringField(TEXT("emitter"), TEXT("Em"));
    GetParams->SetNumberField(TEXT("dynamic_input_node"), 123);
    ResGet = FMonolithNiagaraActions::HandleGetDynamicInputValue(GetParams);
    TestFalse(TEXT("HandleGetDynamicInputValue should reject non-string dynnode"), ResGet.bSuccess);

    GetParams->SetStringField(TEXT("dynamic_input_node"), TEXT("Dyn"));
    GetParams->SetNumberField(TEXT("input"), 123);
    ResGet = FMonolithNiagaraActions::HandleGetDynamicInputValue(GetParams);
    TestFalse(TEXT("HandleGetDynamicInputValue should reject non-string input"), ResGet.bSuccess);

    // Test HandleGetDynamicInputTree
    TSharedRef<FJsonObject> TreeParams = MakeShared<FJsonObject>();
    TreeParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Sys"));
    TreeParams->SetNumberField(TEXT("emitter"), 123);
    FMonolithActionResult ResTree = FMonolithNiagaraActions::HandleGetDynamicInputTree(TreeParams);
    TestFalse(TEXT("HandleGetDynamicInputTree should reject non-string emitter"), ResTree.bSuccess);

    TreeParams->SetStringField(TEXT("emitter"), TEXT("Em"));
    TreeParams->SetNumberField(TEXT("module_node"), 123);
    ResTree = FMonolithNiagaraActions::HandleGetDynamicInputTree(TreeParams);
    TestFalse(TEXT("HandleGetDynamicInputTree should reject non-string module_node"), ResTree.bSuccess);

    return true;
}
