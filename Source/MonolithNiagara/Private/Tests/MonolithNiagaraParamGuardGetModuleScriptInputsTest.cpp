#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetModuleScriptInputsTest, "Monolith.Niagara.ParamGuard.GetModuleScriptInputs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetModuleScriptInputsTest::RunTest(const FString& Parameters)
{
	FMonolithNiagaraActions Actions;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("script_path"), 123); // Invalid type

		FMonolithActionResult Result = Actions.HandleGetModuleScriptInputs(Params);
		TestFalse(TEXT("GetModuleScriptInputs should fail with non-string script_path param"), Result.bSuccess);
        TestTrue(TEXT("GetModuleScriptInputs error message should mention script_path"), Result.ErrorMessage.Contains(TEXT("script_path")));
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        // missing script_path

		FMonolithActionResult Result = Actions.HandleGetModuleScriptInputs(Params);
		TestFalse(TEXT("GetModuleScriptInputs should fail with missing script_path param"), Result.bSuccess);
	}

	return true;
}
