// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetDIPropertiesTest, "Monolith.Niagara.ParamGuard.GetDIProperties", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetDIPropertiesTest::RunTest(const FString& Parameters)
{
	FMonolithNiagaraActions Actions;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("di_class"), 123); // Invalid type

		FMonolithActionResult Result = Actions.HandleGetDIProperties(Params);
		TestTrue(TEXT("GetDIProperties should fail with non-string di_class param"), Result.ResultType == FMonolithActionResult::Error);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = Actions.HandleGetDIProperties(Params);
		TestTrue(TEXT("GetDIProperties should fail with missing di_class param"), Result.ResultType == FMonolithActionResult::Error);
	}

	return true;
}
