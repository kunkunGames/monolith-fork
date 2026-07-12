#include "Misc/AutomationTest.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithActionRegistry.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraAddModuleParamGuardTest, "Monolith.Niagara.ParamGuard.AddModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraAddModuleParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingSystem"));

	// Test missing emitter
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("niagara.add_module"), Params);
	TestTrue(TEXT("Missing emitter should fail gracefully"), !Result.bSuccess);
	TestTrue(TEXT("Missing emitter error message"), Result.ErrorMessage.Contains(TEXT("Parameter 'emitter' must be a string")));

	Params->SetStringField(TEXT("emitter"), TEXT("SomeEmitter"));

	// Test missing usage
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("niagara.add_module"), Params);
	TestTrue(TEXT("Missing usage should fail gracefully"), !Result.bSuccess);
	TestTrue(TEXT("Missing usage error message"), Result.ErrorMessage.Contains(TEXT("Parameter 'usage' must be a string")));

	Params->SetStringField(TEXT("usage"), TEXT("ParticleSpawn"));

	// Test missing module_script
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("niagara.add_module"), Params);
	TestTrue(TEXT("Missing module_script should fail gracefully"), !Result.bSuccess);
	TestTrue(TEXT("Missing module_script error message"), Result.ErrorMessage.Contains(TEXT("Parameter 'module_script' must be a string")));

	return true;
}
