#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Read-only Enhanced Input asset discovery, bounded inspection, and validation.
 *
 * The `input` namespace is independent of the optional GAS authoring toggle:
 * UInputAction and UInputMappingContext are engine Enhanced Input assets. These
 * handlers never transact, save, mutate, or dirty the inspected packages.
 */
class MONOLITHGAS_API FMonolithGASInputAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListInputActions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params);
};
