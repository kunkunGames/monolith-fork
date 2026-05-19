#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithDataflowActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardDataflowRegistersDedicatedNamespaceTest, "Monolith.ParamGuard.MonolithDataflow.RegistersDedicatedNamespace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardDataflowRegistersDedicatedNamespaceTest::RunTest(const FString& Parameters)
{
	FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("dataflow.get_status action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("get_status")));
	TestTrue(TEXT("dataflow.list_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("list_assets")));

	return true;
}
