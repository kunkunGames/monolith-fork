// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorPreviewActions.cpp
//
// Phase 3 of plan: 2026-05-26-monolith-editor-preview-expansion.md.
//
// Two composite-capture editor:: actions, both producing PNG output:
//
//   editor::capture_material_grid   — render N materials side-by-side on
//                                      identical preview meshes in ONE scene,
//                                      ONE camera, ONE PNG. Shares lighting +
//                                      HDRI across all cells.
//   editor::capture_with_overlay    — render a static mesh with one of five
//                                      FEngineShowFlags overlays toggled on
//                                      (wireframe / normals / uv_density /
//                                      lightmap_density / shader_complexity).
//
// Both reuse the proven render-target + scene-capture pipeline from
// MonolithEditorActions.cpp::RenderAndSaveCapture. Declarations live in the
// public MonolithEditorActions.h header (Phase 3 block); registrations live
// in MonolithEditorActions.cpp::RegisterActions.
//
// UE 5.7 show-flag verification (offline source_query, plan Section 4 row
// "FEngineShowFlags"):
//   SetWireframe              — Engine/Source/Runtime/Engine/Public/ShowFlags.h
//   SetMeshEdges              — Engine/Source/Runtime/Engine/Public/ShowFlags.h:461
//   SetMeshUVDensityAccuracy  — Engine/Source/Runtime/Engine/Public/ShowFlags.h:501
//   SetLightMapDensity        — Engine/Source/Runtime/Engine/Public/ShowFlags.h:441
//   SetShaderComplexity       — Engine/Source/Runtime/Engine/Public/ShowFlags.h:431
//   (SetVisualizeMeshNormals does NOT exist in UE 5.7 — `normals` mode falls
//   back to SetMeshEdges which renders edges as a wireframe overlay. The
//   limitation is documented in the action description.)
// =============================================================================

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithAssetUtils.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

#include "AdvancedPreviewScene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "ShowFlags.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPreviewActions, Log, All);

// ----- Local helpers ---------------------------------------------------------

namespace
{
	/** Resolve "plane" / "sphere" / "cube" to engine BasicShapes path. */
	static FString ResolvePreviewMeshPath(const FString& Kind)
	{
		if (Kind.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Engine/BasicShapes/Sphere");
		}
		if (Kind.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Engine/BasicShapes/Cube");
		}
		return TEXT("/Engine/BasicShapes/Plane");
	}

	/** Parse optional {location, rotation, fov} camera object (or string-serialized variant). */
	static bool ParseCameraObject(
		const TSharedPtr<FJsonObject>& Params,
		FVector& OutLocation,
		FRotator& OutRotation,
		float& OutFOV,
		bool& bOutCameraProvided,
		FString& OutError)
	{
		bOutCameraProvided = false;
		if (!Params->HasField(TEXT("camera")))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;

		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr;
			if (Params->TryGetStringField(TEXT("camera"), CameraStr) && !CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				CameraObj = &ParsedCamera;
			}
		}

		if (!CameraObj || !(*CameraObj).IsValid())
		{
			OutError = TEXT("Invalid param: 'camera' must be an object or JSON string");
			return false;
		}

		bool bHasCameraTransform = false;

		if ((*CameraObj)->HasField(TEXT("location")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
			if (!(*CameraObj)->TryGetArrayField(TEXT("location"), Loc) || !Loc || Loc->Num() < 3)
			{
				OutError = TEXT("Invalid param: 'camera.location' must be an array with at least 3 numbers");
				return false;
			}
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!(*Loc)[0]->TryGetNumber(X) || !(*Loc)[1]->TryGetNumber(Y) || !(*Loc)[2]->TryGetNumber(Z))
			{
				OutError = TEXT("Invalid param: 'camera.location' elements must be numbers");
				return false;
			}
			OutLocation = FVector(X, Y, Z);
			bHasCameraTransform = true;
		}
		if ((*CameraObj)->HasField(TEXT("rotation")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
			if (!(*CameraObj)->TryGetArrayField(TEXT("rotation"), Rot) || !Rot || Rot->Num() < 3)
			{
				OutError = TEXT("Invalid param: 'camera.rotation' must be an array with at least 3 numbers");
				return false;
			}
			double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
			if (!(*Rot)[0]->TryGetNumber(Pitch) || !(*Rot)[1]->TryGetNumber(Yaw) || !(*Rot)[2]->TryGetNumber(Roll))
			{
				OutError = TEXT("Invalid param: 'camera.rotation' elements must be numbers");
				return false;
			}
			OutRotation = FRotator(Pitch, Yaw, Roll);
			bHasCameraTransform = true;
		}
		if ((*CameraObj)->HasField(TEXT("fov")))
		{
			double TempFOV = 0.0;
			if (!(*CameraObj)->TryGetNumberField(TEXT("fov"), TempFOV))
			{
				OutError = TEXT("Invalid param: 'camera.fov' must be a number");
				return false;
			}
			OutFOV = (float)TempFOV;
		}

		bOutCameraProvided = bHasCameraTransform;
		return true;
	}

	/** Parse optional [w, h] array; absent field keeps defaults, malformed field returns false. */
	static bool ParseResolutionArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32& OutW,
		int32& OutH,
		FString& OutError)
	{
		if (!Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			OutError = FString::Printf(TEXT("Invalid param: '%s' must be an array"), FieldName);
			return false;
		}
		if (Arr->Num() < 2)
		{
			OutError = FString::Printf(TEXT("Invalid param: '%s' must contain width and height"), FieldName);
			return false;
		}
		double W = 0.0, H = 0.0;
		if (!(*Arr)[0]->TryGetNumber(W) || !(*Arr)[1]->TryGetNumber(H))
		{
			OutError = FString::Printf(TEXT("Invalid param: '%s' elements must be numbers"), FieldName);
			return false;
		}
		OutW = FMath::Max(1, (int32)W);
		OutH = FMath::Max(1, (int32)H);
		return true;
	}

	/** Build default timestamp-suffixed output path under Saved/Screenshots/Monolith/<Bucket>/. */
	static FString DefaultOutputPath(const TCHAR* Bucket)
	{
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		return FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") / Bucket /
			FString::Printf(TEXT("%s.png"), *Timestamp);
	}

	/** Resolve user-supplied or default output path, normalizing relative paths. */
	static bool ResolveOutputPath(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* DefaultBucket,
		FString& OutPath,
		FString& OutError)
	{
		if (Params->HasField(TEXT("output_path")))
		{
			FString OutputPath;
			if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
			{
				OutError = TEXT("Invalid param: 'output_path' must be a non-empty string");
				return false;
			}
			if (FPaths::IsRelative(OutputPath))
			{
				OutputPath = FPaths::ProjectDir() / OutputPath;
			}
			OutPath = OutputPath;
			return true;
		}
		OutPath = DefaultOutputPath(DefaultBucket);
		return true;
	}
}

// =============================================================================
// editor::capture_material_grid
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleCaptureMaterialGrid(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params object is null"));
	}

	// material_paths array — required.
	const TArray<TSharedPtr<FJsonValue>>* MaterialPathsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("material_paths"), MaterialPathsArr) || !MaterialPathsArr)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array is required"));
	}
	if (MaterialPathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array is empty"));
	}
	if (MaterialPathsArr->Num() > 16)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array exceeds 16-entry limit"));
	}

	// Resolve materials — log + skip any that fail to load.
	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(MaterialPathsArr->Num());
	for (const TSharedPtr<FJsonValue>& Val : *MaterialPathsArr)
	{
		if (!Val.IsValid())
		{
			continue;
		}
		const FString Path = Val->AsString();
		if (Path.IsEmpty())
		{
			continue;
		}
		UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(Path);
		if (!Material)
		{
			UE_LOG(LogMonolithPreviewActions, Warning,
				TEXT("capture_material_grid: failed to load material '%s' — cell will be empty"), *Path);
			continue;
		}
		Materials.Add(Material);
	}

	if (Materials.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("No materials successfully loaded from material_paths"));
	}

	// resolution — default 1024x1024.
	int32 ResX = 1024, ResY = 1024;
	FString ParamError;
	if (!ParseResolutionArray(Params, TEXT("resolution"), ResX, ResY, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	// columns — default ceil(sqrt(N)) per resolved open question #5.
	const int32 MaterialCount = Materials.Num();
	int32 Columns = (int32)FMath::CeilToInt32(FMath::Sqrt((float)MaterialCount));
	if (Params->HasField(TEXT("columns")))
	{
		double ColsD = 0.0;
		if (!Params->TryGetNumberField(TEXT("columns"), ColsD))
		{
			return FMonolithActionResult::Error(
				TEXT("Invalid param: 'columns' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Columns = FMath::Max(1, (int32)ColsD);
	}
	const int32 Rows = (int32)FMath::CeilToInt32((float)MaterialCount / (float)Columns);

	// preview_mesh — default "sphere" for the grid (better material readout than plane).
	FString PreviewMeshKind = TEXT("sphere");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		if (!Params->TryGetStringField(TEXT("preview_mesh"), PreviewMeshKind))
		{
			return FMonolithActionResult::Error(
				TEXT("Invalid param: 'preview_mesh' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	const FString PreviewMeshPath = ResolvePreviewMeshPath(PreviewMeshKind);

	UStaticMesh* PreviewMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(PreviewMeshPath);
	if (!PreviewMesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load preview mesh: %s"), *PreviewMeshPath));
	}

	// Per-cell resolution (informational; the grid is one big capture, not stitched).
	const int32 CellW = FMath::Max(1, ResX / Columns);
	const int32 CellH = FMath::Max(1, ResY / Rows);

	// Grid layout in world space. 200 cm centre-to-centre — works for the
	// 100 cm engine sphere/cube/plane at unit scale with margin between cells.
	const float CellSpacing = 200.0f;
	const float TotalGridW = CellSpacing * (Columns - 1);
	const float TotalGridH = CellSpacing * (Rows - 1);
	const FVector GridOriginOffset(0.0f, -TotalGridW * 0.5f, TotalGridH * 0.5f);

	// Camera default: frame the whole grid from -X looking +X.
	// Distance derived from grid extents + FOV (60 deg default) so all cells fit.
	float FOV = 60.0f;
	FVector CameraLocation(0.0f, 0.0f, 0.0f);
	FRotator CameraRotation(0.0f, 0.0f, 0.0f);
	bool bCameraProvided = false;
	if (!ParseCameraObject(Params, CameraLocation, CameraRotation, FOV, bCameraProvided, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!bCameraProvided)
	{
		// Auto-frame: required half-extent is max(width, height) / 2 + cell radius (~100).
		const float HalfExtent = FMath::Max(TotalGridW, TotalGridH) * 0.5f + 120.0f;
		const float HalfFOVRad = FMath::DegreesToRadians(FOV * 0.5f);
		const float Distance = HalfExtent / FMath::Tan(HalfFOVRad);
		CameraLocation = FVector(-FMath::Max(300.0f, Distance + 100.0f), 0.0f, 0.0f);
		CameraRotation = FRotator(0.0f, 0.0f, 0.0f); // looking +X
	}

	// Output path.
	FString OutputPath;
	if (!ResolveOutputPath(Params, TEXT("CaptureMaterialGrid"), OutputPath, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();

	// One shared preview scene — the value-add: identical lighting + HDRI across all cells.
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UWorld* World = PreviewScene->GetWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("PreviewScene has no UWorld"));
	}

	// Spawn one mesh component per resolved material at its grid position.
	TArray<UStaticMeshComponent*> CellComps;
	CellComps.Reserve(MaterialCount);
	for (int32 i = 0; i < MaterialCount; ++i)
	{
		const int32 Col = i % Columns;
		const int32 Row = i / Columns;

		// Y axis = left-right; Z axis = up-down. X is depth (camera axis).
		const FVector CellLocation = GridOriginOffset + FVector(
			0.0f,
			CellSpacing * Col,
			-CellSpacing * Row);

		UStaticMeshComponent* CellComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CellComp->SetStaticMesh(PreviewMesh);
		CellComp->SetMaterial(0, Materials[i]);
		CellComp->SetRelativeLocation(CellLocation);
		PreviewScene->AddComponent(CellComp, CellComp->GetRelativeTransform());
		CellComps.Add(CellComp);
	}

	// One shared RT sized to the requested total resolution.
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
	RT->UpdateResourceImmediate(true);

	// One shared capture component framed to cover all cells.
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Single capture → readback → PNG via the proven helper.
	const bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup.
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	for (UStaticMeshComponent* CellComp : CellComps)
	{
		PreviewScene->RemoveComponent(CellComp);
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("capture_material_grid: render or save failed"));
	}

	// Build result payload.
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("material_count"), MaterialCount);
	Result->SetNumberField(TEXT("columns"), Columns);
	Result->SetNumberField(TEXT("rows"), Rows);

	TSharedPtr<FJsonObject> CellRes = MakeShared<FJsonObject>();
	CellRes->SetNumberField(TEXT("width"), CellW);
	CellRes->SetNumberField(TEXT("height"), CellH);
	Result->SetObjectField(TEXT("cell_resolution"), CellRes);

	TSharedPtr<FJsonObject> TotalRes = MakeShared<FJsonObject>();
	TotalRes->SetNumberField(TEXT("width"), ResX);
	TotalRes->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), TotalRes);

	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

// =============================================================================
// editor::capture_with_overlay
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleCaptureWithOverlay(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params object is null"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("asset_path is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode) || Mode.IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("mode is required (wireframe | normals | uv_density | lightmap_density | shader_complexity)"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	// Validate mode upfront so we error before allocating any RHI resources.
	const bool bModeWireframe       = Mode.Equals(TEXT("wireframe"), ESearchCase::IgnoreCase);
	const bool bModeNormals         = Mode.Equals(TEXT("normals"), ESearchCase::IgnoreCase);
	const bool bModeUVDensity       = Mode.Equals(TEXT("uv_density"), ESearchCase::IgnoreCase);
	const bool bModeLightmapDensity = Mode.Equals(TEXT("lightmap_density"), ESearchCase::IgnoreCase);
	const bool bModeShaderComplex   = Mode.Equals(TEXT("shader_complexity"), ESearchCase::IgnoreCase);

	if (!(bModeWireframe || bModeNormals || bModeUVDensity || bModeLightmapDensity || bModeShaderComplex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported mode '%s' (supported: wireframe, normals, uv_density, lightmap_density, shader_complexity)"),
			*Mode), FMonolithJsonUtils::ErrInvalidParams);
	}

	UStaticMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(AssetPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load static mesh: %s"), *AssetPath));
	}

	int32 ResX = 512, ResY = 512;
	FString ParamError;
	if (!ParseResolutionArray(Params, TEXT("resolution"), ResX, ResY, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	// Camera default: -X 200 units, looking +X.
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	bool bCameraProvided = false;
	if (!ParseCameraObject(Params, CameraLocation, CameraRotation, FOV, bCameraProvided, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FString OutputPath;
	if (!ResolveOutputPath(Params, TEXT("CaptureWithOverlay"), OutputPath, ParamError))
	{
		return FMonolithActionResult::Error(ParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();

	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	MeshComp->SetStaticMesh(Mesh);
	PreviewScene->AddComponent(MeshComp, MeshComp->GetRelativeTransform());

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
	RT->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Toggle the requested show flag BEFORE first CaptureScene call. Mid-frame
	// show-flag changes are not deterministic (plan Section 8 gotcha).
	if (bModeWireframe)
	{
		CaptureComp->ShowFlags.SetWireframe(true);
	}
	else if (bModeNormals)
	{
		// UE 5.7 has no FEngineShowFlags::SetVisualizeMeshNormals — verified
		// offline via source_query. SetMeshEdges is the closest functional
		// approximation (renders mesh edges as a wireframe overlay on top of
		// the lit pass). Documented in the file header.
		CaptureComp->ShowFlags.SetMeshEdges(true);
	}
	else if (bModeUVDensity)
	{
		CaptureComp->ShowFlags.SetMeshUVDensityAccuracy(true);
	}
	else if (bModeLightmapDensity)
	{
		CaptureComp->ShowFlags.SetLightMapDensity(true);
	}
	else if (bModeShaderComplex)
	{
		CaptureComp->ShowFlags.SetShaderComplexity(true);
	}

	UWorld* World = PreviewScene->GetWorld();
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	const bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup.
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(MeshComp);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("capture_with_overlay: render or save failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetStringField(TEXT("mode"), Mode);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ResX);
	ResObj->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}
