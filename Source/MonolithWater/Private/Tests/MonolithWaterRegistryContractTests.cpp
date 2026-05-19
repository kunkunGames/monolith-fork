#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithWaterActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWaterRegistersDedicatedNamespaceTest, "Monolith.Registry.MonolithWater.RegistersDedicatedNamespace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWaterRegistersDedicatedNamespaceTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithWaterActions::RegisterActions(Registry);

	TestTrue(TEXT("water.get_status should be registered"), Registry.HasAction(TEXT("water"), TEXT("get_status")));
	TestTrue(TEXT("water.list_bodies should be registered"), Registry.HasAction(TEXT("water"), TEXT("list_bodies")));

	return true;
}
