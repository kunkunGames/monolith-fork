#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Bounded, reflection-only Chooser discovery, readback, and validation.
 *
 * These six actions intentionally live in MonolithAnimation beside the
 * existing Chooser authoring actions so one module owns registration and
 * namespace teardown. The handlers do not mutate or compile Chooser assets.
 * They remain registered when the optional Chooser plugin is disabled and
 * return an explicit availability error for actions that need the class.
 */
class MONOLITHANIMATION_API FMonolithChooserReadActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListChooserTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetChooserTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserColumns(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserRows(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateChooserTable(const TSharedPtr<FJsonObject>& Params);
};
