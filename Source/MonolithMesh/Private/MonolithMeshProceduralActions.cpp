#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshProceduralActions.h"
#include "MonolithMeshProceduralCache.h"
#include "MonolithMeshHandlePool.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithAssetUtils.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshDecompositionFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Editor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Math/RandomStream.h"

using namespace UE::Geometry;

UMonolithMeshHandlePool* FMonolithMeshProceduralActions::Pool = nullptr;

void FMonolithMeshProceduralActions::SetHandlePool(UMonolithMeshHandlePool* InPool)
{
	Pool = InPool;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_parametric_mesh"),
		TEXT("Generate blockout-quality parametric furniture/props from boolean operations on primitives. "
			"Types: chair, table, desk, shelf, cabinet, bed, door_frame, window_frame, stairs, ramp, pillar, counter, toilet, sink, bathtub"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateParametricMesh),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Furniture type: chair, table, desk, shelf, cabinet, bed, door_frame, window_frame, stairs, ramp, pillar, counter, toilet, sink, bathtub"))
			.Optional(TEXT("dimensions"), TEXT("object"), TEXT("{ width, depth, height } in cm — defaults vary per type"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Type-specific params: seat_height, back_height, leg_thickness, stair_count, stair_depth, shelf_count, bowl_radius, etc."))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle (for further operations)"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh (e.g. /Game/Blockout/SM_Chair_01)"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset at save_path"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn the mesh as an actor in the current level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z] for scene placement"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll] for scene placement"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label when placing in scene"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_horror_prop"),
		TEXT("Generate horror-specific procedural props: barricade, debris_pile, cage, coffin, gurney, broken_wall, vent_grate"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateHorrorProp),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Horror prop type: barricade, debris_pile, cage, coffin, gurney, broken_wall, vent_grate"))
			.Optional(TEXT("dimensions"), TEXT("object"), TEXT("{ width, depth, height } in cm — defaults vary per type"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Type-specific params: board_count, bar_count, noise_scale, hole_radius, slot_count, gap_ratio, etc."))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed for procedural variation"), TEXT("0"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle (for further operations)"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset at save_path"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn the mesh as an actor in the current level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z] for scene placement"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll] for scene placement"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label when placing in scene"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	// ---- Phase 19B: Structures + Mazes ----

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_structure"),
		TEXT("Generate room/corridor/junction structures with walls, floor, ceiling, and door/window openings via boolean subtract. "
			"Types: room, corridor, L_corridor, T_junction, stairwell"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateStructure),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Structure type: room, corridor, L_corridor, T_junction, stairwell"))
			.Optional(TEXT("dimensions"), TEXT("object"), TEXT("{ width, depth, height } in cm"))
			.Optional(TEXT("wall_thickness"), TEXT("number"), TEXT("Wall thickness in cm"), TEXT("20"))
			.Optional(TEXT("floor_thickness"), TEXT("number"), TEXT("Floor/ceiling slab thickness in cm"), TEXT("3"))
			.Optional(TEXT("has_ceiling"), TEXT("boolean"), TEXT("Include ceiling slab"), TEXT("true"))
			.Optional(TEXT("has_floor"), TEXT("boolean"), TEXT("Include floor slab"), TEXT("true"))
			.Optional(TEXT("openings"), TEXT("array"), TEXT("Array of opening specs: { wall: north|south|east|west, type: door|window|vent, width, height, offset_x, offset_z }"))
			.Optional(TEXT("add_trim"), TEXT("boolean"), TEXT("Add doorframe/window/vent trim geometry around openings"), TEXT("true"))
			.Optional(TEXT("wall_mode"), TEXT("string"), TEXT("Wall construction: sweep (thin walls) or box (legacy cubes)"), TEXT("sweep"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn actor in level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_building_shell"),
		TEXT("Generate a multi-story building shell from a 2D footprint polygon. Extrudes walls per floor, adds floor/ceiling slabs."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateBuildingShell),
		FParamSchemaBuilder()
			.Required(TEXT("footprint"), TEXT("array"), TEXT("2D polygon as array of [x, y] points (CCW winding, cm)"))
			.Optional(TEXT("floors"), TEXT("integer"), TEXT("Number of floors"), TEXT("1"))
			.Optional(TEXT("floor_height"), TEXT("number"), TEXT("Height per floor in cm"), TEXT("300"))
			.Optional(TEXT("wall_thickness"), TEXT("number"), TEXT("Wall thickness in cm"), TEXT("25"))
			.Optional(TEXT("floor_thickness"), TEXT("number"), TEXT("Floor slab thickness in cm"), TEXT("15"))
			.Optional(TEXT("stairwell_cutout"), TEXT("object"), TEXT("Optional stairwell cutout: { x, y, width, depth } in cm"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn actor in level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_maze"),
		TEXT("Generate a grid-based maze with 3 algorithms: recursive_backtracker, prims, binary_tree. "
			"Returns maze layout JSON for AI pathfinding. Seed for reproducibility."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateMaze),
		FParamSchemaBuilder()
			.Optional(TEXT("algorithm"), TEXT("string"), TEXT("Maze algorithm: recursive_backtracker, prims, binary_tree"), TEXT("recursive_backtracker"))
			.Optional(TEXT("grid_size"), TEXT("array"), TEXT("Grid dimensions [columns, rows]"), TEXT("[8, 8]"))
			.Optional(TEXT("cell_size"), TEXT("number"), TEXT("Cell size in cm"), TEXT("200"))
			.Optional(TEXT("wall_height"), TEXT("number"), TEXT("Wall height in cm"), TEXT("250"))
			.Optional(TEXT("wall_thickness"), TEXT("number"), TEXT("Wall thickness in cm"), TEXT("20"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed for reproducibility"), TEXT("0"))
			.Optional(TEXT("merge_walls"), TEXT("boolean"), TEXT("Apply SelfUnion to merge overlapping wall geometry (slower but cleaner)"), TEXT("false"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn actor in level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	// ---- Phase 19C: Pipes + Fragments ----

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_pipe_network"),
		TEXT("Sweep a circular cross-section along 3D path points to create pipes/ducts. "
			"Configurable radius, segments, elbow handling via MiterLimit. Supports ball joints at junctions."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreatePipeNetwork),
		FParamSchemaBuilder()
			.Required(TEXT("path_points"), TEXT("array"), TEXT("Array of [x, y, z] path points (minimum 2)"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Pipe radius in cm"), TEXT("10"))
			.Optional(TEXT("segments"), TEXT("integer"), TEXT("Cross-section polygon segments"), TEXT("12"))
			.Optional(TEXT("miter_limit"), TEXT("number"), TEXT("Miter limit for sharp elbows (higher = sharper allowed corners)"), TEXT("2.0"))
			.Optional(TEXT("ball_joints"), TEXT("boolean"), TEXT("Add sphere joints at each path point"), TEXT("false"))
			.Optional(TEXT("joint_radius_scale"), TEXT("number"), TEXT("Joint sphere radius as multiplier of pipe radius"), TEXT("1.3"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn actor in level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_fragments"),
		TEXT("Fragment a mesh via iterative plane slicing. Each cut adds a random plane through the mesh interior. "
			"Produces N fragments as separate handles. Seed for reproducibility. Uses ApplyMeshPlaneSlice + SplitMeshByComponents."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateFragments),
		FParamSchemaBuilder()
			.Required(TEXT("source_handle"), TEXT("string"), TEXT("Mesh handle to fragment (original is not modified)"))
			.Optional(TEXT("fragment_count"), TEXT("integer"), TEXT("Target number of fragments (actual may differ slightly)"), TEXT("8"))
			.Optional(TEXT("noise"), TEXT("number"), TEXT("Perlin noise displacement on cut surfaces (0 = clean cuts)"), TEXT("0"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed for reproducibility"), TEXT("0"))
			.Optional(TEXT("gap_width"), TEXT("number"), TEXT("Gap width between sliced halves"), TEXT("0.5"))
			.Optional(TEXT("handle_prefix"), TEXT("string"), TEXT("Prefix for fragment handles (e.g. 'frag' -> frag_0, frag_1, ...)"), TEXT("frag"))
			.Build());

	// ---- Phase 19D: Terrain ----

	Registry.RegisterAction(TEXT("mesh"), TEXT("create_terrain_patch"),
		TEXT("Generate a terrain patch as a subdivided grid with Perlin noise heightmap displacement. "
			"Multi-octave noise via repeated ApplyPerlinNoiseToMesh2 passes."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::CreateTerrainPatch),
		FParamSchemaBuilder()
			.Optional(TEXT("size"), TEXT("array"), TEXT("Terrain size [x, y] in cm"), TEXT("[2000, 2000]"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Grid subdivisions per axis"), TEXT("32"))
			.Optional(TEXT("amplitude"), TEXT("number"), TEXT("Base noise amplitude (height displacement) in cm"), TEXT("100"))
			.Optional(TEXT("frequency"), TEXT("number"), TEXT("Base noise frequency"), TEXT("0.01"))
			.Optional(TEXT("octaves"), TEXT("integer"), TEXT("Number of noise octaves (each halves amplitude, doubles frequency)"), TEXT("4"))
			.Optional(TEXT("persistence"), TEXT("number"), TEXT("Amplitude multiplier per octave"), TEXT("0.5"))
			.Optional(TEXT("lacunarity"), TEXT("number"), TEXT("Frequency multiplier per octave"), TEXT("2.0"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed"), TEXT("0"))
			.Optional(TEXT("handle"), TEXT("string"), TEXT("Save result to a mesh handle"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("Asset path to save as StaticMesh"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("place_in_scene"), TEXT("boolean"), TEXT("Spawn actor in level"), TEXT("false"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation [pitch, yaw, roll]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("use_cache"), TEXT("boolean"), TEXT("Check cache before generating"), TEXT("true"))
			.Optional(TEXT("auto_save"), TEXT("boolean"), TEXT("Auto-save to /Game/Generated/ even without explicit save_path"), TEXT("true"))
			.Build());

	// ---- Cache management actions ----
	RegisterCacheActions(Registry);
}

// ============================================================================
// Shared helpers
// ============================================================================

static const FString GS_ERROR = TEXT("Enable the GeometryScripting plugin in your .uproject to use procedural geometry.");

bool FMonolithMeshProceduralActions::ParseDimensions(const TSharedPtr<FJsonObject>& Params,
	float& Width, float& Depth, float& Height,
	float DefaultWidth, float DefaultDepth, float DefaultHeight, FString& OutError)
{
	OutError.Empty();
	Width = DefaultWidth;
	Depth = DefaultDepth;
	Height = DefaultHeight;

	const TSharedPtr<FJsonObject>* DimObj = nullptr;
	if (Params->TryGetObjectField(TEXT("dimensions"), DimObj) && DimObj && (*DimObj).IsValid())
	{
		if ((*DimObj)->HasField(TEXT("width")))
		{
			double TempVal;
			if (!(*DimObj)->TryGetNumberField(TEXT("width"), TempVal))
			{
				OutError = TEXT("Invalid type for parameter 'width'. Expected number.");
				return false;
			}
			Width = static_cast<float>(TempVal);
		}
		if ((*DimObj)->HasField(TEXT("depth")))
		{
			double TempVal;
			if (!(*DimObj)->TryGetNumberField(TEXT("depth"), TempVal))
			{
				OutError = TEXT("Invalid type for parameter 'depth'. Expected number.");
				return false;
			}
			Depth = static_cast<float>(TempVal);
		}
		if ((*DimObj)->HasField(TEXT("height")))
		{
			double TempVal;
			if (!(*DimObj)->TryGetNumberField(TEXT("height"), TempVal))
			{
				OutError = TEXT("Invalid type for parameter 'height'. Expected number.");
				return false;
			}
			Height = static_cast<float>(TempVal);
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FMonolithMeshProceduralActions::ParseSubParams(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* SubObj = nullptr;
	if (Params->TryGetObjectField(TEXT("params"), SubObj) && SubObj && (*SubObj).IsValid())
	{
		return *SubObj;
	}
	return MakeShared<FJsonObject>();
}

void FMonolithMeshProceduralActions::CleanupMesh(UDynamicMesh* Mesh, bool bHadBooleans)
{
	if (!Mesh) return;

	if (bHadBooleans)
	{
		// After booleans: recompute normals with split by angle
		FGeometryScriptSplitNormalsOptions SplitOpts;
		SplitOpts.bSplitByOpeningAngle = true;
		SplitOpts.OpeningAngleDeg = 15.0f;
		FGeometryScriptCalculateNormalsOptions CalcOpts;
		UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(Mesh, SplitOpts, CalcOpts);
	}
	else
	{
		// Additive-only: self-union to merge overlapping geometry + clean normals
		FGeometryScriptMeshSelfUnionOptions SelfUnionOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion(Mesh, SelfUnionOpts);

		FGeometryScriptSplitNormalsOptions SplitOpts;
		SplitOpts.bSplitByOpeningAngle = true;
		SplitOpts.OpeningAngleDeg = 15.0f;
		FGeometryScriptCalculateNormalsOptions CalcOpts;
		UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(Mesh, SplitOpts, CalcOpts);
	}
}

bool FMonolithMeshProceduralActions::SaveMeshToAsset(UDynamicMesh* Mesh, const FString& SavePath, bool bOverwrite, FString& OutError)
{
	if (!Pool)
	{
		OutError = GS_ERROR;
		return false;
	}

	// Create a temporary handle, save it, then release
	FString TempHandle = FString::Printf(TEXT("__proc_save_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	FString CreateError;

	// We can't use Pool->CreateHandle for an already-built mesh, so we use the save pipeline directly.
	// Reuse the handle pool's SaveHandle which converts DynamicMesh -> MeshDescription -> StaticMesh.
	// But we need a handle entry. Create internal handle, copy mesh data in, save, release.
	if (!Pool->CreateHandle(TempHandle, TEXT("internal:procedural_save"), CreateError))
	{
		OutError = CreateError;
		return false;
	}

	UDynamicMesh* TempMesh = Pool->GetHandle(TempHandle, CreateError);
	if (!TempMesh)
	{
		Pool->ReleaseHandle(TempHandle);
		OutError = TEXT("Failed to get temporary handle mesh");
		return false;
	}

	// Copy geometry into the temp handle
	TempMesh->SetMesh(Mesh->GetMeshRef());

	// Save via pool
	if (!Pool->SaveHandle(TempHandle, SavePath, bOverwrite, OutError))
	{
		Pool->ReleaseHandle(TempHandle);
		return false;
	}

	Pool->ReleaseHandle(TempHandle);
	return true;
}

AActor* FMonolithMeshProceduralActions::PlaceMeshInScene(const FString& AssetPath, const FVector& Location, const FRotator& Rotation, const FString& Label, bool bSnapToFloor /*= true*/, const FString& Folder /*= TEXT("")*/)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World) return nullptr;

	UStaticMesh* SM = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(AssetPath);
	if (!SM) return nullptr;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
	if (!Actor) return nullptr;

	Actor->GetStaticMeshComponent()->SetStaticMesh(SM);

	// Auto snap-to-floor: trace downward to find the actual floor surface.
	// Proc gen meshes have pivot at bottom-center (min Z = 0), so NewZ = Hit.ImpactPoint.Z.
	// Only snaps DOWN — if location is already below the actual floor, it stays put.
	if (bSnapToFloor)
	{
		FHitResult FloorHit;
		FVector TraceStart = Location + FVector(0, 0, 50.0);  // Start slightly above to catch floor at same Z
		FVector TraceEnd = Location - FVector(0, 0, 500.0);    // Trace 5m down
		FCollisionQueryParams FloorParams(SCENE_QUERY_STAT(ProcMeshFloorSnap), true);
		FloorParams.AddIgnoredActor(Actor);

		if (World->LineTraceSingleByChannel(FloorHit, TraceStart, TraceEnd, ECC_WorldStatic, FloorParams))
		{
			FVector NewLoc = Actor->GetActorLocation();
			NewLoc.Z = FloorHit.ImpactPoint.Z;
			Actor->SetActorLocation(NewLoc);
		}
	}

	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}

	// Set outliner folder — use explicit folder, or derive from label/asset name
	if (!Folder.IsEmpty())
	{
		Actor->SetFolderPath(FName(*Folder));
	}

	return Actor;
}

// ============================================================================
// create_parametric_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateParametricMesh(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	FString Type;
	if (!Params->TryGetStringField(TEXT("type"), Type))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: type"));
	}
	Type = Type.ToLower().TrimStartAndEnd();

	// Parse dimensions with type-appropriate defaults
	float Width, Depth, Height;
	FString DimensionError;
	bool bParsedDimensions = true;

	// Type-specific dimension defaults (cm)
	if      (Type == TEXT("chair"))        bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 45, 45, 90, DimensionError);
	else if (Type == TEXT("table"))        bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 120, 75, 75, DimensionError);
	else if (Type == TEXT("desk"))         bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 120, 60, 75, DimensionError);
	else if (Type == TEXT("shelf"))        bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 80, 30, 180, DimensionError);
	else if (Type == TEXT("cabinet"))      bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 60, 45, 90, DimensionError);
	else if (Type == TEXT("bed"))          bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 100, 200, 55, DimensionError);
	else if (Type == TEXT("door_frame"))   bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 90, 15, 210, DimensionError);
	else if (Type == TEXT("window_frame")) bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 120, 20, 100, DimensionError);
	else if (Type == TEXT("stairs"))       bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 90, 28, 18, DimensionError);
	else if (Type == TEXT("ramp"))         bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 100, 200, 100, DimensionError);
	else if (Type == TEXT("pillar"))       bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 30, 30, 300, DimensionError);
	else if (Type == TEXT("counter"))      bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 200, 60, 90, DimensionError);
	else if (Type == TEXT("toilet"))       bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 40, 65, 40, DimensionError);
	else if (Type == TEXT("sink"))         bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 60, 45, 85, DimensionError);
	else if (Type == TEXT("bathtub"))      bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 75, 170, 60, DimensionError);
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown type '%s'. Valid: chair, table, desk, shelf, cabinet, bed, door_frame, window_frame, stairs, ramp, pillar, counter, toilet, sink, bathtub"), *Type));
	}
	if (!bParsedDimensions)
	{
		return FMonolithActionResult::Error(DimensionError);
	}

	auto SubParams = ParseSubParams(Params);

	// Create working mesh
	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FString BuildError;
	bool bHadBooleans = false;

	// Dispatch to type-specific builder
	if (Type == TEXT("chair"))
	{
		float SeatH = 45.0f;
		if (SubParams->HasField(TEXT("seat_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("seat_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'seat_height'. Expected number."));
			SeatH = static_cast<float>(TempVal);
		}
		float BackH = Height - SeatH;
		if (SubParams->HasField(TEXT("back_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("back_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'back_height'. Expected number."));
			BackH = static_cast<float>(TempVal);
		}
		float LegT = 4.0f;
		if (SubParams->HasField(TEXT("leg_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("leg_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'leg_thickness'. Expected number."));
			LegT = static_cast<float>(TempVal);
		}
		if (!BuildChair(Mesh, Width, Depth, Height, SeatH, BackH, LegT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("table"))
	{
		float LegT = 5.0f;
		if (SubParams->HasField(TEXT("leg_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("leg_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'leg_thickness'. Expected number."));
			LegT = static_cast<float>(TempVal);
		}
		float TopT = 4.0f;
		if (SubParams->HasField(TEXT("top_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("top_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'top_thickness'. Expected number."));
			TopT = static_cast<float>(TempVal);
		}
		if (!BuildTable(Mesh, Width, Depth, Height, LegT, TopT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("desk"))
	{
		float LegT = 5.0f;
		if (SubParams->HasField(TEXT("leg_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("leg_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'leg_thickness'. Expected number."));
			LegT = static_cast<float>(TempVal);
		}
		float TopT = 4.0f;
		if (SubParams->HasField(TEXT("top_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("top_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'top_thickness'. Expected number."));
			TopT = static_cast<float>(TempVal);
		}
		bool bDrawer = true;
		if (SubParams->HasField(TEXT("has_drawer")) && !SubParams->TryGetBoolField(TEXT("has_drawer"), bDrawer))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'has_drawer'. Expected boolean."));
		}
		if (!BuildDesk(Mesh, Width, Depth, Height, LegT, TopT, bDrawer, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("shelf"))
	{
		int32 ShelfN = 4;
		if (SubParams->HasField(TEXT("shelf_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("shelf_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'shelf_count'. Expected number."));
			ShelfN = static_cast<int32>(TempVal);
		}
		float BoardT = 2.0f;
		if (SubParams->HasField(TEXT("board_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("board_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'board_thickness'. Expected number."));
			BoardT = static_cast<float>(TempVal);
		}
		if (!BuildShelf(Mesh, Width, Depth, Height, ShelfN, BoardT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("cabinet"))
	{
		float WallT = 3.0f;
		if (SubParams->HasField(TEXT("wall_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
			WallT = static_cast<float>(TempVal);
		}
		float RecessD = Depth - WallT * 2;
		if (SubParams->HasField(TEXT("recess_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("recess_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'recess_depth'. Expected number."));
			RecessD = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildCabinet(Mesh, Width, Depth, Height, WallT, RecessD, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("bed"))
	{
		float MattH = 20.0f;
		if (SubParams->HasField(TEXT("mattress_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("mattress_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'mattress_height'. Expected number."));
			MattH = static_cast<float>(TempVal);
		}
		float HeadH = 50.0f;
		if (SubParams->HasField(TEXT("headboard_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("headboard_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'headboard_height'. Expected number."));
			HeadH = static_cast<float>(TempVal);
		}
		if (!BuildBed(Mesh, Width, Depth, Height, MattH, HeadH, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("door_frame"))
	{
		float FrameT = 10.0f;
		if (SubParams->HasField(TEXT("frame_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("frame_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'frame_thickness'. Expected number."));
			FrameT = static_cast<float>(TempVal);
		}
		float FrameD = Depth;
		if (SubParams->HasField(TEXT("frame_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("frame_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'frame_depth'. Expected number."));
			FrameD = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildDoorFrame(Mesh, Width, Height, FrameT, FrameD, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("window_frame"))
	{
		float FrameT = 8.0f;
		if (SubParams->HasField(TEXT("frame_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("frame_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'frame_thickness'. Expected number."));
			FrameT = static_cast<float>(TempVal);
		}
		float FrameD = Depth;
		if (SubParams->HasField(TEXT("frame_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("frame_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'frame_depth'. Expected number."));
			FrameD = static_cast<float>(TempVal);
		}
		float SillH = 90.0f;
		if (SubParams->HasField(TEXT("sill_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("sill_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'sill_height'. Expected number."));
			SillH = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildWindowFrame(Mesh, Width, Height, FrameT, FrameD, SillH, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("stairs"))
	{
		int32 StepN = 8;
		if (SubParams->HasField(TEXT("stair_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("stair_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'stair_count'. Expected number."));
			StepN = static_cast<int32>(TempVal);
		}
		float StepH = Height;
		if (SubParams->HasField(TEXT("step_height")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("step_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'step_height'. Expected number."));
			StepH = static_cast<float>(TempVal);
		}
		float StepD = Depth;
		if (SubParams->HasField(TEXT("step_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("step_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'step_depth'. Expected number."));
			StepD = static_cast<float>(TempVal);
		}
		bool bFloat = false;
		if (SubParams->HasField(TEXT("floating")) && !SubParams->TryGetBoolField(TEXT("floating"), bFloat))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'floating'. Expected boolean."));
		}
		if (!BuildStairs(Mesh, Width, StepH, StepD, StepN, bFloat, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("ramp"))
	{
		if (!BuildRamp(Mesh, Width, Depth, Height, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("pillar"))
	{
		float Radius = Width * 0.5f;
		int32 Sides = 12;
		if (SubParams->HasField(TEXT("sides")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("sides"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'sides'. Expected number."));
			Sides = static_cast<int32>(TempVal);
		}
		bool bRound = true;
		if (SubParams->HasField(TEXT("round")) && !SubParams->TryGetBoolField(TEXT("round"), bRound))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'round'. Expected boolean."));
		}
		if (!BuildPillar(Mesh, Radius, Height, Sides, bRound, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("counter"))
	{
		float TopT = 5.0f;
		if (SubParams->HasField(TEXT("top_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("top_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'top_thickness'. Expected number."));
			TopT = static_cast<float>(TempVal);
		}
		if (!BuildCounter(Mesh, Width, Depth, Height, TopT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("toilet"))
	{
		float BowlD = 15.0f;
		if (SubParams->HasField(TEXT("bowl_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("bowl_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'bowl_depth'. Expected number."));
			BowlD = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildToilet(Mesh, Width, Depth, Height, BowlD, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("sink"))
	{
		float BowlR = FMath::Min(Width, Depth) * 0.35f;
		if (SubParams->HasField(TEXT("bowl_radius")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("bowl_radius"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'bowl_radius'. Expected number."));
			BowlR = static_cast<float>(TempVal);
		}
		float BowlD = 12.0f;
		if (SubParams->HasField(TEXT("bowl_depth")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("bowl_depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'bowl_depth'. Expected number."));
			BowlD = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildSink(Mesh, Width, Depth, Height, BowlR, BowlD, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("bathtub"))
	{
		float WallT = 5.0f;
		if (SubParams->HasField(TEXT("wall_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
			WallT = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildBathtub(Mesh, Width, Depth, Height, WallT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}

	// Cleanup pass
	CleanupMesh(Mesh, bHadBooleans);

	int32 TriCount = Mesh->GetTriangleCount();

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetBoolField(TEXT("had_booleans"), bHadBooleans);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, FString::Printf(TEXT("parametric:%s"), *Type),
		TEXT("create_parametric_mesh"), TEXT("Parametric"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// create_horror_prop
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateHorrorProp(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	FString Type;
	if (!Params->TryGetStringField(TEXT("type"), Type))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: type"));
	}
	Type = Type.ToLower().TrimStartAndEnd();

	int32 Seed = 0;
	if (Params->HasField(TEXT("seed")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("seed"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'seed'. Expected number."));
		Seed = static_cast<int32>(TempVal);
	}

	float Width, Depth, Height;
	FString DimensionError;
	bool bParsedDimensions = true;

	if      (Type == TEXT("barricade"))   bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 120, 10, 200, DimensionError);
	else if (Type == TEXT("debris_pile")) bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 150, 150, 60, DimensionError);
	else if (Type == TEXT("cage"))        bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 100, 100, 200, DimensionError);
	else if (Type == TEXT("coffin"))      bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 60, 200, 50, DimensionError);
	else if (Type == TEXT("gurney"))      bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 70, 190, 90, DimensionError);
	else if (Type == TEXT("broken_wall")) bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 300, 25, 250, DimensionError);
	else if (Type == TEXT("vent_grate"))  bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 60, 5, 40, DimensionError);
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown horror prop type '%s'. Valid: barricade, debris_pile, cage, coffin, gurney, broken_wall, vent_grate"), *Type));
	}
	if (!bParsedDimensions)
	{
		return FMonolithActionResult::Error(DimensionError);
	}

	auto SubParams = ParseSubParams(Params);

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FString BuildError;
	bool bHadBooleans = false;

	if (Type == TEXT("barricade"))
	{
		int32 BoardN = 5;
		if (SubParams->HasField(TEXT("board_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("board_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'board_count'. Expected number."));
			BoardN = static_cast<int32>(TempVal);
		}
		float GapR = 0.3f;
		if (SubParams->HasField(TEXT("gap_ratio")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("gap_ratio"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'gap_ratio'. Expected number."));
			GapR = static_cast<float>(TempVal);
		}
		if (!BuildBarricade(Mesh, Width, Height, Depth, BoardN, GapR, Seed, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("debris_pile"))
	{
		float Radius  = FMath::Max(Width, Depth) * 0.5f;
		int32 PieceN = 12;
		if (SubParams->HasField(TEXT("piece_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("piece_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'piece_count'. Expected number."));
			PieceN = static_cast<int32>(TempVal);
		}
		if (!BuildDebrisPile(Mesh, Radius, Height, PieceN, Seed, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("cage"))
	{
		int32 BarN = 8;
		if (SubParams->HasField(TEXT("bar_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("bar_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'bar_count'. Expected number."));
			BarN = static_cast<int32>(TempVal);
		}
		float BarR = 1.5f;
		if (SubParams->HasField(TEXT("bar_radius")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("bar_radius"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'bar_radius'. Expected number."));
			BarR = static_cast<float>(TempVal);
		}
		if (!BuildCage(Mesh, Width, Depth, Height, BarN, BarR, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("coffin"))
	{
		float WallT = 3.0f;
		if (SubParams->HasField(TEXT("wall_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
			WallT = static_cast<float>(TempVal);
		}
		float LidGap = 2.0f;
		if (SubParams->HasField(TEXT("lid_gap")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("lid_gap"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'lid_gap'. Expected number."));
			LidGap = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildCoffin(Mesh, Width, Depth, Height, WallT, LidGap, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("gurney"))
	{
		float LegR = 2.0f;
		if (SubParams->HasField(TEXT("leg_radius")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("leg_radius"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'leg_radius'. Expected number."));
			LegR = static_cast<float>(TempVal);
		}
		if (!BuildGurney(Mesh, Width, Depth, Height, LegR, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("broken_wall"))
	{
		float NoiseS = 0.3f;
		if (SubParams->HasField(TEXT("noise_scale")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("noise_scale"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'noise_scale'. Expected number."));
			NoiseS = static_cast<float>(TempVal);
		}
		float HoleR = 60.0f;
		if (SubParams->HasField(TEXT("hole_radius")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("hole_radius"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'hole_radius'. Expected number."));
			HoleR = static_cast<float>(TempVal);
		}
		bHadBooleans = true;
		if (!BuildBrokenWall(Mesh, Width, Height, Depth, NoiseS, HoleR, Seed, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}
	else if (Type == TEXT("vent_grate"))
	{
		int32 SlotN = 6;
		if (SubParams->HasField(TEXT("slot_count")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("slot_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'slot_count'. Expected number."));
			SlotN = static_cast<int32>(TempVal);
		}
		float FrameT = 3.0f;
		if (SubParams->HasField(TEXT("frame_thickness")))
		{
			double TempVal;
			if (!SubParams->TryGetNumberField(TEXT("frame_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'frame_thickness'. Expected number."));
			FrameT = static_cast<float>(TempVal);
		}
		if (!BuildVentGrate(Mesh, Width, Height, Depth, SlotN, FrameT, BuildError))
			return FMonolithActionResult::Error(BuildError);
	}

	CleanupMesh(Mesh, bHadBooleans);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetBoolField(TEXT("had_booleans"), bHadBooleans);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, FString::Printf(TEXT("horror:%s"), *Type),
		TEXT("create_horror_prop"), TEXT("Horror"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Parametric Furniture Builders
// ============================================================================

bool FMonolithMeshProceduralActions::BuildChair(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float SeatHeight, float BackHeight, float LegThickness, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float SeatT = 3.0f; // seat thickness
	float HalfLeg = LegThickness * 0.5f;

	// Seat (Origin=Base, so Z is bottom of box)
	FTransform SeatXf(FRotator::ZeroRotator, FVector(0, 0, SeatHeight - SeatT), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, SeatXf, Width, Depth, SeatT, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Back rest
	float BackZ = SeatHeight;
	FTransform BackXf(FRotator::ZeroRotator, FVector(0, -Depth * 0.5f + HalfLeg, BackZ), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BackXf, Width, LegThickness, BackHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// 4 Legs (cylinders, Origin=Base)
	float LegInsetX = Width * 0.5f - HalfLeg - 1.0f;
	float LegInsetY = Depth * 0.5f - HalfLeg - 1.0f;

	FVector2D LegPositions[] = {
		{ -LegInsetX, -LegInsetY },
		{  LegInsetX, -LegInsetY },
		{ -LegInsetX,  LegInsetY },
		{  LegInsetX,  LegInsetY }
	};

	for (const auto& Pos : LegPositions)
	{
		FTransform LegXf(FRotator::ZeroRotator, FVector(Pos.X, Pos.Y, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(Mesh, Opts, LegXf, HalfLeg, SeatHeight, 8, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
	}

	return true;
}

bool FMonolithMeshProceduralActions::BuildTable(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float LegThickness, float TopThickness, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float HalfLeg = LegThickness * 0.5f;
	float LegHeight = Height - TopThickness;

	// Table top
	FTransform TopXf(FRotator::ZeroRotator, FVector(0, 0, LegHeight), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, TopXf, Width, Depth, TopThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// 4 Legs
	float InX = Width * 0.5f - HalfLeg - 2.0f;
	float InY = Depth * 0.5f - HalfLeg - 2.0f;
	FVector2D Corners[] = { {-InX,-InY}, {InX,-InY}, {-InX,InY}, {InX,InY} };

	for (const auto& C : Corners)
	{
		FTransform LegXf(FRotator::ZeroRotator, FVector(C.X, C.Y, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, LegXf, LegThickness, LegThickness, LegHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	return true;
}

bool FMonolithMeshProceduralActions::BuildDesk(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float LegThickness, float TopThickness, bool bHasDrawer, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float LegHeight = Height - TopThickness;

	// Top
	FTransform TopXf(FRotator::ZeroRotator, FVector(0, 0, LegHeight), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, TopXf, Width, Depth, TopThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// 4 Legs
	float HalfLeg = LegThickness * 0.5f;
	float InX = Width * 0.5f - HalfLeg - 2.0f;
	float InY = Depth * 0.5f - HalfLeg - 2.0f;
	FVector2D Corners[] = { {-InX,-InY}, {InX,-InY}, {-InX,InY}, {InX,InY} };

	for (const auto& C : Corners)
	{
		FTransform LegXf(FRotator::ZeroRotator, FVector(C.X, C.Y, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, LegXf, LegThickness, LegThickness, LegHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	// Modesty panel (back)
	float PanelHeight = LegHeight * 0.6f;
	FTransform PanelXf(FRotator::ZeroRotator, FVector(0, -Depth * 0.5f + 1.0f, LegHeight - PanelHeight), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, PanelXf, Width, 2.0f, PanelHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Optional drawer box (right side)
	if (bHasDrawer)
	{
		float DrawerW = Width * 0.35f;
		float DrawerH = LegHeight * 0.25f;
		FTransform DrawerXf(FRotator::ZeroRotator, FVector(Width * 0.5f - DrawerW * 0.5f - LegThickness, 0, LegHeight - DrawerH - TopThickness), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, DrawerXf, DrawerW, Depth - LegThickness * 2, DrawerH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	return true;
}

bool FMonolithMeshProceduralActions::BuildShelf(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	int32 ShelfCount, float BoardThickness, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	ShelfCount = FMath::Clamp(ShelfCount, 2, 20);

	// Two side panels
	float SideThickness = 2.0f;
	FTransform LeftXf(FRotator::ZeroRotator, FVector(-Width * 0.5f + SideThickness * 0.5f, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, LeftXf, SideThickness, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	FTransform RightXf(FRotator::ZeroRotator, FVector(Width * 0.5f - SideThickness * 0.5f, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, RightXf, SideThickness, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Shelves using AppendMeshRepeated
	float InnerWidth = Width - SideThickness * 2;
	float Spacing = (Height - BoardThickness) / FMath::Max(1, ShelfCount - 1);

	// Create one shelf template
	UDynamicMesh* ShelfTemplate = NewObject<UDynamicMesh>(Pool);
	FTransform ShelfBaseXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(ShelfTemplate, Opts, ShelfBaseXf, InnerWidth, Depth, BoardThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// First shelf at z=0
	FTransform FirstXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, ShelfTemplate, FirstXf);

	// Repeated shelves above
	if (ShelfCount > 1)
	{
		FTransform StepXf(FRotator::ZeroRotator, FVector(0, 0, Spacing), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(
			Mesh, ShelfTemplate, StepXf, ShelfCount - 1, true);
	}

	// Back panel
	FTransform BackXf(FRotator::ZeroRotator, FVector(0, -Depth * 0.5f + 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BackXf, InnerWidth, 1.0f, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	return true;
}

bool FMonolithMeshProceduralActions::BuildCabinet(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float WallThickness, float RecessDepth, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Outer box
	FTransform OuterXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, OuterXf, Width, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Inner recess (boolean subtract)
	UDynamicMesh* Cutter = NewObject<UDynamicMesh>(Pool);
	float CutW = Width - WallThickness * 2;
	float CutH = Height - WallThickness * 2;
	float CutD = FMath::Min(RecessDepth, Depth - WallThickness);

	// Position cutter at front face, inset by wall thickness
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, Depth * 0.5f - CutD * 0.5f, WallThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Cutter, Opts, CutXf, CutW, CutD, CutH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, Cutter, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildBed(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float MattressHeight, float HeadboardHeight, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float FrameH = Height - MattressHeight;

	// Frame
	FTransform FrameXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, FrameXf, Width, Depth, FrameH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Mattress (slightly inset)
	float MattInset = 3.0f;
	FTransform MattXf(FRotator::ZeroRotator, FVector(0, 0, FrameH), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, MattXf, Width - MattInset * 2, Depth - MattInset * 2, MattressHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Headboard
	FTransform HeadXf(FRotator::ZeroRotator, FVector(0, -Depth * 0.5f + 2.0f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, HeadXf, Width, 4.0f, HeadboardHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	return true;
}

bool FMonolithMeshProceduralActions::BuildDoorFrame(UDynamicMesh* Mesh, float Width, float Height,
	float FrameThickness, float FrameDepth, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float TotalW = Width + FrameThickness * 2;
	float TotalH = Height + FrameThickness;

	// Outer frame block
	FTransform OuterXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, OuterXf, TotalW, FrameDepth, TotalH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Inner cutout (the door opening)
	UDynamicMesh* Cutter = NewObject<UDynamicMesh>(Pool);
	// Oversized depth to ensure clean boolean
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Cutter, Opts, CutXf, Width, FrameDepth + 10.0f, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, Cutter, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildWindowFrame(UDynamicMesh* Mesh, float Width, float Height,
	float FrameThickness, float FrameDepth, float SillHeight, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	float TotalW = Width + FrameThickness * 2;
	float TotalH = Height + FrameThickness * 2 + SillHeight;

	// Outer wall section (full wall)
	FTransform OuterXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, OuterXf, TotalW, FrameDepth, TotalH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Window cutout
	UDynamicMesh* Cutter = NewObject<UDynamicMesh>(Pool);
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, 0, SillHeight), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Cutter, Opts, CutXf, Width, FrameDepth + 10.0f, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, Cutter, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildStairs(UDynamicMesh* Mesh, float Width, float StepHeight, float StepDepth,
	int32 StepCount, bool bFloating, FString& OutError)
{
	StepCount = FMath::Clamp(StepCount, 1, 100);

	FGeometryScriptPrimitiveOptions Opts;
	FTransform StairXf = FTransform::Identity;

	// Native staircase primitive — no need to build from boxes
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendLinearStairs(
		Mesh, Opts, StairXf, Width, StepHeight, StepDepth, StepCount, bFloating);

	return true;
}

bool FMonolithMeshProceduralActions::BuildRamp(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Build a ramp as a box, then shear the top face.
	// Simpler approach: use a wedge via AppendBox + plane cut.
	// Even simpler: AppendBox full size, then cut diagonally.
	// Simplest reliable approach: build as a box and use TransformMeshSelection.
	// Actually, just use a box and scale — or build from triangulated polygon extrude.
	// For blockout quality, a box with one side sliced off works:

	// Create full box
	FTransform BoxXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BoxXf, Width, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Plane cut: remove the upper-front triangle.
	// Cut plane normal points into the material we want to keep.
	// We want to cut from the top-front corner (0, Depth/2, Height) to bottom-back corner (0, -Depth/2, 0).
	// Normal = cross product of the ramp surface = pointing upward-forward.
	FVector RampNormal = FVector(0, -Height, Depth).GetSafeNormal();
	FVector RampPoint = FVector(0, Depth * 0.5f, Height);
	FTransform CutFrame(FRotationMatrix::MakeFromZ(RampNormal).Rotator(), RampPoint, FVector::OneVector);

	FGeometryScriptMeshPlaneCutOptions CutOpts;
	CutOpts.bFillHoles = true;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneCut(Mesh, CutFrame, CutOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildPillar(UDynamicMesh* Mesh, float Radius, float Height,
	int32 Sides, bool bRound, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;
	FTransform PillarXf = FTransform::Identity;

	if (bRound)
	{
		Sides = FMath::Clamp(Sides, 6, 64);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			Mesh, Opts, PillarXf, Radius, Height, Sides, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
	}
	else
	{
		// Square pillar
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, PillarXf, Radius * 2, Radius * 2, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	return true;
}

bool FMonolithMeshProceduralActions::BuildCounter(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float TopThickness, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Counter top
	FTransform TopXf(FRotator::ZeroRotator, FVector(0, 0, Height - TopThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, TopXf, Width, Depth, TopThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Base cabinet body (inset slightly from top)
	float BaseInset = 3.0f;
	float BaseH = Height - TopThickness - 10.0f; // kick space at bottom
	FTransform BaseXf(FRotator::ZeroRotator, FVector(0, BaseInset, 10.0f), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BaseXf, Width - BaseInset * 2, Depth - BaseInset, BaseH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	return true;
}

bool FMonolithMeshProceduralActions::BuildToilet(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float BowlDepth, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Base/tank (box)
	FTransform BaseXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BaseXf, Width, Depth * 0.5f, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Bowl rim (cylinder at front)
	float BowlRadius = Width * 0.45f;
	FTransform BowlXf(FRotator::ZeroRotator, FVector(0, Depth * 0.25f, Height * 0.6f), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(Mesh, Opts, BowlXf, BowlRadius, Height * 0.4f, 12, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	// Bowl interior subtraction (sphere)
	UDynamicMesh* BowlCut = NewObject<UDynamicMesh>(Pool);
	float CutRadius = BowlRadius * 0.8f;
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, Depth * 0.25f, Height - BowlDepth + CutRadius * 0.3f), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(BowlCut, Opts, CutXf, CutRadius, 8, 12, EGeometryScriptPrimitiveOriginMode::Center);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, BowlCut, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildSink(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float BowlRadius, float BowlDepth, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Pedestal / vanity
	FTransform BaseXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BaseXf, Width, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Bowl subtraction (sphere centered at top surface)
	UDynamicMesh* BowlCut = NewObject<UDynamicMesh>(Pool);
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, Depth * 0.1f, Height - BowlDepth + BowlRadius * 0.4f), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(BowlCut, Opts, CutXf, BowlRadius, 8, 12, EGeometryScriptPrimitiveOriginMode::Center);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, BowlCut, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildBathtub(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float WallThickness, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Outer shell
	FTransform OuterXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, OuterXf, Width, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Inner subtraction (smaller box from top)
	UDynamicMesh* InnerCut = NewObject<UDynamicMesh>(Pool);
	float CutW = Width - WallThickness * 2;
	float CutD = Depth - WallThickness * 2;
	float CutH = Height - WallThickness; // leave bottom thickness
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, 0, WallThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(InnerCut, Opts, CutXf, CutW, CutD, CutH + 10.0f, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, InnerCut, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

// ============================================================================
// Horror Prop Builders
// ============================================================================

bool FMonolithMeshProceduralActions::BuildBarricade(UDynamicMesh* Mesh, float Width, float Height, float Depth,
	int32 BoardCount, float GapRatio, int32 Seed, FString& OutError)
{
	BoardCount = FMath::Clamp(BoardCount, 2, 20);
	GapRatio = FMath::Clamp(GapRatio, 0.05f, 0.8f);

	FRandomStream Rng(Seed);
	FGeometryScriptPrimitiveOptions Opts;

	float TotalSlots = BoardCount + (BoardCount - 1) * GapRatio;
	float BoardH = Height / TotalSlots;
	float GapH = BoardH * GapRatio;

	// Create one plank template
	UDynamicMesh* Plank = NewObject<UDynamicMesh>(Pool);
	FTransform PlankXf = FTransform::Identity;
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Plank, Opts, PlankXf, Width, Depth, BoardH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Place planks with random offsets and slight rotations
	TArray<FTransform> PlankTransforms;
	for (int32 i = 0; i < BoardCount; ++i)
	{
		float Z = i * (BoardH + GapH);
		float OffX = Rng.FRandRange(-Width * 0.05f, Width * 0.05f);
		float RotRoll = Rng.FRandRange(-3.0f, 3.0f);

		PlankTransforms.Add(FTransform(FRotator(0, 0, RotRoll), FVector(OffX, 0, Z), FVector::OneVector));
	}

	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshTransformed(
		Mesh, Plank, PlankTransforms, FTransform::Identity, true);

	return true;
}

bool FMonolithMeshProceduralActions::BuildDebrisPile(UDynamicMesh* Mesh, float Radius, float Height,
	int32 PieceCount, int32 Seed, FString& OutError)
{
	PieceCount = FMath::Clamp(PieceCount, 3, 50);
	FRandomStream Rng(Seed);
	FGeometryScriptPrimitiveOptions Opts;

	for (int32 i = 0; i < PieceCount; ++i)
	{
		// Random position within radius, biased toward center and ground
		float Angle = Rng.FRandRange(0.0f, 360.0f);
		float Dist = Rng.FRandRange(0.0f, Radius) * Rng.FRandRange(0.3f, 1.0f); // center bias
		float Z = Rng.FRandRange(0.0f, Height * 0.7f);

		FVector Pos(
			FMath::Cos(FMath::DegreesToRadians(Angle)) * Dist,
			FMath::Sin(FMath::DegreesToRadians(Angle)) * Dist,
			Z);

		FRotator Rot(Rng.FRandRange(-30, 30), Rng.FRandRange(0, 360), Rng.FRandRange(-30, 30));

		// Random piece size
		float SizeX = Rng.FRandRange(5, Radius * 0.3f);
		float SizeY = Rng.FRandRange(5, Radius * 0.3f);
		float SizeZ = Rng.FRandRange(3, Height * 0.3f);

		FTransform PieceXf(Rot, Pos, FVector::OneVector);

		// Alternate between boxes and cylinders for variety
		if (i % 3 == 0)
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
				Mesh, Opts, PieceXf, SizeX * 0.5f, SizeZ, 6, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
		}
		else
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, PieceXf, SizeX, SizeY, SizeZ, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		}
	}

	return true;
}

bool FMonolithMeshProceduralActions::BuildCage(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	int32 BarCount, float BarRadius, FString& OutError)
{
	BarCount = FMath::Clamp(BarCount, 3, 50);
	FGeometryScriptPrimitiveOptions Opts;

	// Create one vertical bar template
	UDynamicMesh* Bar = NewObject<UDynamicMesh>(Pool);
	FTransform BarXf = FTransform::Identity;
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(Bar, Opts, BarXf, BarRadius, Height, 6, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	// Front face bars using AppendMeshRepeated
	float FrontSpacing = Width / FMath::Max(1, BarCount - 1);
	FTransform FrontStart(FRotator::ZeroRotator, FVector(-Width * 0.5f, Depth * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, Bar, FrontStart);
	if (BarCount > 1)
	{
		FTransform FrontStep(FRotator::ZeroRotator, FVector(FrontSpacing, 0, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(Mesh, Bar, FrontStep, BarCount - 1, true);

		// Offset for back face (start from first back bar position)
		// We appended front bars at Y = +Depth/2, now do back at Y = -Depth/2
	}

	// Back face bars
	FTransform BackStart(FRotator::ZeroRotator, FVector(-Width * 0.5f, -Depth * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, Bar, BackStart);
	if (BarCount > 1)
	{
		FTransform BackStep(FRotator::ZeroRotator, FVector(FrontSpacing, 0, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(Mesh, Bar, BackStep, BarCount - 1, true);
	}

	// Side bars (fewer, connecting front/back)
	int32 SideBarCount = FMath::Max(2, BarCount / 2);
	float SideSpacing = Depth / FMath::Max(1, SideBarCount - 1);

	FTransform LeftStart(FRotator::ZeroRotator, FVector(-Width * 0.5f, -Depth * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, Bar, LeftStart);
	if (SideBarCount > 1)
	{
		FTransform LeftStep(FRotator::ZeroRotator, FVector(0, SideSpacing, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(Mesh, Bar, LeftStep, SideBarCount - 1, true);
	}

	FTransform RightStart(FRotator::ZeroRotator, FVector(Width * 0.5f, -Depth * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, Bar, RightStart);
	if (SideBarCount > 1)
	{
		FTransform RightStep(FRotator::ZeroRotator, FVector(0, SideSpacing, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(Mesh, Bar, RightStep, SideBarCount - 1, true);
	}

	// Top frame (4 horizontal bars)
	UDynamicMesh* TopBarX = NewObject<UDynamicMesh>(Pool);
	FTransform TopBarXXf(FRotator(0, 0, 90), FVector(0, 0, 0), FVector::OneVector); // rotated to lie horizontal along X
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(TopBarX, Opts, TopBarXXf, BarRadius * 1.5f, Width, 6, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	FTransform TopFrontXf(FRotator::ZeroRotator, FVector(-Width * 0.5f, Depth * 0.5f, Height), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, TopBarX, TopFrontXf);
	FTransform TopBackXf(FRotator::ZeroRotator, FVector(-Width * 0.5f, -Depth * 0.5f, Height), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, TopBarX, TopBackXf);

	UDynamicMesh* TopBarY = NewObject<UDynamicMesh>(Pool);
	FTransform TopBarYXf(FRotator(90, 0, 0), FVector(0, 0, 0), FVector::OneVector); // rotated along Y
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(TopBarY, Opts, TopBarYXf, BarRadius * 1.5f, Depth, 6, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	FTransform TopLeftXf(FRotator::ZeroRotator, FVector(-Width * 0.5f, -Depth * 0.5f, Height), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, TopBarY, TopLeftXf);
	FTransform TopRightXf(FRotator::ZeroRotator, FVector(Width * 0.5f, -Depth * 0.5f, Height), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, TopBarY, TopRightXf);

	return true;
}

bool FMonolithMeshProceduralActions::BuildCoffin(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float WallThickness, float LidGap, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Base box (the coffin body)
	float BodyH = Height * 0.7f;
	FTransform BodyXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, BodyXf, Width, Depth, BodyH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Interior subtraction
	UDynamicMesh* InnerCut = NewObject<UDynamicMesh>(Pool);
	float CutW = Width - WallThickness * 2;
	float CutD = Depth - WallThickness * 2;
	float CutH = BodyH - WallThickness;
	FTransform CutXf(FRotator::ZeroRotator, FVector(0, 0, WallThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(InnerCut, Opts, CutXf, CutW, CutD, CutH + 10.0f, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, InnerCut, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	// Lid (separate box, slightly offset/open)
	float LidH = Height - BodyH;
	FTransform LidXf(FRotator(5.0f, 0, 0), FVector(0, LidGap, BodyH + LidGap), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, LidXf, Width, Depth, LidH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	return true;
}

bool FMonolithMeshProceduralActions::BuildGurney(UDynamicMesh* Mesh, float Width, float Depth, float Height,
	float LegRadius, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	float TopThickness = 3.0f;
	float LegHeight = Height - TopThickness;

	// Flat surface top
	FTransform TopXf(FRotator::ZeroRotator, FVector(0, 0, LegHeight), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, TopXf, Width, Depth, TopThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// 4 legs (thin cylinders)
	float InX = Width * 0.5f - LegRadius * 2;
	float InY = Depth * 0.5f - LegRadius * 2;
	FVector2D Corners[] = { {-InX,-InY}, {InX,-InY}, {-InX,InY}, {InX,InY} };

	for (const auto& C : Corners)
	{
		FTransform LegXf(FRotator::ZeroRotator, FVector(C.X, C.Y, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(Mesh, Opts, LegXf, LegRadius, LegHeight, 8, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
	}

	// Side rails
	float RailHeight = 15.0f;
	float RailThickness = 1.5f;
	FTransform LeftRailXf(FRotator::ZeroRotator, FVector(-Width * 0.5f + RailThickness * 0.5f, 0, LegHeight + TopThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, LeftRailXf, RailThickness, Depth * 0.6f, RailHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	FTransform RightRailXf(FRotator::ZeroRotator, FVector(Width * 0.5f - RailThickness * 0.5f, 0, LegHeight + TopThickness), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, RightRailXf, RailThickness, Depth * 0.6f, RailHeight, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	return true;
}

bool FMonolithMeshProceduralActions::BuildBrokenWall(UDynamicMesh* Mesh, float Width, float Height, float Thickness,
	float NoiseScale, float HoleRadius, int32 Seed, FString& OutError)
{
	FGeometryScriptPrimitiveOptions Opts;

	// Wall with subdivisions (needed so noise has vertices to deform)
	int32 SubsX = FMath::Max(1, FMath::RoundToInt32(Width / 15.0f));
	int32 SubsZ = FMath::Max(1, FMath::RoundToInt32(Height / 15.0f));
	FTransform WallXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, WallXf, Width, Thickness, Height, SubsX, 1, SubsZ, EGeometryScriptPrimitiveOriginMode::Base);

	// Create noise-deformed sphere as cutter
	UDynamicMesh* Cutter = NewObject<UDynamicMesh>(Pool);
	FTransform SphereXf(FRotator::ZeroRotator, FVector(0, 0, Height * 0.5f), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
		Cutter, Opts, SphereXf, HoleRadius, 12, 16, EGeometryScriptPrimitiveOriginMode::Center);

	// Apply Perlin noise to the cutter sphere for irregular hole shape
	FGeometryScriptPerlinNoiseOptions NoiseOpts;
	NoiseOpts.BaseLayer.Magnitude = HoleRadius * NoiseScale;
	NoiseOpts.BaseLayer.Frequency = 0.03f;
	NoiseOpts.BaseLayer.RandomSeed = Seed;
	NoiseOpts.bApplyAlongNormal = true;

	FGeometryScriptMeshSelection EmptySelection; // empty = full mesh
	UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh2(Cutter, EmptySelection, NoiseOpts);

	// Boolean subtract the deformed sphere from the wall
	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Mesh, FTransform::Identity, Cutter, FTransform::Identity,
		EGeometryScriptBooleanOperation::Subtract, BoolOpts);

	return true;
}

bool FMonolithMeshProceduralActions::BuildVentGrate(UDynamicMesh* Mesh, float Width, float Height, float Depth,
	int32 SlotCount, float FrameThickness, FString& OutError)
{
	SlotCount = FMath::Clamp(SlotCount, 1, 30);
	FGeometryScriptPrimitiveOptions Opts;

	// Outer frame
	FTransform FrameXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, FrameXf, Width, Depth, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Horizontal bars using AppendMeshRepeated
	float InnerW = Width - FrameThickness * 2;
	float BarThickness = 1.5f;
	float TotalSlotH = Height - FrameThickness * 2;
	float Spacing = TotalSlotH / (SlotCount + 1); // even spacing including bars

	UDynamicMesh* BarTemplate = NewObject<UDynamicMesh>(Pool);
	FTransform BarXf = FTransform::Identity;
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(BarTemplate, Opts, BarXf, InnerW, Depth + 2.0f, BarThickness, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// First bar
	FTransform FirstBarXf(FRotator::ZeroRotator, FVector(0, 0, FrameThickness + Spacing - BarThickness * 0.5f), FVector::OneVector);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Mesh, BarTemplate, FirstBarXf);

	// Remaining bars
	if (SlotCount > 1)
	{
		FTransform StepXf(FRotator::ZeroRotator, FVector(0, 0, Spacing), FVector::OneVector);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(Mesh, BarTemplate, StepXf, SlotCount - 1, true);
	}

	return true;
}

// ============================================================================
// Additional Shared Helpers (Phases 19B-D)
// ============================================================================

bool FMonolithMeshProceduralActions::ParseVectorArray(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FVector>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (!Params->TryGetArrayField(Key, Arr) || Arr->Num() == 0)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Val : *Arr)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inner;
		if (Val->TryGetArray(Inner) && Inner->Num() >= 3)
		{
			FVector V;
			V.X = (*Inner)[0]->AsNumber();
			V.Y = (*Inner)[1]->AsNumber();
			V.Z = (*Inner)[2]->AsNumber();
			Out.Add(V);
		}
	}

	return Out.Num() > 0;
}

bool FMonolithMeshProceduralActions::ParseVector2DArray(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FVector2D>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (!Params->TryGetArrayField(Key, Arr) || Arr->Num() == 0)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Val : *Arr)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inner;
		if (Val->TryGetArray(Inner) && Inner->Num() >= 2)
		{
			FVector2D V;
			V.X = (*Inner)[0]->AsNumber();
			V.Y = (*Inner)[1]->AsNumber();
			Out.Add(V);
		}
	}

	return Out.Num() > 0;
}

TArray<FVector2D> FMonolithMeshProceduralActions::MakeCirclePolygon(float Radius, int32 Segments)
{
	Segments = FMath::Max(3, Segments);
	TArray<FVector2D> Poly;
	Poly.Reserve(Segments);
	for (int32 i = 0; i < Segments; ++i)
	{
		float Angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(Segments);
		Poly.Add(FVector2D(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius));
	}
	return Poly;
}

/** Create a rectangular wall cross-section profile for sweep-based wall construction.
 *  Profile X axis maps perpendicular to sweep path (wall thickness), Y axis maps to up (wall height).
 *  Profile is centered on X so the sweep path traces the wall centerline. */
static TArray<FVector2D> MakeWallProfile(float Thickness, float Height)
{
	TArray<FVector2D> Profile;
	float HalfT = Thickness * 0.5f;
	Profile.Add(FVector2D(-HalfT, 0.0f));
	Profile.Add(FVector2D( HalfT, 0.0f));
	Profile.Add(FVector2D( HalfT, Height));
	Profile.Add(FVector2D(-HalfT, Height));
	return Profile;
}

TArray<FVector2D> FMonolithMeshProceduralActions::InsetPolygon2D(const TArray<FVector2D>& Polygon, float InsetDist)
{
	const int32 N = Polygon.Num();
	if (N < 3) return Polygon;

	TArray<FVector2D> Result;
	Result.Reserve(N);

	for (int32 i = 0; i < N; ++i)
	{
		const FVector2D& Prev = Polygon[(i + N - 1) % N];
		const FVector2D& Curr = Polygon[i];
		const FVector2D& Next = Polygon[(i + 1) % N];

		// Edge normals (pointing inward for CCW polygon)
		FVector2D E0 = (Curr - Prev).GetSafeNormal();
		FVector2D E1 = (Next - Curr).GetSafeNormal();
		FVector2D N0(-E0.Y, E0.X); // right-hand normal
		FVector2D N1(-E1.Y, E1.X);

		// Average inward direction
		FVector2D AvgN = (N0 + N1);
		float Len = AvgN.Size();
		if (Len < KINDA_SMALL_NUMBER)
		{
			Result.Add(Curr + N0 * InsetDist);
		}
		else
		{
			AvgN /= Len;
			// Scale to maintain proper inset distance at corners
			float Dot = FVector2D::DotProduct(N0, AvgN);
			float Scale = (FMath::Abs(Dot) > 0.01f) ? (InsetDist / Dot) : InsetDist;
			Scale = FMath::Clamp(Scale, InsetDist * 0.5f, InsetDist * 3.0f); // prevent degenerate spikes
			Result.Add(Curr + AvgN * Scale);
		}
	}

	return Result;
}

FString FMonolithMeshProceduralActions::FinalizeProceduralMesh(UDynamicMesh* Mesh, const TSharedPtr<FJsonObject>& Params,
	const TSharedPtr<FJsonObject>& Result, const FString& HandleCategory,
	const FString& ActionName, const FString& Category)
{
	// ---- Cache check (before any generation work is consumed) ----
	bool bUseCache = true;
	if (!ActionName.IsEmpty() && Params->HasField(TEXT("use_cache")))
	{
		if (!Params->TryGetBoolField(TEXT("use_cache"), bUseCache))
		{
			return TEXT("Invalid type for parameter 'use_cache'. Expected boolean.");
		}
	}
	else if (ActionName.IsEmpty())
	{
		bUseCache = false;
	}
	bool bAutoSave = true;
	if (Params->HasField(TEXT("auto_save")) && !Params->TryGetBoolField(TEXT("auto_save"), bAutoSave))
	{
		return TEXT("Invalid type for parameter 'auto_save'. Expected boolean.");
	}
	FString Hash;

	if (bUseCache)
	{
		Hash = FMonolithMeshProceduralCache::Get().ComputeHash(ActionName, Params);
		FString CachedPath;
		if (FMonolithMeshProceduralCache::Get().TryGetCached(Hash, CachedPath))
		{
			// Cache hit — skip save, reuse existing asset
			Result->SetBoolField(TEXT("cache_hit"), true);
			Result->SetStringField(TEXT("save_path"), CachedPath);
			Result->SetStringField(TEXT("hash"), Hash);

			// Still store in handle pool if requested (load from the saved StaticMesh)
			FString HandleName;
			if (Params->TryGetStringField(TEXT("handle"), HandleName) && !HandleName.IsEmpty())
			{
				FString CreateErr;
				if (Pool->CreateHandle(HandleName, FString::Printf(TEXT("internal:%s"), *HandleCategory), CreateErr))
				{
					UDynamicMesh* HandleMesh = Pool->GetHandle(HandleName, CreateErr);
					if (HandleMesh && Mesh)
					{
						HandleMesh->SetMesh(Mesh->GetMeshRef());
					}
				}
				Result->SetStringField(TEXT("handle"), HandleName);
			}

			// Still place in scene if requested
			bool bPlace = false;
			if (Params->HasField(TEXT("place_in_scene")) && !Params->TryGetBoolField(TEXT("place_in_scene"), bPlace))
			{
				return TEXT("Invalid type for parameter 'place_in_scene'. Expected boolean.");
			}
			if (bPlace)
			{
				FVector Location = FVector::ZeroVector;
				MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location);
				FRotator Rotation = FRotator::ZeroRotator;
				MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);
				FString Label;
				Params->TryGetStringField(TEXT("label"), Label);
				bool bSnapToFloor = true;
				if (Params->HasField(TEXT("snap_to_floor")) && !Params->TryGetBoolField(TEXT("snap_to_floor"), bSnapToFloor))
				{
					return TEXT("Invalid type for parameter 'snap_to_floor'. Expected boolean.");
				}

				// Build default folder from Category (e.g., "Procedural/Parametric")
				FString Folder;
				Params->TryGetStringField(TEXT("folder"), Folder);
				if (Folder.IsEmpty() && !Category.IsEmpty())
				{
					Folder = FString::Printf(TEXT("Procedural/%s"), *Category);
				}

				AActor* Actor = PlaceMeshInScene(CachedPath, Location, Rotation, Label, bSnapToFloor, Folder);
				if (Actor)
				{
					Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
					Result->SetBoolField(TEXT("snapped_to_floor"), bSnapToFloor);
				}
			}

			return FString(); // success — cache hit
		}
	}

	// ---- Handle pool storage ----
	FString HandleName;
	if (Params->TryGetStringField(TEXT("handle"), HandleName) && !HandleName.IsEmpty())
	{
		FString CreateErr;
		if (!Pool->CreateHandle(HandleName, FString::Printf(TEXT("internal:%s"), *HandleCategory), CreateErr))
		{
			return CreateErr;
		}
		UDynamicMesh* HandleMesh = Pool->GetHandle(HandleName, CreateErr);
		if (HandleMesh)
		{
			HandleMesh->SetMesh(Mesh->GetMeshRef());
		}
		Result->SetStringField(TEXT("handle"), HandleName);
	}

	// ---- Determine save path (explicit, auto-generated, or none) ----
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);

	if (SavePath.IsEmpty() && bAutoSave && !Category.IsEmpty())
	{
		// Auto-generate an organized save path
		FString Type;
		Result->TryGetStringField(TEXT("type"), Type);
		if (Type.IsEmpty()) Type = HandleCategory;

		float Width = 100.0f, Depth = 100.0f, Height = 100.0f;
		const TSharedPtr<FJsonObject>* DimObj = nullptr;
		if (Params->TryGetObjectField(TEXT("dimensions"), DimObj) && DimObj && (*DimObj).IsValid())
		{
			if ((*DimObj)->HasField(TEXT("width")))
			{
				double TempVal;
				if (!(*DimObj)->TryGetNumberField(TEXT("width"), TempVal)) return TEXT("Invalid type for parameter 'dimensions.width'. Expected number.");
				Width = static_cast<float>(TempVal);
			}
			if ((*DimObj)->HasField(TEXT("depth")))
			{
				double TempVal;
				if (!(*DimObj)->TryGetNumberField(TEXT("depth"), TempVal)) return TEXT("Invalid type for parameter 'dimensions.depth'. Expected number.");
				Depth = static_cast<float>(TempVal);
			}
			if ((*DimObj)->HasField(TEXT("height")))
			{
				double TempVal;
				if (!(*DimObj)->TryGetNumberField(TEXT("height"), TempVal)) return TEXT("Invalid type for parameter 'dimensions.height'. Expected number.");
				Height = static_cast<float>(TempVal);
			}
		}
		// Also check top-level fields used by some actions (e.g. terrain size)
		if (Result->HasField(TEXT("size_x")))
		{
			double TempVal;
			if (!Result->TryGetNumberField(TEXT("size_x"), TempVal)) return TEXT("Invalid type for 'size_x'. Expected number.");
			Width = static_cast<float>(TempVal);
		}
		if (Result->HasField(TEXT("size_y")))
		{
			double TempVal;
			if (!Result->TryGetNumberField(TEXT("size_y"), TempVal)) return TEXT("Invalid type for 'size_y'. Expected number.");
			Depth = static_cast<float>(TempVal);
		}

		int32 Seed = -1;
		if (Params->HasField(TEXT("seed")))
		{
			double TempVal;
			if (!Params->TryGetNumberField(TEXT("seed"), TempVal)) return TEXT("Invalid type for parameter 'seed'. Expected number.");
			Seed = static_cast<int32>(TempVal);
		}

		FString HashForPath = bUseCache ? Hash : FMonolithMeshProceduralCache::Get().ComputeHash(ActionName, Params);
		SavePath = FMonolithMeshProceduralCache::Get().GenerateAutoPath(Category, Type, Width, Depth, Height, HashForPath, Seed);
	}

	// ---- Save to asset ----
	if (!SavePath.IsEmpty())
	{
		bool bOverwrite = false;
		if (Params->HasField(TEXT("overwrite")) && !Params->TryGetBoolField(TEXT("overwrite"), bOverwrite))
		{
			return TEXT("Invalid type for parameter 'overwrite'. Expected boolean.");
		}
		FString SaveErr;
		if (!SaveMeshToAsset(Mesh, SavePath, bOverwrite, SaveErr))
		{
			int32 TriCount = Mesh->GetTriangleCount();
			return FString::Printf(TEXT("Mesh generated (%d tris) but save failed: %s"), TriCount, *SaveErr);
		}
		Result->SetStringField(TEXT("save_path"), SavePath);

		// Register in cache
		if (bUseCache && !Hash.IsEmpty())
		{
			FString Type;
			Result->TryGetStringField(TEXT("type"), Type);
			int32 TriCount = 0;
			if (Result->HasField(TEXT("triangle_count")))
			{
				double TempVal;
				if (!Result->TryGetNumberField(TEXT("triangle_count"), TempVal)) return TEXT("Invalid type for 'triangle_count'. Expected number.");
				TriCount = static_cast<int32>(TempVal);
			}
			FMonolithMeshProceduralCache::Get().Register(Hash, SavePath, ActionName, Type, TriCount, Params);
			Result->SetStringField(TEXT("hash"), Hash);
		}

		// Place in scene if requested
		bool bPlace = false;
		if (Params->HasField(TEXT("place_in_scene")) && !Params->TryGetBoolField(TEXT("place_in_scene"), bPlace))
		{
			return TEXT("Invalid type for parameter 'place_in_scene'. Expected boolean.");
		}
		if (bPlace)
		{
			FVector Location = FVector::ZeroVector;
			MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location);
			FRotator Rotation = FRotator::ZeroRotator;
			MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);
			FString Label;
			Params->TryGetStringField(TEXT("label"), Label);
			bool bSnapToFloor = true;
			if (Params->HasField(TEXT("snap_to_floor")) && !Params->TryGetBoolField(TEXT("snap_to_floor"), bSnapToFloor))
			{
				return TEXT("Invalid type for parameter 'snap_to_floor'. Expected boolean.");
			}

			// Build default folder from Category
			FString Folder;
			Params->TryGetStringField(TEXT("folder"), Folder);
			if (Folder.IsEmpty() && !Category.IsEmpty())
			{
				Folder = FString::Printf(TEXT("Procedural/%s"), *Category);
			}

			AActor* Actor = PlaceMeshInScene(SavePath, Location, Rotation, Label, bSnapToFloor, Folder);
			if (Actor)
			{
				Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
				Result->SetBoolField(TEXT("snapped_to_floor"), bSnapToFloor);
			}
		}
	}

	Result->SetBoolField(TEXT("cache_hit"), false);

	return FString(); // empty = success
}

// ============================================================================
// Phase 19B: create_structure
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateStructure(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	FString Type;
	if (!Params->TryGetStringField(TEXT("type"), Type))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: type"));
	}
	Type = Type.ToLower().TrimStartAndEnd();

	float Width, Depth, Height;
	FString DimensionError;
	bool bParsedDimensions = true;
	if      (Type == TEXT("room"))        bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 400, 600, 300, DimensionError);
	else if (Type == TEXT("corridor"))    bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 200, 800, 300, DimensionError);
	else if (Type == TEXT("l_corridor"))  bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 200, 600, 300, DimensionError);
	else if (Type == TEXT("t_junction"))  bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 200, 600, 300, DimensionError);
	else if (Type == TEXT("stairwell"))   bParsedDimensions = ParseDimensions(Params, Width, Depth, Height, 300, 300, 600, DimensionError);
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown structure type '%s'. Valid: room, corridor, L_corridor, T_junction, stairwell"), *Type));
	}
	if (!bParsedDimensions)
	{
		return FMonolithActionResult::Error(DimensionError);
	}

	float WallT = 20.0f;
	if (Params->HasField(TEXT("wall_thickness")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
		WallT = static_cast<float>(TempVal);
	}
	float FloorT = 3.0f;
	if (Params->HasField(TEXT("floor_thickness")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("floor_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'floor_thickness'. Expected number."));
		FloorT = static_cast<float>(TempVal);
	}
	bool bCeiling = true;
	if (Params->HasField(TEXT("has_ceiling")) && !Params->TryGetBoolField(TEXT("has_ceiling"), bCeiling))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'has_ceiling'. Expected boolean."));
	}
	bool bFloor = true;
	if (Params->HasField(TEXT("has_floor")) && !Params->TryGetBoolField(TEXT("has_floor"), bFloor))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'has_floor'. Expected boolean."));
	}

	bool bAddTrim = true;
	if (Params->HasField(TEXT("add_trim")) && !Params->TryGetBoolField(TEXT("add_trim"), bAddTrim))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'add_trim'. Expected boolean."));
	}

	FString WallMode = TEXT("sweep");
	Params->TryGetStringField(TEXT("wall_mode"), WallMode);
	WallMode = WallMode.ToLower().TrimStartAndEnd();

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FGeometryScriptPrimitiveOptions Opts;
	bool bHadBooleans = false;

	// Legacy box-based wall construction (4 separate AppendBox calls per room)
	auto BuildWallsBox = [&](float W, float D, float H, float WT, FVector Offset)
	{
		// North wall (Y = -D/2)
		FTransform NorthXf(FRotator::ZeroRotator, Offset + FVector(0, -D * 0.5f + WT * 0.5f, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, NorthXf, W, WT, H, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		// South wall (Y = +D/2)
		FTransform SouthXf(FRotator::ZeroRotator, Offset + FVector(0, D * 0.5f - WT * 0.5f, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, SouthXf, W, WT, H, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		// East wall (X = +W/2)
		FTransform EastXf(FRotator::ZeroRotator, Offset + FVector(W * 0.5f - WT * 0.5f, 0, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, EastXf, WT, D - WT * 2, H, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		// West wall (X = -W/2)
		FTransform WestXf(FRotator::ZeroRotator, Offset + FVector(-W * 0.5f + WT * 0.5f, 0, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, WestXf, WT, D - WT * 2, H, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	};

	// Sweep-based thin wall construction: single AppendSimpleSweptPolygon per room
	// Sweeps a rectangular cross-section (Thickness x Height) along the room perimeter.
	// Produces proper mitered corners, fewer triangles, and natural UV flow.
	auto BuildWallsSweep = [&](float W, float D, float H, float WT, FVector Offset)
	{
		TArray<FVector2D> WallProfile = MakeWallProfile(WT, H);

		// Perimeter path: centerline of walls, CCW when viewed from above
		float HW = W * 0.5f;
		float HD = D * 0.5f;
		TArray<FVector> Path;
		Path.Add(Offset + FVector(-HW, -HD, 0.0f));  // NW corner
		Path.Add(Offset + FVector( HW, -HD, 0.0f));  // NE corner
		Path.Add(Offset + FVector( HW,  HD, 0.0f));  // SE corner
		Path.Add(Offset + FVector(-HW,  HD, 0.0f));  // SW corner

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleSweptPolygon(
			Mesh, Opts, FTransform::Identity,
			WallProfile, Path,
			/*bLoop=*/true, /*bCapped=*/false,
			/*StartScale=*/1.0f, /*EndScale=*/1.0f,
			/*RotationAngleDeg=*/0.0f, /*MiterLimit=*/1.5f);
	};

	// Select wall construction method
	auto BuildWalls = [&](float W, float D, float H, float WT, FVector Offset)
	{
		if (WallMode == TEXT("box"))
		{
			BuildWallsBox(W, D, H, WT, Offset);
		}
		else
		{
			BuildWallsSweep(W, D, H, WT, Offset);
		}
	};

	float FloorZ = bFloor ? FloorT : 0.0f;

	if (Type == TEXT("room") || Type == TEXT("corridor") || Type == TEXT("stairwell"))
	{
		BuildWalls(Width, Depth, Height, WallT, FVector(0, 0, FloorZ));
	}
	else if (Type == TEXT("l_corridor"))
	{
		// L-shape: horizontal segment + vertical segment
		// Horizontal: width x depth/2, at Y offset
		float HalfD = Depth * 0.5f;
		BuildWalls(Width, HalfD, Height, WallT, FVector(0, -HalfD * 0.5f, FloorZ));
		// Vertical: width/2 x depth/2, at X offset (connecting at corner)
		float HalfW = Width * 0.5f;
		BuildWalls(HalfW, HalfD, Height, WallT, FVector(Width * 0.25f, HalfD * 0.5f, FloorZ));
		// Remove the connecting wall between the two segments
		UDynamicMesh* LCutter = NewObject<UDynamicMesh>(Pool);
		FTransform LCutXf(FRotator::ZeroRotator, FVector(Width * 0.25f, 0, FloorZ), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(LCutter, Opts, LCutXf, HalfW - WallT * 2, WallT + 2.0f, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		FGeometryScriptMeshBooleanOptions BoolOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(Mesh, FTransform::Identity, LCutter, FTransform::Identity,
			EGeometryScriptBooleanOperation::Subtract, BoolOpts);
		bHadBooleans = true;
	}
	else if (Type == TEXT("t_junction"))
	{
		// T-shape: main corridor + perpendicular branch
		BuildWalls(Width, Depth, Height, WallT, FVector(0, 0, FloorZ));
		// Branch going east from center
		float BranchLen = Width * 0.5f;
		float BranchW = Depth * 0.5f;
		BuildWalls(BranchLen, BranchW, Height, WallT, FVector(Width * 0.5f + BranchLen * 0.5f - WallT, 0, FloorZ));
		// Cut opening between main and branch
		UDynamicMesh* TCutter = NewObject<UDynamicMesh>(Pool);
		FTransform TCutXf(FRotator::ZeroRotator, FVector(Width * 0.5f - WallT * 0.5f, 0, FloorZ), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(TCutter, Opts, TCutXf, WallT + 2.0f, BranchW - WallT * 2, Height, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		FGeometryScriptMeshBooleanOptions BoolOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(Mesh, FTransform::Identity, TCutter, FTransform::Identity,
			EGeometryScriptBooleanOperation::Subtract, BoolOpts);
		bHadBooleans = true;
	}

	// Floor slab
	if (bFloor)
	{
		FTransform FloorXf(FRotator::ZeroRotator, FVector(0, 0, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, FloorXf, Width, Depth, FloorT, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	// Ceiling slab
	if (bCeiling)
	{
		FTransform CeilXf(FRotator::ZeroRotator, FVector(0, 0, FloorZ + Height), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, CeilXf, Width, Depth, FloorT, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	// Process openings (door/window boolean subtracts)
	const TArray<TSharedPtr<FJsonValue>>* OpeningsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("openings"), OpeningsArr) && OpeningsArr)
	{
		for (const auto& OpenVal : *OpeningsArr)
		{
			const TSharedPtr<FJsonObject>* OpenObj = nullptr;
			if (!OpenVal->TryGetObject(OpenObj) || !OpenObj || !(*OpenObj).IsValid())
				continue;

			FString Wall;
			(*OpenObj)->TryGetStringField(TEXT("wall"), Wall);
			Wall = Wall.ToLower();

			FString OpenType;
			(*OpenObj)->TryGetStringField(TEXT("type"), OpenType);
			OpenType = OpenType.ToLower();

			float OpenW = (OpenType == TEXT("vent") ? 40.0f : 120.0f);
			if ((*OpenObj)->HasField(TEXT("width")))
			{
				double TempVal;
				if (!(*OpenObj)->TryGetNumberField(TEXT("width"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'width'. Expected number."));
				OpenW = static_cast<float>(TempVal);
			}
			float OpenH = (OpenType == TEXT("door") ? 210.0f : (OpenType == TEXT("vent") ? 30.0f : 100.0f));
			if ((*OpenObj)->HasField(TEXT("height")))
			{
				double TempVal;
				if (!(*OpenObj)->TryGetNumberField(TEXT("height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'height'. Expected number."));
				OpenH = static_cast<float>(TempVal);
			}
			float OffX = 0.0f;
			if ((*OpenObj)->HasField(TEXT("offset_x")))
			{
				double TempVal;
				if (!(*OpenObj)->TryGetNumberField(TEXT("offset_x"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'offset_x'. Expected number."));
				OffX = static_cast<float>(TempVal);
			}
			float OffZ = (OpenType == TEXT("window") ? 100.0f : (OpenType == TEXT("vent") ? 230.0f : 0.0f));
			if ((*OpenObj)->HasField(TEXT("offset_z")))
			{
				double TempVal;
				if (!(*OpenObj)->TryGetNumberField(TEXT("offset_z"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'offset_z'. Expected number."));
				OffZ = static_cast<float>(TempVal);
			}

			// Compute cutter position based on wall
			FVector CutPos;
			float CutBoxW, CutBoxD;

			if (Wall == TEXT("north"))
			{
				CutPos = FVector(OffX, -Depth * 0.5f + WallT * 0.5f, FloorZ + OffZ);
				CutBoxW = OpenW;
				CutBoxD = WallT + 10.0f;
			}
			else if (Wall == TEXT("south"))
			{
				CutPos = FVector(OffX, Depth * 0.5f - WallT * 0.5f, FloorZ + OffZ);
				CutBoxW = OpenW;
				CutBoxD = WallT + 10.0f;
			}
			else if (Wall == TEXT("east"))
			{
				CutPos = FVector(Width * 0.5f - WallT * 0.5f, OffX, FloorZ + OffZ);
				CutBoxW = WallT + 10.0f;
				CutBoxD = OpenW;
			}
			else if (Wall == TEXT("west"))
			{
				CutPos = FVector(-Width * 0.5f + WallT * 0.5f, OffX, FloorZ + OffZ);
				CutBoxW = WallT + 10.0f;
				CutBoxD = OpenW;
			}
			else
			{
				continue; // skip invalid wall
			}

			UDynamicMesh* OpenCutter = NewObject<UDynamicMesh>(Pool);
			FTransform OpenCutXf(FRotator::ZeroRotator, CutPos, FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(OpenCutter, Opts, OpenCutXf, CutBoxW, CutBoxD, OpenH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

			FGeometryScriptMeshBooleanOptions BoolOpts;
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
				Mesh, FTransform::Identity, OpenCutter, FTransform::Identity,
				EGeometryScriptBooleanOperation::Subtract, BoolOpts);
			bHadBooleans = true;

			// Generate trim frame geometry around the opening
			if (bAddTrim)
			{
				// Trim width: 5cm doors, 3cm windows, 2cm vents
				const float TrimW = (OpenType == TEXT("vent")) ? 2.0f : (OpenType == TEXT("window")) ? 3.0f : 5.0f;
				// Trim depth: slightly wider than wall so it protrudes on both sides
				const float TrimD = WallT + 2.0f;
				// Doors get U-frame (no sill), windows/vents get full frame (4 sides)
				const bool bHasSill = (OpenType != TEXT("door"));

				FGeometryScriptPrimitiveOptions TrimOpts;
				TrimOpts.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerFace;
				TrimOpts.MaterialID = 2; // Trim material slot (walls=0, floor/ceiling=0, trim=2)

				UDynamicMesh* TrimMesh = NewObject<UDynamicMesh>(Pool);

				// Axis mapping per wall orientation:
				// N/S walls run along X, trim depth along Y
				// E/W walls run along Y, trim depth along X
				if (Wall == TEXT("north") || Wall == TEXT("south"))
				{
					// Left jamb
					FTransform LeftJambXf(FRotator::ZeroRotator,
						FVector(CutPos.X - OpenW * 0.5f - TrimW * 0.5f, CutPos.Y, CutPos.Z),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, LeftJambXf,
						TrimW, TrimD, OpenH,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Right jamb
					FTransform RightJambXf(FRotator::ZeroRotator,
						FVector(CutPos.X + OpenW * 0.5f + TrimW * 0.5f, CutPos.Y, CutPos.Z),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, RightJambXf,
						TrimW, TrimD, OpenH,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Header (spans full width including both jambs)
					FTransform HeaderXf(FRotator::ZeroRotator,
						FVector(CutPos.X, CutPos.Y, CutPos.Z + OpenH),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, HeaderXf,
						OpenW + TrimW * 2.0f, TrimD, TrimW,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Sill for windows/vents
					if (bHasSill)
					{
						FTransform SillXf(FRotator::ZeroRotator,
							FVector(CutPos.X, CutPos.Y, CutPos.Z - TrimW),
							FVector::OneVector);
						UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
							TrimMesh, TrimOpts, SillXf,
							OpenW + TrimW * 2.0f, TrimD, TrimW,
							0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
					}
				}
				else // east or west — walls run along Y, trim depth along X
				{
					// Left jamb (lower Y)
					FTransform LeftJambXf(FRotator::ZeroRotator,
						FVector(CutPos.X, CutPos.Y - OpenW * 0.5f - TrimW * 0.5f, CutPos.Z),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, LeftJambXf,
						TrimD, TrimW, OpenH,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Right jamb (upper Y)
					FTransform RightJambXf(FRotator::ZeroRotator,
						FVector(CutPos.X, CutPos.Y + OpenW * 0.5f + TrimW * 0.5f, CutPos.Z),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, RightJambXf,
						TrimD, TrimW, OpenH,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Header
					FTransform HeaderXf(FRotator::ZeroRotator,
						FVector(CutPos.X, CutPos.Y, CutPos.Z + OpenH),
						FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						TrimMesh, TrimOpts, HeaderXf,
						TrimD, OpenW + TrimW * 2.0f, TrimW,
						0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

					// Sill for windows/vents
					if (bHasSill)
					{
						FTransform SillXf(FRotator::ZeroRotator,
							FVector(CutPos.X, CutPos.Y, CutPos.Z - TrimW),
							FVector::OneVector);
						UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
							TrimMesh, TrimOpts, SillXf,
							TrimD, OpenW + TrimW * 2.0f, TrimW,
							0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
					}
				}

				// Append trim onto main mesh
				UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
					Mesh, TrimMesh, FTransform::Identity);
			}
		}
	}

	CleanupMesh(Mesh, bHadBooleans);

	// Box UV projection for proper material tiling on architectural surfaces
	// Scale = 100cm per UV tile (1m tiling). Projects from 6 axes, picks best per triangle.
	{
		FGeometryScriptMeshSelection EmptySelection;
		FTransform UVBox = FTransform::Identity;
		UVBox.SetScale3D(FVector(100.0f));
		UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
			Mesh, 0, UVBox, EmptySelection, 2);
	}

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetStringField(TEXT("wall_mode"), WallMode);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetBoolField(TEXT("had_booleans"), bHadBooleans);
	Result->SetBoolField(TEXT("add_trim"), bAddTrim);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, TEXT("structure"),
		TEXT("create_structure"), TEXT("Structure"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 19B: create_building_shell
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateBuildingShell(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	// Parse 2D footprint polygon
	TArray<FVector2D> Footprint;
	if (!ParseVector2DArray(Params, TEXT("footprint"), Footprint) || Footprint.Num() < 3)
	{
		return FMonolithActionResult::Error(TEXT("'footprint' must be an array of at least 3 [x, y] points (CCW winding)"));
	}

	int32 Floors = 1;
	if (Params->HasField(TEXT("floors")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("floors"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'floors'. Expected number."));
		Floors = static_cast<int32>(TempVal);
	}
	float FloorH = 300.0f;
	if (Params->HasField(TEXT("floor_height")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("floor_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'floor_height'. Expected number."));
		FloorH = static_cast<float>(TempVal);
	}
	float WallT = 25.0f;
	if (Params->HasField(TEXT("wall_thickness")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
		WallT = static_cast<float>(TempVal);
	}
	float SlabT = 15.0f;
	if (Params->HasField(TEXT("floor_thickness")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("floor_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'floor_thickness'. Expected number."));
		SlabT = static_cast<float>(TempVal);
	}

	Floors = FMath::Clamp(Floors, 1, 20);

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FGeometryScriptPrimitiveOptions Opts;
	float TotalH = Floors * FloorH;

	// Extrude outer shell (full height)
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(
		Mesh, Opts, FTransform::Identity, Footprint, TotalH, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	// Inset polygon for interior void
	TArray<FVector2D> InnerPoly = InsetPolygon2D(Footprint, WallT);

	if (InnerPoly.Num() >= 3)
	{
		// Create interior cutter (full height, slightly taller to ensure clean cut)
		UDynamicMesh* InnerCutter = NewObject<UDynamicMesh>(Pool);
		FTransform InnerXf(FRotator::ZeroRotator, FVector(0, 0, SlabT), FVector::OneVector); // leave ground floor slab
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(
			InnerCutter, Opts, InnerXf, InnerPoly, TotalH, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

		FGeometryScriptMeshBooleanOptions BoolOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
			Mesh, FTransform::Identity, InnerCutter, FTransform::Identity,
			EGeometryScriptBooleanOperation::Subtract, BoolOpts);
	}

	// Add floor slabs between stories (ground floor is the bottom of the shell)
	for (int32 i = 1; i < Floors; ++i)
	{
		float SlabZ = i * FloorH;
		FTransform SlabXf(FRotator::ZeroRotator, FVector(0, 0, SlabZ), FVector::OneVector);
		// Use inner polygon for floor slabs (they sit inside the walls)
		if (InnerPoly.Num() >= 3)
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(
				Mesh, Opts, SlabXf, InnerPoly, SlabT, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
		}
	}

	// Roof slab (top of building, using outer footprint for full coverage)
	FTransform RoofXf(FRotator::ZeroRotator, FVector(0, 0, TotalH), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(
		Mesh, Opts, RoofXf, Footprint, SlabT, 0, true, EGeometryScriptPrimitiveOriginMode::Base);

	// Optional stairwell cutout (boolean subtract through all floor slabs)
	const TSharedPtr<FJsonObject>* StairObj = nullptr;
	if (Params->TryGetObjectField(TEXT("stairwell_cutout"), StairObj) && StairObj && (*StairObj).IsValid())
	{
		float SX = 0.0f;
		if ((*StairObj)->HasField(TEXT("x")))
		{
			double TempVal;
			if (!(*StairObj)->TryGetNumberField(TEXT("x"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'x'. Expected number."));
			SX = static_cast<float>(TempVal);
		}
		float SY = 0.0f;
		if ((*StairObj)->HasField(TEXT("y")))
		{
			double TempVal;
			if (!(*StairObj)->TryGetNumberField(TEXT("y"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'y'. Expected number."));
			SY = static_cast<float>(TempVal);
		}
		float SW = 100.0f;
		if ((*StairObj)->HasField(TEXT("width")))
		{
			double TempVal;
			if (!(*StairObj)->TryGetNumberField(TEXT("width"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'width'. Expected number."));
			SW = static_cast<float>(TempVal);
		}
		float SD = 100.0f;
		if ((*StairObj)->HasField(TEXT("depth")))
		{
			double TempVal;
			if (!(*StairObj)->TryGetNumberField(TEXT("depth"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'depth'. Expected number."));
			SD = static_cast<float>(TempVal);
		}

		UDynamicMesh* StairCutter = NewObject<UDynamicMesh>(Pool);
		// Cut through all intermediate floors (not the ground floor or roof)
		for (int32 i = 1; i < Floors; ++i)
		{
			float CutZ = i * FloorH - 1.0f; // slight offset below to ensure clean cut
			FTransform CutXf(FRotator::ZeroRotator, FVector(SX, SY, CutZ), FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(StairCutter, Opts, CutXf, SW, SD, SlabT + 2.0f, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
		}

		if (StairCutter->GetTriangleCount() > 0)
		{
			FGeometryScriptMeshBooleanOptions BoolOpts;
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
				Mesh, FTransform::Identity, StairCutter, FTransform::Identity,
				EGeometryScriptBooleanOperation::Subtract, BoolOpts);
		}
	}

	CleanupMesh(Mesh, /*bHadBooleans=*/true);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("floors"), Floors);
	Result->SetNumberField(TEXT("floor_height"), FloorH);
	Result->SetNumberField(TEXT("total_height"), TotalH + SlabT);
	Result->SetNumberField(TEXT("footprint_points"), Footprint.Num());
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, TEXT("building_shell"),
		TEXT("create_building_shell"), TEXT("BuildingShell"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 19B: create_maze — Maze Algorithms
// ============================================================================

// Maze cell grid: cell (x,y) has walls on all 4 sides. Algorithms carve passages by removing walls.
// A wall between (x0,y0) and (x1,y1) is stored if NOT removed (i.e., remaining walls).

// Helper: cell index
static int32 MazeCellIndex(int32 X, int32 Y, int32 GridW) { return Y * GridW + X; }

TArray<FMonolithMeshProceduralActions::FMazeWall> FMonolithMeshProceduralActions::GenerateMaze_RecursiveBacktracker(int32 GridW, int32 GridH, int32 Seed)
{
	FRandomStream Rng(Seed);
	int32 CellCount = GridW * GridH;

	// Track visited cells
	TArray<bool> Visited;
	Visited.SetNumZeroed(CellCount);

	// Track carved passages (set of wall pairs that have been removed)
	TSet<uint64> CarvedWalls;

	auto PackWall = [](int32 X0, int32 Y0, int32 X1, int32 Y1) -> uint64
	{
		// Canonical ordering so (A,B) == (B,A)
		int32 A = FMath::Min(X0 + Y0 * 10000, X1 + Y1 * 10000);
		int32 B = FMath::Max(X0 + Y0 * 10000, X1 + Y1 * 10000);
		return (static_cast<uint64>(A) << 32) | static_cast<uint64>(B);
	};

	// DFS stack
	TArray<FIntPoint> Stack;
	FIntPoint Start(Rng.RandRange(0, GridW - 1), Rng.RandRange(0, GridH - 1));
	Stack.Push(Start);
	Visited[MazeCellIndex(Start.X, Start.Y, GridW)] = true;

	// Neighbor offsets
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	while (Stack.Num() > 0)
	{
		FIntPoint Current = Stack.Last();

		// Gather unvisited neighbors
		TArray<int32, TInlineAllocator<4>> UnvisitedDirs;
		for (int32 d = 0; d < 4; ++d)
		{
			int32 NX = Current.X + Dirs[d].X;
			int32 NY = Current.Y + Dirs[d].Y;
			if (NX >= 0 && NX < GridW && NY >= 0 && NY < GridH && !Visited[MazeCellIndex(NX, NY, GridW)])
			{
				UnvisitedDirs.Add(d);
			}
		}

		if (UnvisitedDirs.Num() == 0)
		{
			Stack.Pop();
			continue;
		}

		int32 ChosenDir = UnvisitedDirs[Rng.RandRange(0, UnvisitedDirs.Num() - 1)];
		FIntPoint Next(Current.X + Dirs[ChosenDir].X, Current.Y + Dirs[ChosenDir].Y);
		Visited[MazeCellIndex(Next.X, Next.Y, GridW)] = true;
		CarvedWalls.Add(PackWall(Current.X, Current.Y, Next.X, Next.Y));
		Stack.Push(Next);
	}

	// Build remaining walls list
	TArray<FMazeWall> Walls;

	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			// Right wall
			if (X < GridW - 1 && !CarvedWalls.Contains(PackWall(X, Y, X + 1, Y)))
			{
				Walls.Add({X, Y, X + 1, Y});
			}
			// Down wall (south)
			if (Y < GridH - 1 && !CarvedWalls.Contains(PackWall(X, Y, X, Y + 1)))
			{
				Walls.Add({X, Y, X, Y + 1});
			}
		}
	}

	return Walls;
}

TArray<FMonolithMeshProceduralActions::FMazeWall> FMonolithMeshProceduralActions::GenerateMaze_Prims(int32 GridW, int32 GridH, int32 Seed)
{
	FRandomStream Rng(Seed);
	int32 CellCount = GridW * GridH;

	TArray<bool> InMaze;
	InMaze.SetNumZeroed(CellCount);

	TSet<uint64> CarvedWalls;

	auto PackWall = [](int32 X0, int32 Y0, int32 X1, int32 Y1) -> uint64
	{
		int32 A = FMath::Min(X0 + Y0 * 10000, X1 + Y1 * 10000);
		int32 B = FMath::Max(X0 + Y0 * 10000, X1 + Y1 * 10000);
		return (static_cast<uint64>(A) << 32) | static_cast<uint64>(B);
	};

	struct FWallCandidate { int32 X0, Y0, X1, Y1; };
	TArray<FWallCandidate> Frontier;

	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	// Start from random cell
	int32 StartX = Rng.RandRange(0, GridW - 1);
	int32 StartY = Rng.RandRange(0, GridH - 1);
	InMaze[MazeCellIndex(StartX, StartY, GridW)] = true;

	// Add frontier walls
	for (const auto& D : Dirs)
	{
		int32 NX = StartX + D.X, NY = StartY + D.Y;
		if (NX >= 0 && NX < GridW && NY >= 0 && NY < GridH)
		{
			Frontier.Add({StartX, StartY, NX, NY});
		}
	}

	while (Frontier.Num() > 0)
	{
		int32 Idx = Rng.RandRange(0, Frontier.Num() - 1);
		FWallCandidate W = Frontier[Idx];
		Frontier.RemoveAtSwap(Idx);

		bool bIn0 = InMaze[MazeCellIndex(W.X0, W.Y0, GridW)];
		bool bIn1 = InMaze[MazeCellIndex(W.X1, W.Y1, GridW)];

		if (bIn0 == bIn1) continue; // both already in maze or both out

		// Carve this wall
		CarvedWalls.Add(PackWall(W.X0, W.Y0, W.X1, W.Y1));

		// The new cell is the one not yet in the maze
		int32 NewX = bIn0 ? W.X1 : W.X0;
		int32 NewY = bIn0 ? W.Y1 : W.Y0;
		InMaze[MazeCellIndex(NewX, NewY, GridW)] = true;

		// Add new frontier walls
		for (const auto& D : Dirs)
		{
			int32 NX = NewX + D.X, NY = NewY + D.Y;
			if (NX >= 0 && NX < GridW && NY >= 0 && NY < GridH && !InMaze[MazeCellIndex(NX, NY, GridW)])
			{
				Frontier.Add({NewX, NewY, NX, NY});
			}
		}
	}

	// Build remaining walls
	TArray<FMazeWall> Walls;
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			if (X < GridW - 1 && !CarvedWalls.Contains(PackWall(X, Y, X + 1, Y)))
				Walls.Add({X, Y, X + 1, Y});
			if (Y < GridH - 1 && !CarvedWalls.Contains(PackWall(X, Y, X, Y + 1)))
				Walls.Add({X, Y, X, Y + 1});
		}
	}

	return Walls;
}

TArray<FMonolithMeshProceduralActions::FMazeWall> FMonolithMeshProceduralActions::GenerateMaze_BinaryTree(int32 GridW, int32 GridH, int32 Seed)
{
	FRandomStream Rng(Seed);

	TSet<uint64> CarvedWalls;

	auto PackWall = [](int32 X0, int32 Y0, int32 X1, int32 Y1) -> uint64
	{
		int32 A = FMath::Min(X0 + Y0 * 10000, X1 + Y1 * 10000);
		int32 B = FMath::Max(X0 + Y0 * 10000, X1 + Y1 * 10000);
		return (static_cast<uint64>(A) << 32) | static_cast<uint64>(B);
	};

	// For each cell, carve either north or west (binary choice)
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			bool bCanGoNorth = (Y > 0);
			bool bCanGoWest = (X > 0);

			if (bCanGoNorth && bCanGoWest)
			{
				if (Rng.FRand() < 0.5f)
					CarvedWalls.Add(PackWall(X, Y, X, Y - 1)); // north
				else
					CarvedWalls.Add(PackWall(X, Y, X - 1, Y)); // west
			}
			else if (bCanGoNorth)
			{
				CarvedWalls.Add(PackWall(X, Y, X, Y - 1));
			}
			else if (bCanGoWest)
			{
				CarvedWalls.Add(PackWall(X, Y, X - 1, Y));
			}
			// (0,0) corner: no carving possible
		}
	}

	// Build remaining walls
	TArray<FMazeWall> Walls;
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			if (X < GridW - 1 && !CarvedWalls.Contains(PackWall(X, Y, X + 1, Y)))
				Walls.Add({X, Y, X + 1, Y});
			if (Y < GridH - 1 && !CarvedWalls.Contains(PackWall(X, Y, X, Y + 1)))
				Walls.Add({X, Y, X, Y + 1});
		}
	}

	return Walls;
}

FMonolithActionResult FMonolithMeshProceduralActions::CreateMaze(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	FString Algorithm;
	if (!Params->TryGetStringField(TEXT("algorithm"), Algorithm))
	{
		Algorithm = TEXT("recursive_backtracker");
	}
	Algorithm = Algorithm.ToLower().TrimStartAndEnd();

	// Parse grid size
	int32 GridW = 8, GridH = 8;
	const TArray<TSharedPtr<FJsonValue>>* GridArr = nullptr;
	if (Params->TryGetArrayField(TEXT("grid_size"), GridArr) && GridArr && GridArr->Num() >= 2)
	{
		GridW = FMath::Clamp(static_cast<int32>((*GridArr)[0]->AsNumber()), 2, 100);
		GridH = FMath::Clamp(static_cast<int32>((*GridArr)[1]->AsNumber()), 2, 100);
	}

	float CellSize = 200.0f;
	if (Params->HasField(TEXT("cell_size")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("cell_size"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'cell_size'. Expected number."));
		CellSize = static_cast<float>(TempVal);
	}
	float WallH = 250.0f;
	if (Params->HasField(TEXT("wall_height")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("wall_height"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_height'. Expected number."));
		WallH = static_cast<float>(TempVal);
	}
	float WallT = 20.0f;
	if (Params->HasField(TEXT("wall_thickness")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("wall_thickness"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'wall_thickness'. Expected number."));
		WallT = static_cast<float>(TempVal);
	}
	int32 Seed = 0;
	if (Params->HasField(TEXT("seed")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("seed"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'seed'. Expected number."));
		Seed = static_cast<int32>(TempVal);
	}
	bool bMerge = false;
	if (Params->HasField(TEXT("merge_walls")) && !Params->TryGetBoolField(TEXT("merge_walls"), bMerge))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'merge_walls'. Expected boolean."));
	}

	// Generate maze walls
	TArray<FMazeWall> InternalWalls;
	if (Algorithm == TEXT("recursive_backtracker"))
	{
		InternalWalls = GenerateMaze_RecursiveBacktracker(GridW, GridH, Seed);
	}
	else if (Algorithm == TEXT("prims"))
	{
		InternalWalls = GenerateMaze_Prims(GridW, GridH, Seed);
	}
	else if (Algorithm == TEXT("binary_tree"))
	{
		InternalWalls = GenerateMaze_BinaryTree(GridW, GridH, Seed);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown maze algorithm '%s'. Valid: recursive_backtracker, prims, binary_tree"), *Algorithm));
	}

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FGeometryScriptPrimitiveOptions Opts;

	float MazeW = GridW * CellSize;
	float MazeD = GridH * CellSize;

	// Outer boundary walls
	// North boundary (Y = 0)
	FTransform NorthBound(FRotator::ZeroRotator, FVector(MazeW * 0.5f, 0, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, NorthBound, MazeW + WallT, WallT, WallH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	// South boundary (Y = MazeD)
	FTransform SouthBound(FRotator::ZeroRotator, FVector(MazeW * 0.5f, MazeD, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, SouthBound, MazeW + WallT, WallT, WallH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	// West boundary (X = 0)
	FTransform WestBound(FRotator::ZeroRotator, FVector(0, MazeD * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, WestBound, WallT, MazeD, WallH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	// East boundary (X = MazeW)
	FTransform EastBound(FRotator::ZeroRotator, FVector(MazeW, MazeD * 0.5f, 0), FVector::OneVector);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, EastBound, WallT, MazeD, WallH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);

	// Internal walls — each wall segment is between two adjacent cells
	for (const auto& W : InternalWalls)
	{
		// Wall is between cell (X0,Y0) and cell (X1,Y1)
		// Find the midpoint of the shared edge
		float MidX, MidY;
		float SegW, SegD;

		if (W.X0 == W.X1)
		{
			// Horizontal wall (between vertically adjacent cells)
			int32 MinY = FMath::Min(W.Y0, W.Y1);
			MidX = (W.X0 + 0.5f) * CellSize;
			MidY = (MinY + 1.0f) * CellSize;
			SegW = CellSize + WallT; // extend to overlap with perpendicular walls
			SegD = WallT;
		}
		else
		{
			// Vertical wall (between horizontally adjacent cells)
			int32 MinX = FMath::Min(W.X0, W.X1);
			MidX = (MinX + 1.0f) * CellSize;
			MidY = (W.Y0 + 0.5f) * CellSize;
			SegW = WallT;
			SegD = CellSize + WallT;
		}

		FTransform WallXf(FRotator::ZeroRotator, FVector(MidX, MidY, 0), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Mesh, Opts, WallXf, SegW, SegD, WallH, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Base);
	}

	// Optional merge (SelfUnion) for clean geometry
	if (bMerge)
	{
		FGeometryScriptMeshSelfUnionOptions SelfUnionOpts;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion(Mesh, SelfUnionOpts);
	}

	// Normals
	FGeometryScriptSplitNormalsOptions SplitOpts;
	SplitOpts.bSplitByOpeningAngle = true;
	SplitOpts.OpeningAngleDeg = 15.0f;
	FGeometryScriptCalculateNormalsOptions CalcOpts;
	UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(Mesh, SplitOpts, CalcOpts);

	int32 TriCount = Mesh->GetTriangleCount();
	int32 WallCount = InternalWalls.Num() + 4; // +4 boundary

	// Build maze layout JSON for AI pathfinding
	TArray<TSharedPtr<FJsonValue>> LayoutValues;
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		TArray<TSharedPtr<FJsonValue>> RowValues;
		for (int32 X = 0; X < GridW; ++X)
		{
			auto Cell = MakeShared<FJsonObject>();
			Cell->SetNumberField(TEXT("x"), X);
			Cell->SetNumberField(TEXT("y"), Y);
			Cell->SetNumberField(TEXT("world_x"), (X + 0.5f) * CellSize);
			Cell->SetNumberField(TEXT("world_y"), (Y + 0.5f) * CellSize);
			RowValues.Add(MakeShared<FJsonValueObject>(Cell));
		}
		LayoutValues.Append(RowValues);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("algorithm"), Algorithm);
	Result->SetNumberField(TEXT("grid_width"), GridW);
	Result->SetNumberField(TEXT("grid_height"), GridH);
	Result->SetNumberField(TEXT("cell_size"), CellSize);
	Result->SetNumberField(TEXT("wall_count"), WallCount);
	Result->SetNumberField(TEXT("internal_wall_count"), InternalWalls.Num());
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetBoolField(TEXT("merged"), bMerge);
	Result->SetArrayField(TEXT("layout"), LayoutValues);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, TEXT("maze"),
		TEXT("create_maze"), TEXT("Maze"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 19C: create_pipe_network
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreatePipeNetwork(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	TArray<FVector> PathPoints;
	if (!ParseVectorArray(Params, TEXT("path_points"), PathPoints) || PathPoints.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("'path_points' must be an array of at least 2 [x, y, z] points"));
	}

	float Radius = 10.0f;
	if (Params->HasField(TEXT("radius")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("radius"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'radius'. Expected number."));
		Radius = static_cast<float>(TempVal);
	}

	int32 Segments = 12;
	if (Params->HasField(TEXT("segments")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("segments"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'segments'. Expected number."));
		Segments = static_cast<int32>(TempVal);
	}

	float MiterLimit = 2.0f;
	if (Params->HasField(TEXT("miter_limit")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("miter_limit"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'miter_limit'. Expected number."));
		MiterLimit = static_cast<float>(TempVal);
	}

	bool bBallJoints = false;
	if (Params->HasField(TEXT("ball_joints")) && !Params->TryGetBoolField(TEXT("ball_joints"), bBallJoints))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'ball_joints'. Expected boolean."));
	}

	float JointScale = 1.3f;
	if (Params->HasField(TEXT("joint_radius_scale")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("joint_radius_scale"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'joint_radius_scale'. Expected number."));
		JointScale = static_cast<float>(TempVal);
	}

	Segments = FMath::Clamp(Segments, 3, 64);

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FGeometryScriptPrimitiveOptions Opts;

	// Build circle profile
	TArray<FVector2D> CircleProfile = MakeCirclePolygon(Radius, Segments);

	// Sweep along path
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleSweptPolygon(
		Mesh, Opts, FTransform::Identity, CircleProfile, PathPoints,
		/*bLoop=*/false, /*bCapped=*/true, /*StartScale=*/1.0f, /*EndScale=*/1.0f,
		/*RotationAngleDeg=*/0.0f, MiterLimit);

	// Optional ball joints at each path point (except endpoints which are capped)
	if (bBallJoints && PathPoints.Num() > 2)
	{
		float JointR = Radius * JointScale;
		for (int32 i = 1; i < PathPoints.Num() - 1; ++i)
		{
			FTransform JointXf(FRotator::ZeroRotator, PathPoints[i], FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
				Mesh, Opts, JointXf, JointR, 6, 8, EGeometryScriptPrimitiveOriginMode::Center);
		}
	}

	// Cleanup
	CleanupMesh(Mesh, /*bHadBooleans=*/false);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("path_point_count"), PathPoints.Num());
	Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetNumberField(TEXT("segments"), Segments);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetBoolField(TEXT("ball_joints"), bBallJoints);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, TEXT("pipe_network"),
		TEXT("create_pipe_network"), TEXT("Pipe"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 19C: create_fragments
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateFragments(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	FString SourceHandle;
	if (!Params->TryGetStringField(TEXT("source_handle"), SourceHandle) || SourceHandle.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: source_handle"));
	}

	FString HandleErr;
	UDynamicMesh* SourceMesh = Pool->GetHandle(SourceHandle, HandleErr);
	if (!SourceMesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Handle '%s' not found: %s"), *SourceHandle, *HandleErr));
	}

	int32 FragCount = 8;
	if (Params->HasField(TEXT("fragment_count")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("fragment_count"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'fragment_count'. Expected number."));
		FragCount = static_cast<int32>(TempVal);
	}

	float Noise = 0.0f;
	if (Params->HasField(TEXT("noise")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("noise"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'noise'. Expected number."));
		Noise = static_cast<float>(TempVal);
	}

	int32 Seed = 0;
	if (Params->HasField(TEXT("seed")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("seed"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'seed'. Expected number."));
		Seed = static_cast<int32>(TempVal);
	}

	float GapWidth = 0.5f;
	if (Params->HasField(TEXT("gap_width")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("gap_width"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'gap_width'. Expected number."));
		GapWidth = static_cast<float>(TempVal);
	}
	FString HandlePrefix;
	if (!Params->TryGetStringField(TEXT("handle_prefix"), HandlePrefix) || HandlePrefix.IsEmpty())
	{
		HandlePrefix = TEXT("frag");
	}

	FragCount = FMath::Clamp(FragCount, 2, 50);

	// Work on a copy of the source mesh (don't modify the original)
	UDynamicMesh* WorkMesh = NewObject<UDynamicMesh>(Pool);
	if (!WorkMesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate working mesh"));
	}
	WorkMesh->SetMesh(SourceMesh->GetMeshRef());

	// Compute bounding box for random plane generation
	UE::Geometry::FAxisAlignedBox3d BBox = WorkMesh->GetMeshRef().GetBounds(true);
	FVector Center = (FVector)BBox.Center();
	FVector Extent = (FVector)BBox.Extents();

	FRandomStream Rng(Seed);

	// Apply N-1 plane slices to create up to N fragments
	int32 NumSlices = FragCount - 1;

	FGeometryScriptMeshPlaneSliceOptions SliceOpts;
	SliceOpts.bFillHoles = true;
	SliceOpts.GapWidth = GapWidth;

	for (int32 i = 0; i < NumSlices; ++i)
	{
		// Random point inside the bounding box (biased toward center)
		FVector RandPoint = Center + FVector(
			Rng.FRandRange(-Extent.X * 0.7f, Extent.X * 0.7f),
			Rng.FRandRange(-Extent.Y * 0.7f, Extent.Y * 0.7f),
			Rng.FRandRange(-Extent.Z * 0.7f, Extent.Z * 0.7f));

		// Random plane normal
		FVector Normal = FVector(
			Rng.FRandRange(-1.0f, 1.0f),
			Rng.FRandRange(-1.0f, 1.0f),
			Rng.FRandRange(-1.0f, 1.0f)).GetSafeNormal();

		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}

		FTransform CutFrame(FRotationMatrix::MakeFromZ(Normal).Rotator(), RandPoint, FVector::OneVector);
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneSlice(WorkMesh, CutFrame, SliceOpts);
	}

	// Optional noise on cut surfaces (apply globally — noise only significantly affects flat cut faces)
	if (Noise > KINDA_SMALL_NUMBER)
	{
		FGeometryScriptPerlinNoiseOptions NoiseOpts;
		NoiseOpts.BaseLayer.Magnitude = Noise * Extent.GetMax() * 0.1f;
		NoiseOpts.BaseLayer.Frequency = 0.05f;
		NoiseOpts.BaseLayer.RandomSeed = Seed + 1000;
		NoiseOpts.bApplyAlongNormal = true;

		FGeometryScriptMeshSelection EmptySelection;
		UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh2(WorkMesh, EmptySelection, NoiseOpts);
	}

	// Split into separate component meshes
	TArray<UDynamicMesh*> ComponentMeshes;
	UGeometryScriptLibrary_MeshDecompositionFunctions::SplitMeshByComponents(
		WorkMesh, ComponentMeshes, /*MeshPool=*/nullptr);

	// Store each fragment as a handle
	TArray<TSharedPtr<FJsonValue>> HandleNames;
	TArray<TSharedPtr<FJsonValue>> TriCounts;

	for (int32 i = 0; i < ComponentMeshes.Num(); ++i)
	{
		FString FragHandle = FString::Printf(TEXT("%s_%d"), *HandlePrefix, i);

		FString CreateErr;
		if (!Pool->CreateHandle(FragHandle, FString::Printf(TEXT("internal:fragment:%s:%d"), *SourceHandle, i), CreateErr))
		{
			// If handle already exists, skip
			continue;
		}

		UDynamicMesh* FragMesh = Pool->GetHandle(FragHandle, CreateErr);
		if (FragMesh && ComponentMeshes[i])
		{
			FragMesh->SetMesh(ComponentMeshes[i]->GetMeshRef());

			// Clean normals on each fragment
			FGeometryScriptSplitNormalsOptions SplitOpts;
			SplitOpts.bSplitByOpeningAngle = true;
			SplitOpts.OpeningAngleDeg = 15.0f;
			FGeometryScriptCalculateNormalsOptions CalcOpts;
			UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(FragMesh, SplitOpts, CalcOpts);

			HandleNames.Add(MakeShared<FJsonValueString>(FragHandle));
			TriCounts.Add(MakeShared<FJsonValueNumber>(FragMesh->GetTriangleCount()));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_handle"), SourceHandle);
	Result->SetNumberField(TEXT("fragment_count"), HandleNames.Num());
	Result->SetNumberField(TEXT("requested_fragments"), FragCount);
	if (HandleNames.Num() != FragCount)
	{
		Result->SetStringField(TEXT("note"), FString::Printf(
			TEXT("Requested %d fragments but got %d — planar slicing produces variable counts depending on geometry. Each slice can create 0-2 new pieces."),
			FragCount, HandleNames.Num()));
	}
	Result->SetNumberField(TEXT("slices_applied"), NumSlices);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("fragment_handles"), HandleNames);
	Result->SetArrayField(TEXT("triangle_counts"), TriCounts);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 19D: create_terrain_patch
// ============================================================================

FMonolithActionResult FMonolithMeshProceduralActions::CreateTerrainPatch(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(GS_ERROR);
	}

	// Parse size
	float SizeX = 2000.0f, SizeY = 2000.0f;
	const TArray<TSharedPtr<FJsonValue>>* SizeArr = nullptr;
	if (Params->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr && SizeArr->Num() >= 2)
	{
		SizeX = static_cast<float>((*SizeArr)[0]->AsNumber());
		SizeY = static_cast<float>((*SizeArr)[1]->AsNumber());
	}

	int32 Resolution = 32;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("resolution")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'resolution' must be a number"));
		Resolution = static_cast<int32>(Val);
	}

	float Amplitude = 100.0f;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("amplitude")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'amplitude' must be a number"));
		Amplitude = static_cast<float>(Val);
	}

	float Frequency = 0.01f;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("frequency")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'frequency' must be a number"));
		Frequency = static_cast<float>(Val);
	}

	int32 Octaves = 4;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("octaves")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'octaves' must be a number"));
		Octaves = static_cast<int32>(Val);
	}

	float Persistence = 0.5f;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("persistence")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'persistence' must be a number"));
		Persistence = static_cast<float>(Val);
	}

	float Lacunarity = 2.0f;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("lacunarity")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'lacunarity' must be a number"));
		Lacunarity = static_cast<float>(Val);
	}

	int32 Seed = 0;
	if (TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("seed")))
	{
		double Val;
		if (!Field->TryGetNumber(Val)) return FMonolithActionResult::Error(TEXT("Parameter 'seed' must be a number"));
		Seed = static_cast<int32>(Val);
	}

	Resolution = FMath::Clamp(Resolution, 2, 256);
	Octaves = FMath::Clamp(Octaves, 1, 8);

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));
	}

	FGeometryScriptPrimitiveOptions Opts;

	// Create subdivided flat box as terrain base
	// Height = very thin (the noise will displace upward)
	float BaseThickness = 1.0f;
	FTransform BaseXf = FTransform::Identity;
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
		Mesh, Opts, BaseXf, SizeX, SizeY, BaseThickness,
		Resolution, Resolution, 0, // XY subdivisions, no Z subdivision needed
		EGeometryScriptPrimitiveOriginMode::Center);

	// Apply multi-octave Perlin noise displacement
	// Each octave: amplitude *= persistence, frequency *= lacunarity
	float CurrentAmp = Amplitude;
	float CurrentFreq = Frequency;

	for (int32 Oct = 0; Oct < Octaves; ++Oct)
	{
		FGeometryScriptPerlinNoiseOptions NoiseOpts;
		NoiseOpts.BaseLayer.Magnitude = CurrentAmp;
		NoiseOpts.BaseLayer.Frequency = CurrentFreq;
		NoiseOpts.BaseLayer.RandomSeed = Seed + Oct * 137; // vary seed per octave
		NoiseOpts.BaseLayer.FrequencyShift = FVector(Oct * 1000.0f, Oct * 1000.0f, 0); // offset each octave
		NoiseOpts.bApplyAlongNormal = true;

		FGeometryScriptMeshSelection EmptySelection;
		UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh2(Mesh, EmptySelection, NoiseOpts);

		CurrentAmp *= Persistence;
		CurrentFreq *= Lacunarity;
	}

	// Recompute normals for proper terrain shading
	FGeometryScriptSplitNormalsOptions SplitOpts;
	SplitOpts.bSplitByOpeningAngle = true;
	SplitOpts.OpeningAngleDeg = 40.0f; // wider angle for smoother terrain
	FGeometryScriptCalculateNormalsOptions CalcOpts;
	UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(Mesh, SplitOpts, CalcOpts);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("size_x"), SizeX);
	Result->SetNumberField(TEXT("size_y"), SizeY);
	Result->SetNumberField(TEXT("resolution"), Resolution);
	Result->SetNumberField(TEXT("octaves"), Octaves);
	Result->SetNumberField(TEXT("amplitude"), Amplitude);
	Result->SetNumberField(TEXT("frequency"), Frequency);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = FinalizeProceduralMesh(Mesh, Params, Result, TEXT("terrain_patch"),
		TEXT("create_terrain_patch"), TEXT("Terrain"));
	if (!FinalErr.IsEmpty())
	{
		return FMonolithActionResult::Error(FinalErr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Cache Management Actions
// ============================================================================

void FMonolithMeshProceduralActions::RegisterCacheActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("list_cached_meshes"),
		TEXT("List all procedural meshes in the cache manifest with metadata (type, path, triangle count, age)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::ListCachedMeshes),
		FParamSchemaBuilder()
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by mesh type (e.g. chair, barricade, room)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max entries to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("clear_cache"),
		TEXT("Clear the procedural mesh cache. Removes manifest entries (assets remain on disk). Optionally filter by type."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::ClearCacheAction),
		FParamSchemaBuilder()
			.Optional(TEXT("type"), TEXT("string"), TEXT("Only clear entries of this type (e.g. chair). Omit to clear all."))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("validate_cache"),
		TEXT("Validate the cache manifest against disk. Removes entries whose assets no longer exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::ValidateCacheAction),
		FParamSchemaBuilder()
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_cache_stats"),
		TEXT("Get cache statistics: entry counts by type, total entries."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshProceduralActions::GetCacheStatsAction),
		FParamSchemaBuilder()
			.Build());
}

int32 FMonolithMeshProceduralActions::ClampListCachedMeshesLimit(int32 Limit)
{
	return FMath::Clamp(Limit, 1, 1000);
}

FMonolithActionResult FMonolithMeshProceduralActions::ListCachedMeshes(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);
	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		double TempVal;
		if (!Params->TryGetNumberField(TEXT("limit"), TempVal)) return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'limit'. Expected number."));
		Limit = static_cast<int32>(TempVal);
	}
	Limit = ClampListCachedMeshesLimit(Limit);

	TSharedPtr<FJsonObject> Result = FMonolithMeshProceduralCache::Get().ListEntries(TypeFilter, Limit);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshProceduralActions::ClearCacheAction(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);

	int32 Removed = FMonolithMeshProceduralCache::Get().ClearCache(TypeFilter);

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("removed_count"), Removed);
	Result->SetStringField(TEXT("filter"), TypeFilter.IsEmpty() ? TEXT("all") : TypeFilter);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshProceduralActions::ValidateCacheAction(const TSharedPtr<FJsonObject>& Params)
{
	int32 Removed = FMonolithMeshProceduralCache::Get().ValidateCache();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("stale_entries_removed"), Removed);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshProceduralActions::GetCacheStatsAction(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = FMonolithMeshProceduralCache::Get().GetStats();
	return FMonolithActionResult::Success(Result);
}

#endif // WITH_GEOMETRYSCRIPT
