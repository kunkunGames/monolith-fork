#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Phase 22: Polish & Remaining
 * Quality-of-life, proxy mesh generation, HLOD setup, and texture budget analysis.
 */
class MONOLITHMESH_API FMonolithMeshQualityActions
{
public:
	/** Register mesh quality/polish actions with the tool registry. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Proxy & HLOD ---
	static FMonolithActionResult GenerateProxyMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetupHlod(const TSharedPtr<FJsonObject>& Params);

	// --- Texture Budget ---
	static FMonolithActionResult AnalyzeTextureBudget(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);
};
