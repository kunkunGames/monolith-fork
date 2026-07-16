#include "Misc/AutomationTest.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenScatterOnCeilingMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.ScatterOnCeilingRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenScatterOnCeilingMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("scatter_on_ceiling action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("scatter_on_ceiling")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test missing volume_name
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_ceiling"), Params);
	TestFalse(TEXT("scatter_on_ceiling rejects missing volume_name"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_ceiling reports missing volume_name"), Result.ErrorMessage.Contains(TEXT("Missing required param: volume_name")));

	Params->SetStringField(TEXT("volume_name"), TEXT("TestVolumeActor"));

	// Test missing asset_paths
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_ceiling"), Params);
	TestFalse(TEXT("scatter_on_ceiling rejects missing asset_paths"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_ceiling reports missing asset_paths"), Result.ErrorMessage.Contains(TEXT("Missing or empty required param: asset_paths")));

	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/TestPropMesh")));
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Test max count exceeded
	Params->SetNumberField(TEXT("count"), 101.0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_ceiling"), Params);
	TestFalse(TEXT("scatter_on_ceiling rejects count exceeding max limit"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_ceiling reports the validation error"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed (100)")));

	// Test valid params
	Params->SetNumberField(TEXT("count"), 5.0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_ceiling"), Params);
	// In EditorContext tests without full mock setup, it might fail to find the volume,
	// but it shouldn't fail param validation anymore.
	// Since we are mocking volume_name="TestVolumeActor", we can just ensure it doesn't fail with param errors
	TestFalse(TEXT("scatter_on_ceiling valid params doesn't report param errors"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")) || Result.ErrorMessage.Contains(TEXT("Missing required param")));

	return true;
}
