#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "JsonObjectConverter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardCloneModuleOverridesTest, "Monolith.Niagara.ParamGuard.CloneModuleOverrides", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardCloneModuleOverridesTest::RunTest(const FString& Parameters)
{
	// 1. Missing source_emitter (has field but wrong type)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
		Params->SetNumberField(TEXT("source_emitter"), 42); // Wrong type
		Params->SetStringField(TEXT("source_module"), TEXT("ValidModule"));
		Params->SetStringField(TEXT("target_emitter"), TEXT("ValidTargetEmitter"));
		Params->SetStringField(TEXT("target_module"), TEXT("ValidTargetModule"));

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleCloneModuleOverrides(Params);
		TestFalse(TEXT("Should fail when source_emitter is wrong type"), Result.bSuccess);
		TestTrue(TEXT("Should complain about source_emitter"), Result.ErrorMessage.Contains(TEXT("Parameter 'source_emitter' must be a string")));
	}

	// 2. Missing source_module (missing entirely)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
		Params->SetStringField(TEXT("source_emitter"), TEXT("ValidEmitter"));
		// source_module missing
		Params->SetStringField(TEXT("target_emitter"), TEXT("ValidTargetEmitter"));
		Params->SetStringField(TEXT("target_module"), TEXT("ValidTargetModule"));

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleCloneModuleOverrides(Params);
		TestFalse(TEXT("Should fail when source_module is missing"), Result.bSuccess);
		TestTrue(TEXT("Should complain about source_module"), Result.ErrorMessage.Contains(TEXT("Parameter 'source_module' is required and must be a string")));
	}

	// 3. Missing source_module (empty string)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
		Params->SetStringField(TEXT("source_emitter"), TEXT("ValidEmitter"));
		Params->SetStringField(TEXT("source_module"), TEXT(""));
		Params->SetStringField(TEXT("target_emitter"), TEXT("ValidTargetEmitter"));
		Params->SetStringField(TEXT("target_module"), TEXT("ValidTargetModule"));

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleCloneModuleOverrides(Params);
		TestFalse(TEXT("Should fail when source_module is empty"), Result.bSuccess);
		TestTrue(TEXT("Should complain about source_module"), Result.ErrorMessage.Contains(TEXT("Parameter 'source_module' is required and must be a string")));
	}

	// 4. Missing target_emitter (wrong type)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
		Params->SetStringField(TEXT("source_emitter"), TEXT("ValidEmitter"));
		Params->SetStringField(TEXT("source_module"), TEXT("ValidModule"));
		Params->SetNumberField(TEXT("target_emitter"), 42); // Wrong type
		Params->SetStringField(TEXT("target_module"), TEXT("ValidTargetModule"));

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleCloneModuleOverrides(Params);
		TestFalse(TEXT("Should fail when target_emitter is wrong type"), Result.bSuccess);
		TestTrue(TEXT("Should complain about target_emitter"), Result.ErrorMessage.Contains(TEXT("Parameter 'target_emitter' must be a string")));
	}

	// 5. Missing target_module (missing entirely)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
		Params->SetStringField(TEXT("source_emitter"), TEXT("ValidEmitter"));
		Params->SetStringField(TEXT("source_module"), TEXT("ValidModule"));
		Params->SetStringField(TEXT("target_emitter"), TEXT("ValidTargetEmitter"));
		// target_module missing

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleCloneModuleOverrides(Params);
		TestFalse(TEXT("Should fail when target_module is missing"), Result.bSuccess);
		TestTrue(TEXT("Should complain about target_module"), Result.ErrorMessage.Contains(TEXT("Parameter 'target_module' is required and must be a string")));
	}

	return true;
}
