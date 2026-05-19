#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithChaosFractureActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardChaosFractureRegistersDedicatedNamespaceTest, "Monolith.ParamGuard.MonolithChaosFracture.RegistersDedicatedNamespace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardChaosFractureRegistersDedicatedNamespaceTest::RunTest(const FString& Parameters)
{
	FMonolithChaosFractureActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("chaos_fracture.get_status action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("chaos_fracture"), TEXT("get_status")));
	TestTrue(TEXT("chaos_fracture.list_geometry_collection_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("chaos_fracture"), TEXT("list_geometry_collection_assets")));
	TestTrue(TEXT("chaos_fracture.list_geometry_collection_components action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("chaos_fracture"), TEXT("list_geometry_collection_components")));

	return true;
}
