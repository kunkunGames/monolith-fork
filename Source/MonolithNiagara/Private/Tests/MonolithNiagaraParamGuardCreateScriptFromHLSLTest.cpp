#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardCreateScriptFromHLSLTest, "Monolith.Niagara.ParamGuard.CreateScriptFromHLSL", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardCreateScriptFromHLSLTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("niagara"), TEXT("create_module_from_hlsl")))
	{
		FMonolithNiagaraActions::RegisterActions(Registry);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("SomeName"));
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/SomePath"));
	Params->SetStringField(TEXT("hlsl"), TEXT("SomeHlsl"));

	// Missing name
	Params->RemoveField(TEXT("name"));
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("niagara"), TEXT("create_module_from_hlsl"), Params);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if name is missing"), !Result.bSuccess);
	TestTrue(TEXT("CreateScriptFromHLSL should complain about name field"), Result.ErrorMessage.Contains(TEXT("name")));

	Params->SetStringField(TEXT("name"), TEXT("SomeName"));
	Params->RemoveField(TEXT("save_path"));

	// Missing save_path
	Result = Registry.ExecuteAction(TEXT("niagara"), TEXT("create_module_from_hlsl"), Params);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if save_path is missing"), !Result.bSuccess);
	TestTrue(TEXT("CreateScriptFromHLSL should complain about save_path field"), Result.ErrorMessage.Contains(TEXT("save_path")));

	Params->SetStringField(TEXT("save_path"), TEXT("/Game/SomePath"));
	Params->RemoveField(TEXT("hlsl"));

	// Missing hlsl
	Result = Registry.ExecuteAction(TEXT("niagara"), TEXT("create_module_from_hlsl"), Params);
	TestTrue(TEXT("CreateScriptFromHLSL should fail if hlsl is missing"), !Result.bSuccess);
	TestTrue(TEXT("CreateScriptFromHLSL should complain about hlsl field"), Result.ErrorMessage.Contains(TEXT("hlsl")));

	return true;
}
