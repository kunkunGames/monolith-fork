#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardMoveModuleTest, "Monolith.ParamGuard.Niagara.MoveModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardMoveModuleTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/DummySystem"));
		Params->SetNumberField(TEXT("new_index"), 1);
		// Missing 'emitter'
		Params->SetStringField(TEXT("module_node"), TEXT("SomeGuid"));

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleMoveModule(Params);
		TestFalse(TEXT("Missing emitter should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error message mentions emitter"), Result.ErrorMessage.Contains(TEXT("emitter")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/DummySystem"));
		Params->SetNumberField(TEXT("new_index"), 1);
		Params->SetStringField(TEXT("emitter"), TEXT("EmitterA"));
		// Missing 'module_node'

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleMoveModule(Params);
		TestFalse(TEXT("Missing module_node should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("Error message mentions module_node"), Result.ErrorMessage.Contains(TEXT("module_node")));
	}

	return true;
}
