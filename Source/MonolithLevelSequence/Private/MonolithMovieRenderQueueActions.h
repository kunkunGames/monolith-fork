#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithMovieRenderQueueActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetQueue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult LoadQueue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveQueue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DuplicateJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteJob(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteAllJobs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetJobIndex(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RenderQueue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult IsRendering(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RenderProgress(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CancelRender(const TSharedPtr<FJsonObject>& Params);
};
