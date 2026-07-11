#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardGetModuleGraphTest, "Monolith.ParamGuard.Niagara.GetModuleGraph", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardGetModuleGraphTest::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetNumberField(TEXT("script_path"), 12345); // wrong type

    FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetModuleGraph(Params);
    TestFalse(TEXT("HandleGetModuleGraph should fail gracefully with wrong-type script_path"), Result.bSuccess);

    return true;
}
