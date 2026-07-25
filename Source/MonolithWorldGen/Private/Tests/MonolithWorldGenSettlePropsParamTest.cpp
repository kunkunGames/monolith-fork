#include "Misc/AutomationTest.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

BEGIN_DEFINE_SPEC(FMonolithWorldGenSettlePropsParamSpec, "Monolith.ParamGuard.MonolithWorldGen.SettlePropsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FMonolithWorldGenSettlePropsParamSpec)

void FMonolithWorldGenSettlePropsParamSpec::Define()
{
	Describe("SettleProps", [this]()
	{
		It("rejects missing target params", [this]()
		{
			FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
			TestTrue(TEXT("settle_props action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("settle_props")));

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

			// Test missing actor_names and volume_name
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("settle_props"), Params);
			TestFalse(TEXT("settle_props rejects missing target params"), Result.bSuccess);
			TestTrue(TEXT("settle_props reports missing target params error"), Result.ErrorMessage.Contains(TEXT("Provide either actor_names or volume_name")));
		});

		It("accepts valid volume_name without parameter error", [this]()
		{
			FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

			// Setup valid volume_name to test other param validations
			Params->SetStringField(TEXT("volume_name"), TEXT("TestVolumeActor"));

			// Execute with valid volume_name
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("settle_props"), Params);
			// We don't have a real mock volume, so it should fail to find it, but it should not fail on parameter validation
			TestFalse(TEXT("settle_props valid params doesn't report param errors"), Result.ErrorMessage.Contains(TEXT("Provide either actor_names or volume_name")));
		});
	});
}
