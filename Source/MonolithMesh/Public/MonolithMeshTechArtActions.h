#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UMonolithMeshHandlePool;

/**
 * Phase 16: Tech Art Pipeline (7 mesh actions + modelgen action surface)
 * Import, quality fix, LOD generation, texel density, material cost,
 * collision setup, and lightmap density analysis.
 */
class MONOLITHMESH_API FMonolithMeshTechArtActions
{
public:
	/** Register all 7 tech art actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/** Register generated-model provider/job/import/provenance actions with the modelgen namespace */
	static void RegisterModelGenActions(FMonolithToolRegistry& Registry);

#if WITH_GEOMETRYSCRIPT
	/** Set the handle pool instance (called during module startup) */
	static void SetHandlePool(UMonolithMeshHandlePool* InPool);
#endif

private:
	/** Import FBX/glTF mesh via IAssetTools::ImportAssetsAutomated */
	static FMonolithActionResult ImportMesh(const TSharedPtr<FJsonObject>& Params);

	/** List local/external generated-model provider boundaries */
	static FMonolithActionResult ListModelGenerationProviders(const TSharedPtr<FJsonObject>& Params);

	/** Submit a local deterministic generated model job */
	static FMonolithActionResult SubmitGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);

	/** Read a generated model job manifest */
	static FMonolithActionResult GetGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);

	/** Cancel a generated model job when it is still cancelable */
	static FMonolithActionResult CancelGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);

	/** Resolve the local file for a completed generated model job */
	static FMonolithActionResult DownloadGeneratedModelResult(const TSharedPtr<FJsonObject>& Params);

	/** Import a generated model file or completed job as StaticMesh assets */
	static FMonolithActionResult ImportGeneratedModel(const TSharedPtr<FJsonObject>& Params);

	/** Read Monolith generation provenance from a StaticMesh */
	static FMonolithActionResult GetGeneratedModelProvenance(const TSharedPtr<FJsonObject>& Params);

	/** Export a UStaticMesh / USkeletalMesh asset to FBX file on disk via UAssetExportTask */
	static FMonolithActionResult ExportMesh(const TSharedPtr<FJsonObject>& Params);

	/** Auto-fix mesh quality: weld, degenerate removal, hole fill, normals (GeometryScript) */
	static FMonolithActionResult FixMeshQuality(const TSharedPtr<FJsonObject>& Params);

	/** One-shot LOD generation: simplify + write back to UStaticMesh source models */
	static FMonolithActionResult AutoGenerateLods(const TSharedPtr<FJsonObject>& Params);

	/** Texel density analysis: UV area vs world-space area ratio per section */
	static FMonolithActionResult AnalyzeTexelDensity(const TSharedPtr<FJsonObject>& Params);

	/** Cross-module: spatial query + shader instruction count per material */
	static FMonolithActionResult AnalyzeMaterialCostInRegion(const TSharedPtr<FJsonObject>& Params);

	/** Set collision on a static mesh asset (simple shapes, complex, auto-convex) */
	static FMonolithActionResult SetMeshCollision(const TSharedPtr<FJsonObject>& Params);

	/** Lightmap texel density analysis and resolution recommendations */
	static FMonolithActionResult AnalyzeLightmapDensity(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

#if WITH_GEOMETRYSCRIPT
	static UMonolithMeshHandlePool* Pool;
#endif
};
