#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithNiagaraActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraRequestCompileSchemaTest, "Monolith.Registry.Niagara.RequestCompileHasOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraRequestCompileSchemaTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithNiagaraActions::RegisterActions(Registry);

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("niagara")))
	{
		if (Info.Action == TEXT("request_compile"))
		{
			Schema = Info.ParamSchema;
			break;
		}
	}
	TestNotNull(TEXT("Schema should exist"), Schema.Get());

	if (Schema)
	{
		const TSharedPtr<FJsonObject>* ForceParam = nullptr;
		bool bFoundForce = Schema->TryGetObjectField(TEXT("force"), ForceParam);
		TestTrue(TEXT("force param should exist in schema"), bFoundForce);

		const TSharedPtr<FJsonObject>* SyncParam = nullptr;
		bool bFoundSync = Schema->TryGetObjectField(TEXT("synchronous"), SyncParam);
		TestTrue(TEXT("synchronous param should exist in schema"), bFoundSync);
	}

	return true;
}
