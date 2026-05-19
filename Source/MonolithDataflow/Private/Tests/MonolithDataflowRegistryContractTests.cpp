#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithDataflowActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardDataflowRegistersDedicatedNamespaceTest, "Monolith.ParamGuard.MonolithDataflow.RegistersDedicatedNamespace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardDataflowRegistersDedicatedNamespaceTest::RunTest(const FString& Parameters)
{
	FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("dataflow.get_status action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("get_status")));
	TestTrue(TEXT("dataflow.list_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("list_assets")));

#if WITH_MONOLITH_DATAFLOW
	TestTrue(TEXT("dataflow.get_dataflow_graph action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("get_dataflow_graph")));
	TestTrue(TEXT("dataflow.list_dataflow_node_types action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("list_dataflow_node_types")));
	TestTrue(TEXT("dataflow.get_dataflow_node_schema action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("get_dataflow_node_schema")));
	TestTrue(TEXT("dataflow.validate_dataflow_graph action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("validate_dataflow_graph")));
	TestTrue(TEXT("dataflow.list_dataflow_variables action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("list_dataflow_variables")));
	TestTrue(TEXT("dataflow.list_dataflow_comments action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("dataflow"), TEXT("list_dataflow_comments")));
#endif

	return true;
}
