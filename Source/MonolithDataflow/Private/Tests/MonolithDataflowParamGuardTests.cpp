#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithDataflowActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// FMonolithDataflowActions::GetDataflowNodeSchema
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDataflowParamGuardGetDataflowNodeSchemaTest, "Monolith.ParamGuard.Dataflow.GetDataflowNodeSchema.RejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowParamGuardGetDataflowNodeSchemaTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("dataflow"), TEXT("get_dataflow_node_schema")))
	{
		FMonolithDataflowActions::RegisterActions(Registry);
	}

#if WITH_MONOLITH_DATAFLOW
	// TryGetStringField returns false if it's not a string (e.g. number). The action expects a string.
	// We want to ensure that it correctly handles type errors by complaining about the missing string value
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("type_name"), 123);

	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("dataflow"), TEXT("get_dataflow_node_schema"), Payload);

	TestFalse(TEXT("get_dataflow_node_schema with malformed type_name should fail"), Result.bSuccess);
	TestTrue(TEXT("Error should be populated for malformed type_name"), Result.ErrorMessage.Contains(TEXT("type_name")));
#endif

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
