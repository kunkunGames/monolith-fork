#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardRemoveModuleTest, "Monolith.Niagara.ParamGuard.RemoveModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardRemoveModuleTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingAsset.MissingAsset"));

	// Missing emitter
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleRemoveModule(Params);
	TestTrue(TEXT("RemoveModule should fail if emitter is missing"), !Result.bSuccess);
	TestEqual(TEXT("RemoveModule should complain about emitter field"), Result.ErrorMessage, TEXT("Missing required string field: emitter"));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));

	// Missing module_node
	Result = FMonolithNiagaraActions::HandleRemoveModule(Params);
	TestTrue(TEXT("RemoveModule should fail if module_node is missing"), !Result.bSuccess);
	TestEqual(TEXT("RemoveModule should complain about module_node field"), Result.ErrorMessage, TEXT("Missing required string field: module_node"));

	return true;
}
