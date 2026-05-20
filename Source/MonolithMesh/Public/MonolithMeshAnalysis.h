#pragma once

#include "CoreMinimal.h"

class UWorld;
class UNavigationSystemV1;
class ANavigationData;
struct FNavPathPoint;

/**
 * Shared analysis helpers for Horror & Accessibility actions.
 * Tension scoring, concealment calculation, path clearance, flood-fill.
 */
namespace MonolithMeshAnalysis
{
	// ========================================================================
	// Concealment
	// ========================================================================

	/**
	 * Compute concealment of a point from one or more viewpoints.
	 * Fires rays from the test point toward each viewpoint.
	 * @return 0.0 (fully visible) to 1.0 (fully concealed)
	 */
	MONOLITHMESH_API float ComputeConcealment(UWorld* World, const FVector& TestPoint, const TArray<FVector>& Viewpoints);

	// ========================================================================
	// Path Clearance
	// ========================================================================

	/** Result of a clearance measurement at a single point */
	struct FPathClearance
	{
		FVector Location = FVector::ZeroVector;
		float LeftClearance = 0.0f;   // cm
		float RightClearance = 0.0f;  // cm
		float TotalWidth = 0.0f;      // cm
		FString LeftObstruction;
		FString RightObstruction;
	};

	/**
	 * Measure perpendicular clearance along a path at each point.
	 * Fires rays perpendicular to the path direction at each point.
	 * @param MaxWidth  Maximum distance to trace on each side (cm)
	 */
	MONOLITHMESH_API TArray<FPathClearance> MeasurePathClearance(UWorld* World, const TArray<FVector>& PathPoints, float MaxWidth = 500.0f);

	// ========================================================================
	// Tension Scoring
	// ========================================================================

	/** Inputs for tension computation */
	struct FTensionInputs
	{
		float AverageSightlineDistance = 0.0f; // cm
		float CeilingHeight = 0.0f;           // cm (0 = no ceiling detected)
		float RoomVolume = 0.0f;              // cubic cm (approximate)
		int32 ExitCount = 0;
	};

	/** Tension classification */
	enum class ETensionLevel : uint8
	{
		Calm,
		Uneasy,
		Tense,
		Dread,
		Panic
	};

	/** Get string name for tension level */
	MONOLITHMESH_API const TCHAR* TensionLevelToString(ETensionLevel Level);

	/**
	 * Compute a tension score 0-100 from spatial inputs.
	 * Low sightlines + low ceiling + small volume + few exits = high tension.
	 */
	MONOLITHMESH_API float ComputeTensionScore(const FTensionInputs& Inputs);

	/** Map a 0-100 tension score to a tension level */
	MONOLITHMESH_API ETensionLevel ClassifyTension(float Score);

	// ========================================================================
	// Spatial Measurements
	// ========================================================================

	/** Measure ceiling height above a point via upward ray */
	MONOLITHMESH_API float MeasureCeilingHeight(UWorld* World, const FVector& Location, float MaxHeight = 2000.0f);

	/** Approximate room volume from radial sweep distances */
	MONOLITHMESH_API float ApproximateRoomVolume(UWorld* World, const FVector& Location, float MaxRadius = 5000.0f, int32 RayCount = 16);

	/** Count exit directions from a point (navmesh reachable directions that lead far away) */
	MONOLITHMESH_API int32 CountExits(UWorld* World, const FVector& Location, float TestRadius = 2000.0f, int32 Directions = 8);

	// ========================================================================
	// Navmesh Flood Fill
	// ========================================================================

	/** A dead-end region found by flood fill */
	struct FDeadEnd
	{
		FVector Center = FVector::ZeroVector;
		FVector ExitDirection = FVector::ZeroVector;
		float Depth = 0.0f;         // Distance from exit to deepest point (cm)
		float Width = 0.0f;         // Average width (cm)
		float ExitWidth = 0.0f;     // Width at the narrowest exit (cm)
		TArray<FVector> BoundaryPoints;
	};

	/**
	 * Flood-fill navmesh within a region to find dead-end areas (single-exit).
	 * Samples a grid on the navmesh, builds connectivity, identifies nodes with
	 * only one exit path.
	 */
	MONOLITHMESH_API TArray<FDeadEnd> FloodFillDeadEnds(UWorld* World, const FBox& Region, float GridSize = 200.0f);

	// ========================================================================
	// Navigation Helpers
	// ========================================================================

	/** Get navmesh system + data, returning false with error if unavailable */
	MONOLITHMESH_API bool GetNavSystem(UWorld* World, UNavigationSystemV1*& OutNavSys, ANavigationData*& OutNavData, FString& OutError);

	/** Find a navmesh path and return the path points + total distance. Returns false if no path. */
	MONOLITHMESH_API bool FindNavPath(UWorld* World, const FVector& Start, const FVector& End, TArray<FVector>& OutPoints, float& OutDistance, float AgentRadius = 42.0f);

	/** JSON array from FVector */
	MONOLITHMESH_API TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);
}
