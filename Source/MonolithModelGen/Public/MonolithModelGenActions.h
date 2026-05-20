#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Generated-model provider, job, import, and provenance actions.
 */
class MONOLITHMODELGEN_API FMonolithModelGenActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListModelGenerationProviders(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SubmitGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CancelGeneratedModelJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DownloadGeneratedModelResult(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportGeneratedModel(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGeneratedModelProvenance(const TSharedPtr<FJsonObject>& Params);
};
