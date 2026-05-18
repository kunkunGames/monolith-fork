#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.snapshot - dry-run by default; stores derived CRG projection manifest only when execute=true. */
class FProjectSnapshotAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("snapshot"); }
	static FString GetDescription() { return TEXT("Capture current ProjectIndex CRG projection manifest. Dry-run unless execute=true"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
