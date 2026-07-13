#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetStaticSwitchValueTest, "Monolith.Niagara.ParamGuard.SetStaticSwitchValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetStaticSwitchValueTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingAsset.MissingAsset"));
	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->SetStringField(TEXT("input"), TEXT("SomeInput"));
	Params->SetStringField(TEXT("value"), TEXT("SomeValue"));

	// Missing emitter
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetStaticSwitchValue(Params);
	TestTrue(TEXT("SetStaticSwitchValue should fail if emitter is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetStaticSwitchValue should complain about emitter field"), Result.ErrorMessage, TEXT("Missing required string field: emitter"));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));
	Params->RemoveField(TEXT("module_node"));

	// Missing module_node
	Result = FMonolithNiagaraActions::HandleSetStaticSwitchValue(Params);
	TestTrue(TEXT("SetStaticSwitchValue should fail if module_node is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetStaticSwitchValue should complain about module_node field"), Result.ErrorMessage, TEXT("Missing required string field: module_node (or module_name)"));

	Params->SetStringField(TEXT("module_node"), TEXT("SomeModule"));
	Params->RemoveField(TEXT("input"));

	// Missing input
	Result = FMonolithNiagaraActions::HandleSetStaticSwitchValue(Params);
	TestTrue(TEXT("SetStaticSwitchValue should fail if input is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetStaticSwitchValue should complain about input field"), Result.ErrorMessage, TEXT("Missing required string field: input (or input_name)"));

	return true;
}
