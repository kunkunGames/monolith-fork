#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Read-only culture discovery and bounded StringTable inspection.
 *
 * These handlers use internationalization and AssetRegistry read APIs only.
 * They never transact, save, mutate, or dirty an inspected package.
 */
class MONOLITHCONFIG_API FMonolithLocalizationActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListCultures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListStringTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateStringTable(const TSharedPtr<FJsonObject>& Params);
};
