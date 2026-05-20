#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * StaticMesh game-readiness and LOD validation actions.
 */
class MONOLITHMESH_API FMonolithMeshValidationActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ValidateGameReady(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SuggestLodStrategy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchValidate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CompareLodChain(const TSharedPtr<FJsonObject>& Params);
};
