#include "Misc/AutomationTest.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenScatterOnWallsMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.ScatterOnWallsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenScatterOnWallsMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("scatter_on_walls action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("scatter_on_walls")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test missing volume_name
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_walls"), Params);
	TestFalse(TEXT("scatter_on_walls rejects missing volume_name"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_walls reports schema missing-param validation"), Result.ErrorMessage.Contains(TEXT("Missing required param")));
	TestTrue(TEXT("scatter_on_walls reports missing volume_name"), Result.ErrorMessage.Contains(TEXT("volume_name")));

	Params->SetStringField(TEXT("volume_name"), TEXT("TestVolumeActor"));

	// Test missing asset_paths
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_walls"), Params);
	TestFalse(TEXT("scatter_on_walls rejects missing asset_paths"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_walls reports schema missing-param validation"), Result.ErrorMessage.Contains(TEXT("Missing required param")));
	TestTrue(TEXT("scatter_on_walls reports missing asset_paths"), Result.ErrorMessage.Contains(TEXT("asset_paths")));

	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/TestPropMesh")));
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Test max count exceeded
	Params->SetNumberField(TEXT("count"), 101.0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_walls"), Params);
	TestFalse(TEXT("scatter_on_walls rejects count exceeding max limit"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_walls reports the validation error"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed (100)")));

	// Test valid params
	Params->SetNumberField(TEXT("count"), 5.0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_walls"), Params);
	// In EditorContext tests without full mock setup, it might fail to find the volume,
	// but it shouldn't fail param validation anymore.
	// Since we are mocking volume_name="TestVolumeActor", we can just ensure it doesn't fail with param errors
	TestFalse(TEXT("scatter_on_walls valid params doesn't report param errors"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")) || Result.ErrorMessage.Contains(TEXT("Missing required param")));

	return true;
}
