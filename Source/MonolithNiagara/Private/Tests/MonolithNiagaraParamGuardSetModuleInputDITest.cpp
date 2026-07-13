#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetModuleInputDITest, "Monolith.Niagara.ParamGuard.SetModuleInputDI", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetModuleInputDITest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingAsset.MissingAsset"));
	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->SetStringField(TEXT("input"), TEXT("SomeInput"));
	Params->SetStringField(TEXT("di_class"), TEXT("SomeDI"));

	// Missing emitter
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetModuleInputDI(Params);
	TestTrue(TEXT("SetModuleInputDI should fail if emitter is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputDI should complain about emitter field"), Result.ErrorMessage, TEXT("Missing required string field: emitter"));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));
	Params->RemoveField(TEXT("module_node"));

	// Missing module_node
	Result = FMonolithNiagaraActions::HandleSetModuleInputDI(Params);
	TestTrue(TEXT("SetModuleInputDI should fail if module_node is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputDI should complain about module_node field"), Result.ErrorMessage, TEXT("Missing required string field: module_node (or module_name/module)"));

	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->RemoveField(TEXT("input"));

	// Missing input
	Result = FMonolithNiagaraActions::HandleSetModuleInputDI(Params);
	TestTrue(TEXT("SetModuleInputDI should fail if input is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputDI should complain about input field"), Result.ErrorMessage, TEXT("Missing required string field: input (or input_name)"));

	Params->SetStringField(TEXT("input"), TEXT("SomeInput"));
	Params->RemoveField(TEXT("di_class"));

	// Missing di_class
	Result = FMonolithNiagaraActions::HandleSetModuleInputDI(Params);
	TestTrue(TEXT("SetModuleInputDI should fail if di_class is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleInputDI should complain about di_class field"), Result.ErrorMessage, TEXT("Missing required string field: di_class"));

	return true;
}
