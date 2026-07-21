#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshTerrainActions.h"
#include "MonolithMeshBlockoutActions.h"
#include "MonolithMeshRoofActions.h"
#include "MonolithMeshBuildingActions.h"
#include "MonolithMeshContextPropActions.h"
#include "MonolithMeshFacadeActions.h"
#include "MonolithMeshCityBlockActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenSampleTerrainGridMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.SampleTerrainGridRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenSampleTerrainGridMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("sample_terrain_grid action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("sample_terrain_grid")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Center;
	Center.Add(MakeShared<FJsonValueNumber>(0));
	Center.Add(MakeShared<FJsonValueNumber>(0));
	Center.Add(MakeShared<FJsonValueNumber>(0));
	Params->SetArrayField(TEXT("center"), Center);

	TArray<TSharedPtr<FJsonValue>> Size;
	Size.Add(MakeShared<FJsonValueNumber>(1000));
	Size.Add(MakeShared<FJsonValueNumber>(1000));
	Params->SetArrayField(TEXT("size"), Size);

	// Malformed grid_resolution
	Params->SetStringField(TEXT("grid_resolution"), TEXT("invalid_string"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("sample_terrain_grid"), Params);

	TestFalse(TEXT("sample_terrain_grid rejects malformed grid_resolution parameter"), Result.bSuccess);
	TestTrue(TEXT("sample_terrain_grid reports the validation error"), Result.ErrorMessage.Contains(TEXT("Parameter 'grid_resolution' must be a number")));

	// Malformed trace_height
	Params->RemoveField(TEXT("grid_resolution"));
	Params->SetStringField(TEXT("trace_height"), TEXT("invalid_string"));

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("sample_terrain_grid"), Params);

	TestFalse(TEXT("sample_terrain_grid rejects malformed trace_height parameter"), Result.bSuccess);
	TestTrue(TEXT("sample_terrain_grid reports the validation error"), Result.ErrorMessage.Contains(TEXT("Parameter 'trace_height' must be a number")));

	// Malformed trace_depth
	Params->RemoveField(TEXT("trace_height"));
	Params->SetStringField(TEXT("trace_depth"), TEXT("invalid_string"));

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("sample_terrain_grid"), Params);

	TestFalse(TEXT("sample_terrain_grid rejects malformed trace_depth parameter"), Result.bSuccess);
	TestTrue(TEXT("sample_terrain_grid reports the validation error"), Result.ErrorMessage.Contains(TEXT("Parameter 'trace_depth' must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenTerrainSampleMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.TerrainSampleRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenTerrainSampleMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("analyze_building_site action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("analyze_building_site")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Poly;
	TArray<TSharedPtr<FJsonValue>> P0; P0.Add(MakeShared<FJsonValueNumber>(0)); P0.Add(MakeShared<FJsonValueNumber>(0));
	Poly.Add(MakeShared<FJsonValueArray>(P0));
	TArray<TSharedPtr<FJsonValue>> P1; P1.Add(MakeShared<FJsonValueNumber>(100)); P1.Add(MakeShared<FJsonValueNumber>(0));
	Poly.Add(MakeShared<FJsonValueArray>(P1));
	TArray<TSharedPtr<FJsonValue>> P2; P2.Add(MakeShared<FJsonValueNumber>(0)); P2.Add(MakeShared<FJsonValueNumber>(100));
	Poly.Add(MakeShared<FJsonValueArray>(P2));
	Params->SetArrayField(TEXT("footprint_polygon"), Poly);

	TSharedPtr<FJsonObject> TerrainObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Row;
	Row.Add(MakeShared<FJsonValueNumber>(0.0));
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueArray>(Row));
	TerrainObj->SetArrayField(TEXT("samples"), Samples);

	TerrainObj->SetStringField(TEXT("min_z"), TEXT("zero"));
	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed min_z parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("min_z")));

	TerrainObj->RemoveField(TEXT("min_z"));
	TerrainObj->SetStringField(TEXT("all_hit"), TEXT("true"));
	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed all_hit parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("all_hit")));

	TerrainObj->RemoveField(TEXT("all_hit"));
	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	Params->SetStringField(TEXT("floor_height"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed floor_height parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'floor_height'")));

	Params->RemoveField(TEXT("floor_height"));
	Params->SetStringField(TEXT("hospice_mode"), TEXT("invalid"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed hospice_mode parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'hospice_mode'")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenParamGuardCreateGridFromRoomsTest, "Monolith.ParamGuard.WorldGen.CreateGridFromRoomsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenParamGuardCreateGridFromRoomsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshBuildingActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("create_grid_from_rooms action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_grid_from_rooms")));

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rooms;
	TSharedPtr<FJsonObject> Room = MakeShared<FJsonObject>();
	Room->SetStringField(TEXT("room_id"), TEXT("r1"));
	Room->SetStringField(TEXT("room_type"), TEXT("hall"));
	Room->SetNumberField(TEXT("x"), 0);
	Room->SetNumberField(TEXT("y"), 0);
	Room->SetNumberField(TEXT("width"), 2);
	Room->SetNumberField(TEXT("height"), 2);
	Rooms.Add(MakeShared<FJsonValueObject>(Room));
	JsonObj->SetArrayField(TEXT("rooms"), Rooms);
	JsonObj->SetStringField(TEXT("cell_size"), TEXT("not_a_number"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_grid_from_rooms"), JsonObj);

	TestFalse(TEXT("CreateGridFromRooms rejects malformed cell_size parameter"), Result.bSuccess);
	TestTrue(TEXT("CreateGridFromRooms reports the validation error"), Result.ErrorMessage.Contains(TEXT("cell_size must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenRoofMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.GenerateRoofRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenRoofMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshRoofActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("generate_roof action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("generate_roof")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Poly;
	TArray<TSharedPtr<FJsonValue>> P0;
	P0.Add(MakeShared<FJsonValueNumber>(0.0));
	P0.Add(MakeShared<FJsonValueNumber>(0.0));
	Poly.Add(MakeShared<FJsonValueArray>(P0));

	TArray<TSharedPtr<FJsonValue>> P1;
	P1.Add(MakeShared<FJsonValueNumber>(100.0));
	P1.Add(MakeShared<FJsonValueNumber>(0.0));
	Poly.Add(MakeShared<FJsonValueArray>(P1));

	TArray<TSharedPtr<FJsonValue>> P2;
	P2.Add(MakeShared<FJsonValueNumber>(100.0));
	P2.Add(MakeShared<FJsonValueNumber>(100.0));
	Poly.Add(MakeShared<FJsonValueArray>(P2));

	TArray<TSharedPtr<FJsonValue>> P3;
	P3.Add(MakeShared<FJsonValueNumber>(0.0));
	P3.Add(MakeShared<FJsonValueNumber>(100.0));
	Poly.Add(MakeShared<FJsonValueArray>(P3));
	Params->SetArrayField(TEXT("footprint_polygon"), Poly);
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestRoof"));
	Params->SetStringField(TEXT("pitch_degrees"), TEXT("steep"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
	TestFalse(TEXT("GenerateRoof rejects malformed pitch_degrees parameter"), Result.bSuccess);
	TestTrue(TEXT("GenerateRoof reports the validation error"), Result.ErrorMessage.Contains(TEXT("pitch_degrees must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenBuildingFromGridMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.CreateBuildingFromGridRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenBuildingFromGridMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshBuildingActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("create_building_from_grid action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_building_from_grid")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestBuilding"));

	// Create dummy grid
	TArray<TSharedPtr<FJsonValue>> Grid;
	TArray<TSharedPtr<FJsonValue>> Row;
	Row.Add(MakeShared<FJsonValueNumber>(1));
	Grid.Add(MakeShared<FJsonValueArray>(Row));
	Params->SetArrayField(TEXT("grid"), Grid);

	// Create dummy rooms
	TArray<TSharedPtr<FJsonValue>> Rooms;
	Params->SetArrayField(TEXT("rooms"), Rooms);

	// Create dummy doors
	TArray<TSharedPtr<FJsonValue>> Doors;
	Params->SetArrayField(TEXT("doors"), Doors);

	// Add malformed parameter
	Params->SetStringField(TEXT("cell_size"), TEXT("invalid_string"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_building_from_grid"), Params);
	TestFalse(TEXT("create_building_from_grid rejects malformed cell_size parameter"), Result.bSuccess);
	TestTrue(TEXT("create_building_from_grid reports the validation error"), Result.ErrorMessage.Contains(TEXT("cell_size must be a number")));

	Params->RemoveField(TEXT("cell_size"));
	Params->SetStringField(TEXT("snap_to_floor"), TEXT("invalid_string"));

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_building_from_grid"), Params);
	TestFalse(TEXT("create_building_from_grid rejects malformed snap_to_floor parameter"), Result.bSuccess);
	TestTrue(TEXT("create_building_from_grid reports the late validation error before mutation"), Result.ErrorMessage.Contains(TEXT("snap_to_floor must be a boolean")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenScatterSurfaceTest, "Monolith.ParamGuard.MonolithWorldGen.ScatterOnSurfaceRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenScatterSurfaceTest::RunTest(const FString& Parameters)
{
	FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("scatter_on_surface action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("scatter_on_surface")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test missing surface_actor
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_surface"), Params);
	TestFalse(TEXT("scatter_on_surface rejects missing surface_actor"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_surface reports schema missing-param validation"), Result.ErrorMessage.Contains(TEXT("Missing required param(s):")));
	TestTrue(TEXT("scatter_on_surface reports missing surface_actor"), Result.ErrorMessage.Contains(TEXT("surface_actor")));
	TestTrue(TEXT("scatter_on_surface reports missing asset_paths"), Result.ErrorMessage.Contains(TEXT("asset_paths")));

	Params->SetStringField(TEXT("surface_actor"), TEXT("TestSurfaceActor"));

	// Test missing asset_paths
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_surface"), Params);
	TestFalse(TEXT("scatter_on_surface rejects missing asset_paths"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_surface reports schema missing-param validation"), Result.ErrorMessage.Contains(TEXT("Missing required param(s):")));
	TestTrue(TEXT("scatter_on_surface reports missing asset_paths"), Result.ErrorMessage.Contains(TEXT("asset_paths")));

	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/TestMesh")));
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Test max count exceeded
	Params->SetNumberField(TEXT("count"), 101.0);
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("scatter_on_surface"), Params);
	TestFalse(TEXT("scatter_on_surface rejects count exceeding max limit"), Result.bSuccess);
	TestTrue(TEXT("scatter_on_surface reports the validation error"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed (100)")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenHorrorDamageMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.ApplyHorrorDamageRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenHorrorDamageMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshFacadeActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("apply_horror_damage action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("apply_horror_damage")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target_actor"), TEXT("TestFacadeActor"));

	// Add malformed damage_level
	Params->SetStringField(TEXT("damage_level"), TEXT("extreme"));
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("apply_horror_damage"), Params);
	TestFalse(TEXT("apply_horror_damage rejects malformed damage_level parameter"), Result.bSuccess);
	TestTrue(TEXT("apply_horror_damage reports the validation error for damage_level"), Result.ErrorMessage.Contains(TEXT("damage_level")));

	// Add malformed seed
	Params->RemoveField(TEXT("damage_level"));
	Params->SetStringField(TEXT("seed"), TEXT("random"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("apply_horror_damage"), Params);
	TestFalse(TEXT("apply_horror_damage rejects malformed seed parameter"), Result.bSuccess);
	TestTrue(TEXT("apply_horror_damage reports the validation error for seed"), Result.ErrorMessage.Contains(TEXT("seed")));

	// Add malformed cracks
	Params->RemoveField(TEXT("seed"));
	Params->SetStringField(TEXT("cracks"), TEXT("true"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("apply_horror_damage"), Params);
	TestFalse(TEXT("apply_horror_damage rejects malformed cracks parameter"), Result.bSuccess);
	TestTrue(TEXT("apply_horror_damage reports the validation error for cracks"), Result.ErrorMessage.Contains(TEXT("cracks")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenBlockoutBPMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.CreateBlockoutBlueprintRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenBlockoutBPMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshBlockoutActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("create_blockout_blueprint action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_blockout_blueprint")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("force"), TEXT("invalid_boolean"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_blueprint"), Params);
	TestFalse(TEXT("create_blockout_blueprint rejects malformed force parameter"), Result.bSuccess);
	TestTrue(TEXT("create_blockout_blueprint reports the validation error for force"), Result.ErrorMessage.Contains(TEXT("force must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenTerrainGridResClampTest, "Monolith.WorldGen.ParamGuard.TerrainGridResClamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenTerrainGridResClampTest::RunTest(const FString& Parameters)
{
	auto Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CenterArr; CenterArr.Add(MakeShared<FJsonValueNumber>(0)); CenterArr.Add(MakeShared<FJsonValueNumber>(0)); CenterArr.Add(MakeShared<FJsonValueNumber>(0));
	Payload->SetArrayField(TEXT("center"), CenterArr);
	TArray<TSharedPtr<FJsonValue>> SizeArr; SizeArr.Add(MakeShared<FJsonValueNumber>(1000)); SizeArr.Add(MakeShared<FJsonValueNumber>(1000));
	Payload->SetArrayField(TEXT("size"), SizeArr);
	Payload->SetNumberField(TEXT("grid_resolution"), 129.0);

	FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("sample_terrain_grid"), Payload);

	TestTrue(TEXT("SampleTerrainGrid with grid_resolution > 128 should fail"), !Result.bSuccess);
	TestTrue(TEXT("SampleTerrainGrid failure should mention 'exceeds the maximum allowed (128)'"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed (128)")));

	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenCityBlockMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.CityBlockRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenCityBlockMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshCityBlockActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("create_city_block action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_city_block")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("save_path_prefix"), TEXT("/Game/TestBlock"));

	// Add malformed street_width
	Params->SetStringField(TEXT("street_width"), TEXT("very_wide"));
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_city_block"), Params);
	TestFalse(TEXT("create_city_block rejects malformed street_width parameter"), Result.bSuccess);
	TestTrue(TEXT("create_city_block reports the validation error for street_width"), Result.ErrorMessage.Contains(TEXT("street_width")));

	// Add malformed skip_roofs
	Params->RemoveField(TEXT("street_width"));
	Params->SetStringField(TEXT("skip_roofs"), TEXT("false"));
	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_city_block"), Params);
	TestFalse(TEXT("create_city_block rejects malformed skip_roofs parameter"), Result.bSuccess);
	TestTrue(TEXT("create_city_block reports the validation error for skip_roofs"), Result.ErrorMessage.Contains(TEXT("skip_roofs")));

	return true;
}

#endif // WITH_GEOMETRYSCRIPT
