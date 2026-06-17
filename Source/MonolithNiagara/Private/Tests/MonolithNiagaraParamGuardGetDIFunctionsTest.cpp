// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetDIFunctionsTest, "Monolith.Niagara.ParamGuard.GetDIFunctions", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetDIFunctionsTest::RunTest(const FString& Parameters)
{
	FMonolithNiagaraActions Actions;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("di_class"), 123); // Invalid type

		FMonolithActionResult Result = Actions.HandleGetDIFunctions(Params);
		TestTrue(TEXT("GetDIFunctions should fail with non-string di_class param"), Result.ResultType == FMonolithActionResult::Error);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = Actions.HandleGetDIFunctions(Params);
		TestTrue(TEXT("GetDIFunctions should fail with missing di_class param"), Result.ResultType == FMonolithActionResult::Error);
	}

	return true;
}
