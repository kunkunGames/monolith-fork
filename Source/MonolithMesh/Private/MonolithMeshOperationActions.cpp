#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshOperationActions.h"
#include "MonolithMeshHandlePool.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"

#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/CollisionFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using namespace UE::Geometry;

UMonolithMeshHandlePool* FMonolithMeshOperationActions::Pool = nullptr;

namespace
{
	bool ReadVectorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutVector, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values))
		{
			return true;
		}

		if (!Values || Values->Num() < 3)
		{
			OutError = FString::Printf(TEXT("'%s' must be an array [x,y,z]"), FieldName);
			return false;
		}

		OutVector = FVector(
			(*Values)[0]->AsNumber(),
			(*Values)[1]->AsNumber(),
			(*Values)[2]->AsNumber());
		return true;
	}

	bool TryGetPoolAndHandleName(UMonolithMeshHandlePool* InPool, const TSharedPtr<FJsonObject>& Params, FString& OutHandleName, FMonolithActionResult& OutError)
	{
		if (!InPool)
		{
			OutError = FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
			return false;
		}

		if (!Params->TryGetStringField(TEXT("handle"), OutHandleName) || OutHandleName.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(TEXT("\'handle\' is required and must be a string"));
			return false;
		}

		return true;
	}

	UDynamicMesh* GetWorkingMeshForOperation(
		UMonolithMeshHandlePool* Pool,
		const TSharedPtr<FJsonObject>& Params,
		const FString& OperationName,
		FString& OutHandleName,
		FString& OutError)
	{
		FString SourceHandle;
		if (!Params->TryGetStringField(TEXT("handle"), SourceHandle) || SourceHandle.IsEmpty())
		{
			OutError = TEXT("'handle' is required");
			return nullptr;
		}

		FString SourceError;
		UDynamicMesh* SourceMesh = Pool->GetHandle(SourceHandle, SourceError);
		if (!SourceMesh)
		{
			OutError = SourceError;
			return nullptr;
		}

		FString ResultHandle;
		if (Params->TryGetStringField(TEXT("result_handle"), ResultHandle))
		{
			ResultHandle.TrimStartAndEndInline();
		}

		if (!ResultHandle.IsEmpty() && ResultHandle != SourceHandle)
		{
			FString CreateError;
			if (!Pool->CreateHandle(
				ResultHandle,
				FString::Printf(TEXT("internal:%s:%s"), *OperationName, *SourceHandle),
				CreateError))
			{
				OutError = CreateError;
				return nullptr;
			}

			UDynamicMesh* ResultMesh = Pool->GetHandle(ResultHandle, CreateError);
			if (!ResultMesh)
			{
				OutError = CreateError;
				return nullptr;
			}

			ResultMesh->SetMesh(SourceMesh->GetMeshRef());
			OutHandleName = ResultHandle;
			return ResultMesh;
		}

		OutHandleName = SourceHandle;
		return SourceMesh;
	}
}

void FMonolithMeshOperationActions::SetHandlePool(UMonolithMeshHandlePool* InPool)
{
	Pool = InPool;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshOperationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_handle"),
		TEXT("Create a mesh handle from a StaticMesh asset or primitive (box/sphere/cylinder/cone/torus/plane)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::CreateHandle),
		FParamSchemaBuilder()
			.Required(TEXT("source"), TEXT("string"), TEXT("Asset path (e.g. /Game/Meshes/SM_Box) or primitive descriptor (e.g. primitive:box)"))
			.Required(TEXT("handle"), TEXT("string"), TEXT("Name for this handle"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("release_handle"),
		TEXT("Release a mesh handle, freeing memory"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::ReleaseHandle),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle name to release"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("list_handles"),
		TEXT("List all active mesh handles with source, triangle count, and idle time"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::ListHandles),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("save_handle"),
		TEXT("Save a mesh handle as a new StaticMesh asset with auto-generated collision"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::SaveHandle),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle name to save"))
			.RequiredAssetPath(TEXT("target_path"), TEXT("Asset path for the new StaticMesh (e.g. /Game/Meshes/SM_Result)"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow overwriting existing asset"), TEXT("false"))
			.Optional(TEXT("collision"), TEXT("string"), TEXT("Collision mode: auto, box, convex, complex_as_simple, none"), TEXT("auto"))
			.Optional(TEXT("max_hulls"), TEXT("integer"), TEXT("Max convex hulls for decomposition (auto/convex modes)"), TEXT("4"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("mesh_boolean"),
		TEXT("Boolean operation (union/subtract/intersect) between two mesh handles"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::MeshBoolean),
		FParamSchemaBuilder()
			.Required(TEXT("handle_a"), TEXT("string"), TEXT("First mesh handle (target)"))
			.Required(TEXT("handle_b"), TEXT("string"), TEXT("Second mesh handle (tool)"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("Boolean operation: union, subtract, or intersect"))
			.Required(TEXT("result_handle"), TEXT("string"), TEXT("Name for the result handle"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("mesh_simplify"),
		TEXT("Simplify a mesh to a target triangle count or percentage"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::MeshSimplify),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to simplify"))
			.Optional(TEXT("target_triangles"), TEXT("integer"), TEXT("Target triangle count"))
			.Optional(TEXT("target_percentage"), TEXT("number"), TEXT("Target percentage (0.0-1.0) of current triangles"))
			.Optional(TEXT("max_deviation"), TEXT("number"), TEXT("Maximum geometric deviation tolerance"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("mesh_remesh"),
		TEXT("Isotropic remeshing to a target edge length"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::MeshRemesh),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to remesh"))
			.Required(TEXT("target_edge_length"), TEXT("number"), TEXT("Target edge length in cm"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_collision"),
		TEXT("Generate collision shapes for a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GenerateCollision),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to generate collision for"))
			.Optional(TEXT("method"), TEXT("string"), TEXT("Collision method: convex_decomp, auto_box, auto_sphere, auto_capsule, simplified"), TEXT("convex_decomp"))
			.Optional(TEXT("max_hulls"), TEXT("integer"), TEXT("Max convex hulls (for convex_decomp)"), TEXT("4"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_lods"),
		TEXT("Generate LOD chain by repeated simplification"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GenerateLods),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Source handle for LOD0"))
			.Required(TEXT("lod_count"), TEXT("integer"), TEXT("Number of LODs to generate (excluding LOD0)"))
			.Optional(TEXT("reduction_per_lod"), TEXT("number"), TEXT("Triangle reduction ratio per LOD (0.0-1.0)"), TEXT("0.5"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("fill_holes"),
		TEXT("Automatically detect and fill holes in a mesh"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::FillHoles),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to repair"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("compute_uvs"),
		TEXT("Compute UVs using various projection methods"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::ComputeUvs),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to compute UVs for"))
			.Optional(TEXT("method"), TEXT("string"), TEXT("UV method: auto_unwrap, box_project, planar_project, cylinder_project"), TEXT("auto_unwrap"))
			.Optional(TEXT("uv_channel"), TEXT("integer"), TEXT("UV channel index"), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("mirror_mesh"),
		TEXT("Mirror a mesh across an axis"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::MirrorMesh),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to mirror"))
			.Required(TEXT("axis"), TEXT("string"), TEXT("Mirror axis: X, Y, or Z"))
			.Optional(TEXT("weld"), TEXT("boolean"), TEXT("Weld vertices along mirror plane"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("geometry_plane_cut"),
		TEXT("Apply a direct GeometryScript plane cut/slice/mirror to a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GeometryPlaneCut),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to modify"))
			.Optional(TEXT("result_handle"), TEXT("string"), TEXT("Optional output handle; if omitted the input handle is modified in place"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("cut, slice, or mirror"), TEXT("cut"))
			.Optional(TEXT("origin"), TEXT("array"), TEXT("Plane origin [x,y,z]"), TEXT("[0,0,0]"))
			.Optional(TEXT("normal"), TEXT("array"), TEXT("Plane normal [x,y,z]"), TEXT("[0,0,1]"))
			.Optional(TEXT("fill_holes"), TEXT("boolean"), TEXT("Fill cut/slice holes when supported"), TEXT("true"))
			.Optional(TEXT("weld"), TEXT("boolean"), TEXT("Weld along mirror plane"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("geometry_recompute_normals"),
		TEXT("Recompute or reset normals on a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GeometryRecomputeNormals),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to modify"))
			.Optional(TEXT("result_handle"), TEXT("string"), TEXT("Optional output handle; if omitted the input handle is modified in place"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("recompute, split, per_vertex, or per_face"), TEXT("recompute"))
			.Optional(TEXT("split_angle"), TEXT("number"), TEXT("Opening angle in degrees for split mode"), TEXT("15"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("geometry_subdivide"),
		TEXT("Uniform or PN tessellate a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GeometrySubdivide),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to modify"))
			.Optional(TEXT("result_handle"), TEXT("string"), TEXT("Optional output handle; if omitted the input handle is modified in place"))
			.Optional(TEXT("method"), TEXT("string"), TEXT("uniform or pn"), TEXT("uniform"))
			.Optional(TEXT("level"), TEXT("integer"), TEXT("Tessellation level, clamped to 1-5"), TEXT("1"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("geometry_material_ids"),
		TEXT("Inspect or modify per-triangle material IDs on a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GeometryMaterialIds),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to inspect or modify"))
			.Required(TEXT("verb"), TEXT("string"), TEXT("info, remap, clear, or delete_by_id"))
			.Optional(TEXT("result_handle"), TEXT("string"), TEXT("Optional output handle for mutating verbs"))
			.Optional(TEXT("from_id"), TEXT("integer"), TEXT("Source material ID for remap"), TEXT("0"))
			.Optional(TEXT("to_id"), TEXT("integer"), TEXT("Destination material ID for remap"), TEXT("0"))
			.Optional(TEXT("material_id"), TEXT("integer"), TEXT("Material ID for delete_by_id"), TEXT("0"))
			.Optional(TEXT("clear_value"), TEXT("integer"), TEXT("Material ID value for clear"), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("geometry_smooth"),
		TEXT("Apply iterative smoothing to a mesh handle"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshOperationActions::GeometrySmooth),
		FParamSchemaBuilder()
			.Required(TEXT("handle"), TEXT("string"), TEXT("Handle to modify"))
			.Optional(TEXT("result_handle"), TEXT("string"), TEXT("Optional output handle; if omitted the input handle is modified in place"))
			.Optional(TEXT("iterations"), TEXT("integer"), TEXT("Smoothing iterations, clamped to 1-200"), TEXT("10"))
			.Optional(TEXT("speed"), TEXT("number"), TEXT("Smoothing alpha, clamped to 0.0-1.0"), TEXT("0.25"))
			.Build());
}

// ============================================================================
// Handle Management Actions
// ============================================================================

FMonolithActionResult FMonolithMeshOperationActions::CreateHandle(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString Source;
	FString HandleName;

	if (!Params->TryGetStringField(TEXT("source"), Source) || !Params->TryGetStringField(TEXT("handle"), HandleName) || Source.IsEmpty() || HandleName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Both 'source' and 'handle' are required"));
	}

	FString Error;
	if (!Pool->CreateHandle(HandleName, Source, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// Return info about the created handle
	FString GetError;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, GetError);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("source"), Source);
	Result->SetNumberField(TEXT("triangle_count"), Mesh ? Mesh->GetTriangleCount() : 0);
	Result->SetStringField(TEXT("status"), TEXT("created"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::ReleaseHandle(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	if (!Pool->ReleaseHandle(HandleName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Handle '%s' not found"), *HandleName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("status"), TEXT("released"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::ListHandles(const TSharedPtr<FJsonObject>& /*Params*/)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	TSharedPtr<FJsonObject> Result = Pool->ListHandles();
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::SaveHandle(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString HandleName;
	if (!Params->TryGetStringField(TEXT("handle"), HandleName) || HandleName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'handle' is required and must be a string"));
	}

	FString TargetPath;
	if (!Params->TryGetStringField(TEXT("target_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'target_path' is required and must be a string"));
	}

	bool bOverwrite = false;
	if (Params->HasField(TEXT("overwrite")) && !Params->TryGetBoolField(TEXT("overwrite"), bOverwrite))
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'overwrite' must be a boolean"));
	}

	FString CollisionMode = TEXT("auto");
	if (Params->HasField(TEXT("collision")))
	{
		FString InputCollision;
		if (!Params->TryGetStringField(TEXT("collision"), InputCollision))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'collision' must be a string"));
		}
		CollisionMode = InputCollision.ToLower();
	}

	int32 MaxHulls = 4;
	if (Params->HasField(TEXT("max_hulls")))
	{
		double TempMaxHulls;
		if (!Params->TryGetNumberField(TEXT("max_hulls"), TempMaxHulls))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'max_hulls' must be a number"));
		}
		MaxHulls = static_cast<int32>(TempMaxHulls);
		if (MaxHulls < 1 || MaxHulls > 256)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'max_hulls' must be between 1 and 256 (received: %d)"), MaxHulls));
		}
	}

	// Validate collision mode
	static const TSet<FString> ValidModes = { TEXT("auto"), TEXT("convex"), TEXT("box"), TEXT("complex_as_simple"), TEXT("none") };
	if (!ValidModes.Contains(CollisionMode))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid collision mode '%s'. Valid: auto, convex, box, complex_as_simple, none"), *CollisionMode));
	}

	FString Error;
	if (!Pool->SaveHandle(HandleName, TargetPath, bOverwrite, Error, CollisionMode, MaxHulls))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("saved_to"), TargetPath);
	Result->SetStringField(TEXT("collision_mode"), CollisionMode);
	Result->SetNumberField(TEXT("max_hulls"), MaxHulls);
	Result->SetStringField(TEXT("status"), TEXT("saved"));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Mesh Operations
// ============================================================================

FMonolithActionResult FMonolithMeshOperationActions::MeshBoolean(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString HandleA;
	FString HandleB;
	FString Operation;
	FString ResultHandle;

	if (!Params->TryGetStringField(TEXT("handle_a"), HandleA) || HandleA.IsEmpty() ||
		!Params->TryGetStringField(TEXT("handle_b"), HandleB) || HandleB.IsEmpty() ||
		!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty() ||
		!Params->TryGetStringField(TEXT("result_handle"), ResultHandle) || ResultHandle.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("handle_a, handle_b, operation, and result_handle are all required and must be strings"));
	}
	Operation = Operation.ToLower();

	FString ErrorA, ErrorB;
	UDynamicMesh* MeshA = Pool->GetHandle(HandleA, ErrorA);
	UDynamicMesh* MeshB = Pool->GetHandle(HandleB, ErrorB);

	if (!MeshA) return FMonolithActionResult::Error(ErrorA);
	if (!MeshB) return FMonolithActionResult::Error(ErrorB);

	// Map operation string to enum
	EGeometryScriptBooleanOperation BoolOp;
	if (Operation == TEXT("union"))
	{
		BoolOp = EGeometryScriptBooleanOperation::Union;
	}
	else if (Operation == TEXT("subtract"))
	{
		BoolOp = EGeometryScriptBooleanOperation::Subtract;
	}
	else if (Operation == TEXT("intersect"))
	{
		BoolOp = EGeometryScriptBooleanOperation::Intersection;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown boolean operation '%s'. Valid: union, subtract, intersect"), *Operation));
	}

	// Create result handle first (to check name availability)
	FString CreateError;
	if (!Pool->CreateHandle(ResultHandle, FString::Printf(TEXT("internal:boolean:%s(%s,%s)"), *Operation, *HandleA, *HandleB), CreateError))
	{
		return FMonolithActionResult::Error(CreateError);
	}

	UDynamicMesh* ResultMesh = Pool->GetHandle(ResultHandle, CreateError);
	if (!ResultMesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get newly created result handle"));
	}

	// Copy MeshA into result, then boolean with MeshB
	ResultMesh->SetMesh(MeshA->GetMeshRef());

	FGeometryScriptMeshBooleanOptions BoolOpts;
	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		ResultMesh, FTransform::Identity,
		MeshB, FTransform::Identity,
		BoolOp, BoolOpts);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("result_handle"), ResultHandle);
	Result->SetStringField(TEXT("operation"), Operation);
	Result->SetNumberField(TEXT("triangle_count"), ResultMesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::MeshSimplify(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	int32 OriginalTris = Mesh->GetTriangleCount();
	int32 TargetTris = 0;

	if (Params->HasField(TEXT("target_triangles")))
	{
		double TempTargetTris;
		if (!Params->TryGetNumberField(TEXT("target_triangles"), TempTargetTris))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'target_triangles' must be a number"));
		}
		TargetTris = static_cast<int32>(TempTargetTris);
	}
	else if (Params->HasField(TEXT("target_percentage")))
	{
		double Pct;
		if (!Params->TryGetNumberField(TEXT("target_percentage"), Pct))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'target_percentage' must be a number"));
		}
		if (Pct < 0.0 || Pct > 1.0) { return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'target_percentage' must be between 0.0 and 1.0 (received: %f)"), Pct)); }
		TargetTris = FMath::Max(4, FMath::RoundToInt32(OriginalTris * Pct));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Either 'target_triangles' or 'target_percentage' is required"));
	}

	if (Params->HasField(TEXT("max_deviation")))
	{
		double TempTolerance;
		if (!Params->TryGetNumberField(TEXT("max_deviation"), TempTolerance))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'max_deviation' must be a number"));
		}
		float Tolerance = static_cast<float>(TempTolerance);
		FGeometryScriptSimplifyMeshOptions Opts;
		UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTolerance(Mesh, Tolerance, Opts);
	}
	else
	{
		FGeometryScriptSimplifyMeshOptions Opts;
		UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(Mesh, TargetTris, Opts);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetNumberField(TEXT("reduction_ratio"), OriginalTris > 0 ? 1.0 - (double)Mesh->GetTriangleCount() / OriginalTris : 0.0);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::MeshRemesh(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	double TargetEdgeLength;
	if (!Params->TryGetNumberField(TEXT("target_edge_length"), TargetEdgeLength))
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'target_edge_length' is required and must be a number"));
	}
	if (TargetEdgeLength <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("'target_edge_length' must be positive"));
	}

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	int32 OriginalTris = Mesh->GetTriangleCount();

	FGeometryScriptRemeshOptions RemeshOpts;
	FGeometryScriptUniformRemeshOptions UniformOpts;
	UniformOpts.TargetType = EGeometryScriptUniformRemeshTargetType::TargetEdgeLength;
	UniformOpts.TargetEdgeLength = static_cast<float>(TargetEdgeLength);

	UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(Mesh, RemeshOpts, UniformOpts);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetNumberField(TEXT("target_edge_length"), TargetEdgeLength);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GenerateCollision(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	FString Method = TEXT("convex_decomp");
	if (Params->HasField(TEXT("method")))
	{
		FString Tmp;
		if (!Params->TryGetStringField(TEXT("method"), Tmp))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'method'. Expected string."));
		}
		Method = Tmp.ToLower();
	}

	double TempMaxHulls = 4.0;
	if (Params->HasField(TEXT("max_hulls")) && !Params->TryGetNumberField(TEXT("max_hulls"), TempMaxHulls))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'max_hulls'. Expected number."));
	}
	int32 MaxHulls = static_cast<int32>(TempMaxHulls);
	if (MaxHulls < 1 || MaxHulls > 256)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'max_hulls' must be between 1 and 256 (received: %d)"), MaxHulls));
	}

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	FGeometryScriptCollisionFromMeshOptions CollisionOpts;
	CollisionOpts.bEmitTransaction = false;

	if (Method == TEXT("convex_decomp"))
	{
		CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
		CollisionOpts.MaxConvexHullsPerMesh = MaxHulls;
	}
	else if (Method == TEXT("auto_box"))
	{
		CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::AlignedBoxes;
	}
	else if (Method == TEXT("auto_sphere"))
	{
		CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::MinimalSpheres;
	}
	else if (Method == TEXT("auto_capsule"))
	{
		CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::Capsules;
	}
	else if (Method == TEXT("simplified"))
	{
		CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::MinVolumeShapes;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision method '%s'. Valid: convex_decomp, auto_box, auto_sphere, auto_capsule, simplified"), *Method));
	}

	FGeometryScriptSimpleCollision Collision = UGeometryScriptLibrary_CollisionFunctions::GenerateCollisionFromMesh(
		Mesh, CollisionOpts);

	// Store collision on the pool so save_handle can use pre-generated collision instead of re-computing.
	Pool->SetHandleCollision(HandleName, Collision);

	// Report collision shape counts so the user gets useful feedback
	int32 ShapeCount = Collision.AggGeom.BoxElems.Num()
		+ Collision.AggGeom.SphereElems.Num()
		+ Collision.AggGeom.SphylElems.Num()
		+ Collision.AggGeom.ConvexElems.Num();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("method"), Method);
	Result->SetNumberField(TEXT("shape_count"), ShapeCount);
	Result->SetNumberField(TEXT("box_elements"), Collision.AggGeom.BoxElems.Num());
	Result->SetNumberField(TEXT("sphere_elements"), Collision.AggGeom.SphereElems.Num());
	Result->SetNumberField(TEXT("capsule_elements"), Collision.AggGeom.SphylElems.Num());
	Result->SetNumberField(TEXT("convex_elements"), Collision.AggGeom.ConvexElems.Num());
	Result->SetStringField(TEXT("status"), TEXT("generated"));
	Result->SetStringField(TEXT("note"), TEXT("Collision shapes generated and cached. They will be applied when save_handle is called."));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GenerateLods(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	double TempLodCount = 0.0;
	if (!Params->TryGetNumberField(TEXT("lod_count"), TempLodCount))
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'lod_count' is required and must be a number"));
	}
	int32 LodCount = static_cast<int32>(TempLodCount);
	if (LodCount <= 0 || LodCount > 8)
	{
		return FMonolithActionResult::Error(TEXT("'lod_count' must be between 1 and 8"));
	}

	double ReductionPerLod = 0.5;
	if (Params->HasField(TEXT("reduction_per_lod")) && !Params->TryGetNumberField(TEXT("reduction_per_lod"), ReductionPerLod))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'reduction_per_lod'. Expected number."));
	}
	if (ReductionPerLod < 0.1 || ReductionPerLod > 0.9) { return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'reduction_per_lod' must be between 0.1 and 0.9 (received: %f)"), ReductionPerLod)); }

	FString Error;
	UDynamicMesh* SourceMesh = Pool->GetHandle(HandleName, Error);
	if (!SourceMesh) return FMonolithActionResult::Error(Error);

	int32 BaseTris = SourceMesh->GetTriangleCount();
	TArray<TSharedPtr<FJsonValue>> LodArray;
	LodArray.Reserve(LodCount);

	for (int32 Lod = 1; Lod <= LodCount; ++Lod)
	{
		FString LodHandleName = FString::Printf(TEXT("%s_LOD%d"), *HandleName, Lod);

		// Remove existing LOD handle if present
		Pool->ReleaseHandle(LodHandleName);

		// Create LOD handle as copy of source
		FString CreateError;
		if (!Pool->CreateHandle(LodHandleName, FString::Printf(TEXT("internal:lod:%s:%d"), *HandleName, Lod), CreateError))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create LOD handle: %s"), *CreateError));
		}

		UDynamicMesh* LodMesh = Pool->GetHandle(LodHandleName, CreateError);
		if (!LodMesh)
		{
			return FMonolithActionResult::Error(TEXT("Failed to get LOD handle"));
		}

		// Copy source mesh data
		LodMesh->SetMesh(SourceMesh->GetMeshRef());

		// Simplify progressively
		int32 TargetTris = FMath::Max(4, FMath::RoundToInt32(BaseTris * FMath::Pow(ReductionPerLod, Lod)));

		FGeometryScriptSimplifyMeshOptions SimplifyOpts;
		UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(LodMesh, TargetTris, SimplifyOpts);

		TSharedPtr<FJsonObject> LodInfo = MakeShared<FJsonObject>();
		LodInfo->SetStringField(TEXT("handle"), LodHandleName);
		LodInfo->SetNumberField(TEXT("lod_level"), Lod);
		LodInfo->SetNumberField(TEXT("target_triangles"), TargetTris);
		LodInfo->SetNumberField(TEXT("actual_triangles"), LodMesh->GetTriangleCount());
		LodArray.Add(MakeShared<FJsonValueObject>(LodInfo));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_handle"), HandleName);
	Result->SetNumberField(TEXT("source_triangles"), BaseTris);
	Result->SetNumberField(TEXT("lod_count"), LodCount);
	Result->SetNumberField(TEXT("reduction_per_lod"), ReductionPerLod);
	Result->SetArrayField(TEXT("lods"), LodArray);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::FillHoles(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	int32 OriginalTris = Mesh->GetTriangleCount();

	FGeometryScriptFillHolesOptions FillOpts;
	FillOpts.FillMethod = EGeometryScriptFillHolesMethod::Automatic;
	int32 NumFilledHoles = 0;
	int32 NumFailedHoleFills = 0;

	UGeometryScriptLibrary_MeshRepairFunctions::FillAllMeshHoles(
		Mesh, FillOpts, NumFilledHoles, NumFailedHoleFills);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetNumberField(TEXT("holes_filled"), NumFilledHoles);
	Result->SetNumberField(TEXT("holes_failed"), NumFailedHoleFills);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::ComputeUvs(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	FString Method = TEXT("auto_unwrap");
	if (Params->HasField(TEXT("method")))
	{
		FString Tmp;
		if (!Params->TryGetStringField(TEXT("method"), Tmp))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'method'. Expected string."));
		}
		Method = Tmp.ToLower();
	}

	double TempUVChannel = 0.0;
	if (Params->HasField(TEXT("uv_channel")) && !Params->TryGetNumberField(TEXT("uv_channel"), TempUVChannel))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'uv_channel'. Expected number."));
	}
	int32 UVChannel = static_cast<int32>(TempUVChannel);

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	FGeometryScriptMeshSelection EmptySelection; // No selection = operate on whole mesh

	if (Method == TEXT("auto_unwrap"))
	{
		FGeometryScriptXAtlasOptions XAtlasOpts;
		UGeometryScriptLibrary_MeshUVFunctions::AutoGenerateXAtlasMeshUVs(Mesh, UVChannel, XAtlasOpts);
	}
	else if (Method == TEXT("box_project"))
	{
		UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
			Mesh, UVChannel, FTransform::Identity, EmptySelection);
	}
	else if (Method == TEXT("planar_project"))
	{
		UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
			Mesh, UVChannel, FTransform::Identity, EmptySelection);
	}
	else if (Method == TEXT("cylinder_project"))
	{
		UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromCylinderProjection(
			Mesh, UVChannel, FTransform::Identity, EmptySelection);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown UV method '%s'. Valid: auto_unwrap, box_project, planar_project, cylinder_project"), *Method));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("method"), Method);
	Result->SetNumberField(TEXT("uv_channel"), UVChannel);
	Result->SetStringField(TEXT("status"), TEXT("computed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::MirrorMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleName;
	FMonolithActionResult ErrorResult;
	if (!TryGetPoolAndHandleName(Pool, Params, HandleName, ErrorResult))
	{
		return ErrorResult;
	}

	FString Axis;
	if (!Params->TryGetStringField(TEXT("axis"), Axis))
	{
		return FMonolithActionResult::Error(TEXT("'axis' must be a string (X, Y, or Z)"));
	}
	Axis = Axis.ToUpper();
	if (Axis != TEXT("X") && Axis != TEXT("Y") && Axis != TEXT("Z"))
	{
		return FMonolithActionResult::Error(TEXT("'axis' must be X, Y, or Z"));
	}

	bool bWeld = true;
	if (Params->HasField(TEXT("weld")) && !Params->TryGetBoolField(TEXT("weld"), bWeld))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'weld'. Expected boolean."));
	}

	FString Error;
	UDynamicMesh* Mesh = Pool->GetHandle(HandleName, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);

	int32 OriginalTris = Mesh->GetTriangleCount();

	// Build mirror transform: mirror plane normal determines the mirror axis
	// ApplyMeshMirror mirrors across a plane defined by MirrorFrame
	// The plane normal is the local X axis of the frame by convention
	FTransform MirrorFrame = FTransform::Identity;
	if (Axis == TEXT("X"))
	{
		// Mirror plane has normal along X - identity frame works (X-axis is forward)
		MirrorFrame = FTransform::Identity;
	}
	else if (Axis == TEXT("Y"))
	{
		// Rotate so the plane normal (local X) points along world Y
		MirrorFrame.SetRotation(FQuat(FVector::UpVector, FMath::DegreesToRadians(90.0f)));
	}
	else if (Axis == TEXT("Z"))
	{
		// Rotate so the plane normal (local X) points along world Z
		MirrorFrame.SetRotation(FQuat(FVector::RightVector, FMath::DegreesToRadians(-90.0f)));
	}

	FGeometryScriptMeshMirrorOptions MirrorOpts;
	MirrorOpts.bApplyPlaneCut = true;
	MirrorOpts.bWeldAlongPlane = bWeld;

	UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshMirror(
		Mesh, MirrorFrame, MirrorOpts);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), HandleName);
	Result->SetStringField(TEXT("axis"), Axis);
	Result->SetBoolField(TEXT("weld"), bWeld);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GeometryPlaneCut(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString Mode = TEXT("cut");
	if (Params->HasField(TEXT("mode")))
	{
		FString Tmp;
		if (!Params->TryGetStringField(TEXT("mode"), Tmp))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'mode'. Expected string."));
		}
		Mode = Tmp.ToLower();
	}
	if (Mode != TEXT("cut") && Mode != TEXT("slice") && Mode != TEXT("mirror"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown mode '%s'. Valid: cut, slice, mirror"), *Mode));
	}

	FString Error;
	FVector Origin = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	if (!ReadVectorArray(Params, TEXT("origin"), Origin, Error) ||
		!ReadVectorArray(Params, TEXT("normal"), Normal, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	Normal = Normal.GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		return FMonolithActionResult::Error(TEXT("'normal' must not be zero length"));
	}

	bool bFillHoles = true;
	if (Params->HasField(TEXT("fill_holes")) && !Params->TryGetBoolField(TEXT("fill_holes"), bFillHoles))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'fill_holes'. Expected boolean."));
	}
	bool bWeld = true;
	if (Params->HasField(TEXT("weld")) && !Params->TryGetBoolField(TEXT("weld"), bWeld))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'weld'. Expected boolean."));
	}

	FString WorkingHandle;
	UDynamicMesh* Mesh = GetWorkingMeshForOperation(Pool, Params, TEXT("geometry_plane_cut"), WorkingHandle, Error);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(Error);
	}
	const int32 OriginalTris = Mesh->GetTriangleCount();

	FTransform CutFrame(FQuat::FindBetweenNormals(FVector::UpVector, Normal), Origin);
	if (Mode == TEXT("cut"))
	{
		FGeometryScriptMeshPlaneCutOptions CutOptions;
		CutOptions.bFillHoles = bFillHoles;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneCut(Mesh, CutFrame, CutOptions);
	}
	else if (Mode == TEXT("slice"))
	{
		FGeometryScriptMeshPlaneSliceOptions SliceOptions;
		SliceOptions.bFillHoles = bFillHoles;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneSlice(Mesh, CutFrame, SliceOptions);
	}
	else if (Mode == TEXT("mirror"))
	{
		FGeometryScriptMeshMirrorOptions MirrorOptions;
		MirrorOptions.bApplyPlaneCut = true;
		MirrorOptions.bWeldAlongPlane = bWeld;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshMirror(Mesh, CutFrame, MirrorOptions);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), WorkingHandle);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetBoolField(TEXT("fill_holes"), bFillHoles);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GeometryRecomputeNormals(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString Mode = TEXT("recompute");
	if (Params->HasField(TEXT("mode")))
	{
		FString Tmp;
		if (!Params->TryGetStringField(TEXT("mode"), Tmp))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'mode'. Expected string."));
		}
		Mode = Tmp.ToLower();
	}
	if (Mode != TEXT("recompute") && Mode != TEXT("split") && Mode != TEXT("per_vertex") && Mode != TEXT("per_face"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown normals mode '%s'. Valid: recompute, split, per_vertex, per_face"), *Mode));
	}

	double TempAngle = 15.0;
	if (Mode == TEXT("split") && Params->HasField(TEXT("split_angle")) && !Params->TryGetNumberField(TEXT("split_angle"), TempAngle))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'split_angle'. Expected number."));
	}

	FString WorkingHandle;
	FString Error;
	UDynamicMesh* Mesh = GetWorkingMeshForOperation(Pool, Params, TEXT("geometry_recompute_normals"), WorkingHandle, Error);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (Mode == TEXT("recompute"))
	{
		FGeometryScriptCalculateNormalsOptions NormalOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(Mesh, NormalOptions);
	}
	else if (Mode == TEXT("split"))
	{
		FGeometryScriptSplitNormalsOptions SplitOptions;
		SplitOptions.OpeningAngleDeg = static_cast<float>(TempAngle);
		FGeometryScriptCalculateNormalsOptions NormalOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(Mesh, SplitOptions, NormalOptions);
	}
	else if (Mode == TEXT("per_vertex"))
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::SetPerVertexNormals(Mesh);
	}
	else if (Mode == TEXT("per_face"))
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::SetPerFaceNormals(Mesh);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), WorkingHandle);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetNumberField(TEXT("triangle_count"), Mesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GeometrySubdivide(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString Method = TEXT("uniform");
	if (Params->HasField(TEXT("method")))
	{
		FString Tmp;
		if (!Params->TryGetStringField(TEXT("method"), Tmp))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'method'. Expected string."));
		}
		Method = Tmp.ToLower();
	}
	if (Method != TEXT("uniform") && Method != TEXT("pn"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown subdivide method '%s'. Valid: uniform, pn"), *Method));
	}

	double TempLevel = 1.0;
	if (Params->HasField(TEXT("level")) && !Params->TryGetNumberField(TEXT("level"), TempLevel))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'level'. Expected number."));
	}
	const int32 Level = FMath::Clamp(static_cast<int32>(TempLevel), 1, 5);

	FString WorkingHandle;
	FString Error;
	UDynamicMesh* Mesh = GetWorkingMeshForOperation(Pool, Params, TEXT("geometry_subdivide"), WorkingHandle, Error);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(Error);
	}
	const int32 OriginalTris = Mesh->GetTriangleCount();

	if (Method == TEXT("uniform"))
	{
		UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyUniformTessellation(Mesh, Level);
	}
	else if (Method == TEXT("pn"))
	{
		FGeometryScriptPNTessellateOptions Options;
		UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(Mesh, Options, Level);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), WorkingHandle);
	Result->SetStringField(TEXT("method"), Method);
	Result->SetNumberField(TEXT("level"), Level);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GeometryMaterialIds(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	FString Verb;
	if (!Params->TryGetStringField(TEXT("verb"), Verb) || Verb.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'verb' is required and must be a string"));
	}
	Verb = Verb.ToLower();
	if (Verb != TEXT("info") && Verb != TEXT("remap") && Verb != TEXT("clear") && Verb != TEXT("delete_by_id"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown material ID verb '%s'. Valid: info, remap, clear, delete_by_id"), *Verb));
	}

	int32 FromId = 0;
	int32 ToId = 0;
	int32 ClearValue = 0;
	int32 MaterialId = 0;
	if (Verb == TEXT("remap"))
	{
		double TempFromId = 0.0;
		if (Params->HasField(TEXT("from_id")) && !Params->TryGetNumberField(TEXT("from_id"), TempFromId))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'from_id'. Expected number."));
		}
		FromId = static_cast<int32>(TempFromId);

		double TempToId = 0.0;
		if (Params->HasField(TEXT("to_id")) && !Params->TryGetNumberField(TEXT("to_id"), TempToId))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'to_id'. Expected number."));
		}
		ToId = static_cast<int32>(TempToId);
	}
	else if (Verb == TEXT("clear"))
	{
		double TempClearValue = 0.0;
		if (Params->HasField(TEXT("clear_value")) && !Params->TryGetNumberField(TEXT("clear_value"), TempClearValue))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'clear_value'. Expected number."));
		}
		ClearValue = static_cast<int32>(TempClearValue);
	}
	else if (Verb == TEXT("delete_by_id"))
	{
		double TempMaterialId = 0.0;
		if (Params->HasField(TEXT("material_id")) && !Params->TryGetNumberField(TEXT("material_id"), TempMaterialId))
		{
			return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'material_id'. Expected number."));
		}
		MaterialId = static_cast<int32>(TempMaterialId);
	}

	FString WorkingHandle;
	FString Error;
	UDynamicMesh* Mesh = nullptr;

	if (Verb == TEXT("info"))
	{
		FString HandleName;
		if (!Params->TryGetStringField(TEXT("handle"), HandleName) || HandleName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("\'handle\' is required and must be a string"));
		}

		Mesh = Pool->GetHandle(HandleName, Error);
		WorkingHandle = HandleName;
		if (!Mesh)
		{
			return FMonolithActionResult::Error(Error);
		}
	}
	else
	{
		Mesh = GetWorkingMeshForOperation(Pool, Params, TEXT("geometry_material_ids"), WorkingHandle, Error);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	const int32 OriginalTris = Mesh->GetTriangleCount();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), WorkingHandle);
	Result->SetStringField(TEXT("verb"), Verb);

	if (Verb == TEXT("info"))
	{
		bool bHasMaterialIds = false;
		const int32 MaxId = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Mesh, bHasMaterialIds);
		Result->SetBoolField(TEXT("has_material_ids"), bHasMaterialIds);
		Result->SetNumberField(TEXT("max_material_id"), MaxId);
	}
	else if (Verb == TEXT("remap"))
	{
		UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(Mesh, FromId, ToId);
		Result->SetNumberField(TEXT("from_id"), FromId);
		Result->SetNumberField(TEXT("to_id"), ToId);
	}
	else if (Verb == TEXT("clear"))
	{
		UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(Mesh, ClearValue);
		Result->SetNumberField(TEXT("clear_value"), ClearValue);
	}
	else if (Verb == TEXT("delete_by_id"))
	{
		int32 NumDeleted = 0;
		UGeometryScriptLibrary_MeshMaterialFunctions::DeleteTrianglesByMaterialID(Mesh, MaterialId, NumDeleted);
		Result->SetNumberField(TEXT("material_id"), MaterialId);
		Result->SetNumberField(TEXT("deleted_triangles"), NumDeleted);
	}

	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshOperationActions::GeometrySmooth(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("Enable the GeometryScripting plugin in your .uproject to use mesh operations."));
	}

	double TempIterations = 10.0;
	if (Params->HasField(TEXT("iterations")) && !Params->TryGetNumberField(TEXT("iterations"), TempIterations))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'iterations'. Expected number."));
	}
	int32 Iterations = static_cast<int32>(TempIterations);
	if (Iterations < 1 || Iterations > 200) { return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'iterations' must be between 1 and 200 (received: %d)"), Iterations)); }

	double TempSpeed = 0.25;
	if (Params->HasField(TEXT("speed")) && !Params->TryGetNumberField(TEXT("speed"), TempSpeed))
	{
		return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'speed'. Expected number."));
	}
	float Speed = static_cast<float>(TempSpeed);
	if (Speed < 0.0f || Speed > 1.0f) { return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter 'speed' must be between 0.0 and 1.0 (received: %f)"), Speed)); }

	FString WorkingHandle;
	FString Error;
	UDynamicMesh* Mesh = GetWorkingMeshForOperation(Pool, Params, TEXT("geometry_smooth"), WorkingHandle, Error);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(Error);
	}

	const int32 OriginalTris = Mesh->GetTriangleCount();

	FGeometryScriptIterativeMeshSmoothingOptions Options;
	Options.NumIterations = Iterations;
	Options.Alpha = Speed;

	FGeometryScriptMeshSelection EmptySelection;
	UGeometryScriptLibrary_MeshDeformFunctions::ApplyIterativeSmoothingToMesh(Mesh, EmptySelection, Options);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("handle"), WorkingHandle);
	Result->SetNumberField(TEXT("iterations"), Iterations);
	Result->SetNumberField(TEXT("speed"), Speed);
	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("result_triangles"), Mesh->GetTriangleCount());
	Result->SetStringField(TEXT("status"), TEXT("completed"));

	return FMonolithActionResult::Success(Result);
}

#endif // WITH_GEOMETRYSCRIPT
