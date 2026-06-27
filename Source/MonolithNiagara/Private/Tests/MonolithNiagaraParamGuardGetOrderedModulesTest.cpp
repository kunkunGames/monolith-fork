#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetOrderedModulesTest, "Monolith.Niagara.ParamGuard.GetOrderedModules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetOrderedModulesTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Cube")); // Dummy
		// Missing emitter

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetOrderedModules(Params);
		TestFalse(TEXT("Missing emitter should fail"), Result.bSuccess);
		TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Cube"));
		Params->SetNumberField(TEXT("emitter"), 123.0); // Wrong type

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetOrderedModules(Params);
		TestFalse(TEXT("Wrong type emitter should fail"), Result.bSuccess);
		TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Cube"));
		Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));
		Params->SetNumberField(TEXT("usage"), 456.0); // Wrong type for usage

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetOrderedModules(Params);
		TestFalse(TEXT("Wrong type usage should fail"), Result.bSuccess);
		TestEqual(TEXT("Should be invalid params error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
