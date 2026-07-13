#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetModuleEnabledTest, "Monolith.Niagara.ParamGuard.SetModuleEnabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetModuleEnabledTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingAsset.MissingAsset"));
	Params->SetBoolField(TEXT("enabled"), true);

	// Missing emitter
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetModuleEnabled(Params);
	TestTrue(TEXT("SetModuleEnabled should fail if emitter is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleEnabled should complain about emitter field"), Result.ErrorMessage, TEXT("Missing required string field: emitter"));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));

	// Missing module_node
	Result = FMonolithNiagaraActions::HandleSetModuleEnabled(Params);
	TestTrue(TEXT("SetModuleEnabled should fail if module_node is missing"), !Result.bSuccess);
	TestEqual(TEXT("SetModuleEnabled should complain about module_node field"), Result.ErrorMessage, TEXT("Missing required string field: module_node"));

	return true;
}
