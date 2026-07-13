#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetModuleInputBindingTest, "Monolith.Niagara.ParamGuard.SetModuleInputBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetModuleInputBindingTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingAsset.MissingAsset"));
	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->SetStringField(TEXT("input"), TEXT("SomeInput"));
	Params->SetStringField(TEXT("binding"), TEXT("SomeBinding"));

	// Missing emitter
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetModuleInputBinding(Params);
	TestTrue(TEXT("SetModuleInputBinding should fail if emitter is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputBinding should complain about emitter field"), Result.ErrorMessage, TEXT("Missing required string field: emitter"));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));
	Params->RemoveField(TEXT("module_node"));

	// Missing module_node
	Result = FMonolithNiagaraActions::HandleSetModuleInputBinding(Params);
	TestTrue(TEXT("SetModuleInputBinding should fail if module_node is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputBinding should complain about module_node field"), Result.ErrorMessage, TEXT("Missing required string field: module_node (or module_name/module)"));

	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->RemoveField(TEXT("input"));

	// Missing input
	Result = FMonolithNiagaraActions::HandleSetModuleInputBinding(Params);
	TestTrue(TEXT("SetModuleInputBinding should fail if input is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputBinding should complain about input field"), Result.ErrorMessage, TEXT("Missing required string field: input (or input_name)"));

	Params->SetStringField(TEXT("input"), TEXT("SomeInput"));
	Params->RemoveField(TEXT("binding"));

	// Missing binding
	Result = FMonolithNiagaraActions::HandleSetModuleInputBinding(Params);
	TestTrue(TEXT("SetModuleInputBinding should fail if binding is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputBinding should complain about binding field"), Result.ErrorMessage, TEXT("Missing required string field: binding"));

	return true;
}
