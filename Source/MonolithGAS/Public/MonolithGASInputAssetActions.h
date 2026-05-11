#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithGASInputAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListInputActions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateInputAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetInputActionProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateInputMappingContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddInputMapping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveInputMapping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params);
};
