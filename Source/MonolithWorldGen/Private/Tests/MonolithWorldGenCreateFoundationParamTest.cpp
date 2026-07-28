#include "Misc/AutomationTest.h"
#include "MonolithMeshTerrainActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenCreateFoundationParamTest, "Monolith.ParamGuard.WorldGen.CreateFoundationRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenCreateFoundationParamTest::RunTest(const FString& Parameters)
{
	FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("create_foundation action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_foundation")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Missing strategy
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects missing strategy"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports missing strategy"), Result.ErrorMessage.Contains(TEXT("strategy")));

	Params->SetStringField(TEXT("strategy"), TEXT("flat"));

	// Missing footprint_polygon
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects missing footprint_polygon"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports missing footprint_polygon"), Result.ErrorMessage.Contains(TEXT("footprint_polygon")));

	// Invalid footprint_polygon (not an array)
	Params->SetStringField(TEXT("footprint_polygon"), TEXT("not_an_array"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid footprint_polygon"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid footprint_polygon"), Result.ErrorMessage.Contains(TEXT("footprint_polygon")));

	// Setup valid footprint
	TArray<TSharedPtr<FJsonValue>> FootprintArr;
	TArray<TSharedPtr<FJsonValue>> Point1; Point1.Add(MakeShared<FJsonValueNumber>(0.0)); Point1.Add(MakeShared<FJsonValueNumber>(0.0));
	TArray<TSharedPtr<FJsonValue>> Point2; Point2.Add(MakeShared<FJsonValueNumber>(100.0)); Point2.Add(MakeShared<FJsonValueNumber>(0.0));
	TArray<TSharedPtr<FJsonValue>> Point3; Point3.Add(MakeShared<FJsonValueNumber>(100.0)); Point3.Add(MakeShared<FJsonValueNumber>(100.0));
	FootprintArr.Add(MakeShared<FJsonValueArray>(Point1));
	FootprintArr.Add(MakeShared<FJsonValueArray>(Point2));
	FootprintArr.Add(MakeShared<FJsonValueArray>(Point3));
	Params->SetArrayField(TEXT("footprint_polygon"), FootprintArr);

	// Missing terrain_samples
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects missing terrain_samples"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports missing terrain_samples"), Result.ErrorMessage.Contains(TEXT("terrain_samples")));

	// Setup terrain_samples (invalid format to fail parsing)
	Params->SetStringField(TEXT("terrain_samples"), TEXT("not_an_object"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid terrain_samples"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid terrain_samples"), Result.ErrorMessage.Contains(TEXT("terrain_samples")));

	// Setup valid terrain_samples object (just enough to pass TryGetObjectField but maybe fail ParseTerrainSample to test its output, or pass it to reach next checks)
	TSharedPtr<FJsonObject> TerrainObj = MakeShared<FJsonObject>();
    // We want to pass ParseTerrainSample to check the next params.
    TArray<TSharedPtr<FJsonValue>> HeightsArray;
    TArray<TSharedPtr<FJsonValue>> Row1; Row1.Add(MakeShared<FJsonValueNumber>(0.0));
    HeightsArray.Add(MakeShared<FJsonValueArray>(Row1));
    TerrainObj->SetArrayField(TEXT("heights"), HeightsArray);
    TArray<TSharedPtr<FJsonValue>> CenterArr; CenterArr.Add(MakeShared<FJsonValueNumber>(0.0)); CenterArr.Add(MakeShared<FJsonValueNumber>(0.0)); CenterArr.Add(MakeShared<FJsonValueNumber>(0.0));
    TerrainObj->SetArrayField(TEXT("center"), CenterArr);
    TArray<TSharedPtr<FJsonValue>> SizeArr; SizeArr.Add(MakeShared<FJsonValueNumber>(100.0)); SizeArr.Add(MakeShared<FJsonValueNumber>(100.0));
    TerrainObj->SetArrayField(TEXT("size"), SizeArr);
    TerrainObj->SetBoolField(TEXT("all_hit"), true);

	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	// Missing save_path
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects missing save_path"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports missing save_path"), Result.ErrorMessage.Contains(TEXT("save_path")));

	Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestMesh"));

	// Invalid floor_height
	Params->SetStringField(TEXT("floor_height"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid floor_height"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid floor_height"), Result.ErrorMessage.Contains(TEXT("floor_height")));
	Params->RemoveField(TEXT("floor_height"));

	// Invalid pier_diameter
	Params->SetStringField(TEXT("pier_diameter"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid pier_diameter"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid pier_diameter"), Result.ErrorMessage.Contains(TEXT("pier_diameter")));
	Params->RemoveField(TEXT("pier_diameter"));

	// Invalid pier_spacing
	Params->SetStringField(TEXT("pier_spacing"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid pier_spacing"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid pier_spacing"), Result.ErrorMessage.Contains(TEXT("pier_spacing")));
	Params->RemoveField(TEXT("pier_spacing"));

	// Invalid pad_z
	Params->SetStringField(TEXT("pad_z"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid pad_z"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid pad_z"), Result.ErrorMessage.Contains(TEXT("pad_z")));
	Params->RemoveField(TEXT("pad_z"));

	// Invalid overwrite
	Params->SetStringField(TEXT("overwrite"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_foundation"), Params);
	TestFalse(TEXT("create_foundation rejects invalid overwrite"), Result.bSuccess);
	TestTrue(TEXT("create_foundation reports invalid overwrite"), Result.ErrorMessage.Contains(TEXT("overwrite")));
	Params->RemoveField(TEXT("overwrite"));

	return true;
}
