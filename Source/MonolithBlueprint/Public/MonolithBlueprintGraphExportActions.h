#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintGraphExportActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Phase 5C — Graph export/import/copy
	static FMonolithActionResult HandleExportGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCopyNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCloneGraphsWithReferenceRemap(const TSharedPtr<FJsonObject>& Params);
};
