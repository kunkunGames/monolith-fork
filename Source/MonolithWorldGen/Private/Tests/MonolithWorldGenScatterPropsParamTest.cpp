#include "Misc/AutomationTest.h"
#include "MonolithMeshBlockoutActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenScatterPropsParamTest, "Monolith.ParamGuard.WorldGen.ScatterPropsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenScatterPropsParamTest::RunTest(const FString& Parameters)
{
	FMonolithMeshBlockoutActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("scatter_props action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("scatter_props")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test missing volume_name
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects missing volume_name"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports missing volume_name"), Result.ErrorMessage.Contains(TEXT("volume_name")));

	Params->SetStringField(TEXT("volume_name"), TEXT("TestVolume"));

	// Test missing asset_paths
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects missing asset_paths"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports missing asset_paths"), Result.ErrorMessage.Contains(TEXT("asset_paths")));

	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/TestProp")));
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Test missing count
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects missing count"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports invalid count"), Result.ErrorMessage.Contains(TEXT("count (must be > 0)")));

	// Test invalid count
	Params->SetNumberField(TEXT("count"), 0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects invalid count 0"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports invalid count"), Result.ErrorMessage.Contains(TEXT("count (must be > 0)")));

	// Test count exceeding limit
	Params->SetNumberField(TEXT("count"), 300);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects count exceeding limit"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports limit validation"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed (200)")));

	// Test malformed collision_mode
	Params->SetNumberField(TEXT("count"), 5);
	Params->SetStringField(TEXT("collision_mode"), TEXT("invalid_mode"));

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_props"), Params);
	TestFalse(TEXT("scatter_props rejects malformed collision_mode"), Result.bSuccess);
	TestTrue(TEXT("scatter_props reports malformed collision_mode"), Result.ErrorMessage.Contains(TEXT("Invalid collision_mode")));

	return true;
}
