#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithActorMergeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult PreviewActorMerge(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MergeActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateProxyMeshFromActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MergeActorsToInstances(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BakeActorMaterials(const TSharedPtr<FJsonObject>& Params);
};
