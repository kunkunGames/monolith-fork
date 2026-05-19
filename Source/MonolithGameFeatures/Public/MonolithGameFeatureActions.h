#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithGameFeatureActions
{
public:
	static void Register(FMonolithToolRegistry& Registry, bool bEnableInspectionActions);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListPlugins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindGameFeatureData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeGameFeatureData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidatePlugin(const TSharedPtr<FJsonObject>& Params);

	static TSharedPtr<FJsonObject> EmptySchema();
	static TSharedPtr<FJsonObject> ListPluginsSchema();
	static TSharedPtr<FJsonObject> FindGameFeatureDataSchema();
	static TSharedPtr<FJsonObject> DescribeGameFeatureDataSchema();
	static TSharedPtr<FJsonObject> ValidatePluginSchema();
};
