#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshTerrainActions.h"
#include "MonolithMeshRoofActions.h"
#include "MonolithMeshBuildingActions.h"

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

#endif // WITH_GEOMETRYSCRIPT
