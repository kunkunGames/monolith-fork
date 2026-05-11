#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithHlodActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListHlodLayers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetHlodLayer(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateHlodLayer(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConfigureHlodLayer(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListHlodSourceActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListHlodActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetHlodStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CheckHlodHash(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildHlod(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ClearLegacyHlod(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult LegacyHlodNeedsBuild(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportHlod(const TSharedPtr<FJsonObject>& Params);
};
