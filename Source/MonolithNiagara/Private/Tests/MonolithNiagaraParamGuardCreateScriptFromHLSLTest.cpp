#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardCreateScriptFromHLSLTest, "Monolith.Niagara.ParamGuard.CreateScriptFromHLSL", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardCreateScriptFromHLSLTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("SomeName"));
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/SomePath"));
	Params->SetStringField(TEXT("hlsl"), TEXT("SomeHlsl"));

	// Missing name
	Params->RemoveField(TEXT("name"));
	FMonolithActionResult Result = FMonolithNiagaraActions::CreateScriptFromHLSL(Params, ENiagaraScriptUsage::Module);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if name is missing"), !Result.bSuccess);
	TestEqual(TEXT("CreateScriptFromHLSL should complain about name field"), Result.ErrorMessage, TEXT("Missing required string field: name"));

	Params->SetStringField(TEXT("name"), TEXT("SomeName"));
	Params->RemoveField(TEXT("save_path"));

	// Missing save_path
	Result = FMonolithNiagaraActions::CreateScriptFromHLSL(Params, ENiagaraScriptUsage::Module);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if save_path is missing"), !Result.bSuccess);
	TestEqual(TEXT("CreateScriptFromHLSL should complain about save_path field"), Result.ErrorMessage, TEXT("Missing required string field: save_path"));

	Params->SetStringField(TEXT("save_path"), TEXT("/Game/SomePath"));
	Params->RemoveField(TEXT("hlsl"));

	// Missing hlsl
	Result = FMonolithNiagaraActions::CreateScriptFromHLSL(Params, ENiagaraScriptUsage::Module);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if hlsl is missing"), !Result.bSuccess);
	TestEqual(TEXT("CreateScriptFromHLSL should complain about hlsl field"), Result.ErrorMessage, TEXT("Missing required string field: hlsl"));

	return true;
}
