#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.diff_snapshots - read-only diff between stored/current CRG projection manifests. */
class FProjectDiffSnapshotsAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("diff_snapshots"); }
	static FString GetDescription() { return TEXT("Compare stored/current ProjectIndex CRG projection snapshots"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
