#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithNDisplayActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardNDisplayRegistersDedicatedNamespaceTest, "Monolith.ParamGuard.MonolithNDisplay.RegistersDedicatedNamespace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardNDisplayRegistersDedicatedNamespaceTest::RunTest(const FString& Parameters)
{
	FMonolithNDisplayActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("ndisplay.get_status action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("ndisplay"), TEXT("get_status")));
	TestTrue(TEXT("ndisplay.list_config_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("ndisplay"), TEXT("list_config_assets")));

	return true;
}
