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
	static FMonolithActionResult ListActionClasses(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeActionSet(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddActionSetInputMapping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetPrimaryAssetScan(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddGameFeatureDataInputMapping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddGameFeatureDataWidgets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddGameFeatureDataComponents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddGameFeatureDataGameplayCuePaths(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddGameFeatureDataAbilities(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveGameFeatureDataAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidatePlugin(const TSharedPtr<FJsonObject>& Params);

	static TSharedPtr<FJsonObject> EmptySchema();
	static TSharedPtr<FJsonObject> ListPluginsSchema();
	static TSharedPtr<FJsonObject> FindGameFeatureDataSchema();
	static TSharedPtr<FJsonObject> DescribeGameFeatureDataSchema();
	static TSharedPtr<FJsonObject> ListActionClassesSchema();
	static TSharedPtr<FJsonObject> DescribeActionSetSchema();
	static TSharedPtr<FJsonObject> AddActionSetInputMappingSchema();
	static TSharedPtr<FJsonObject> SetPrimaryAssetScanSchema();
	static TSharedPtr<FJsonObject> AddGameFeatureDataInputMappingSchema();
	static TSharedPtr<FJsonObject> AddGameFeatureDataWidgetsSchema();
	static TSharedPtr<FJsonObject> AddGameFeatureDataComponentsSchema();
	static TSharedPtr<FJsonObject> AddGameFeatureDataGameplayCuePathsSchema();
	static TSharedPtr<FJsonObject> AddGameFeatureDataAbilitiesSchema();
	static TSharedPtr<FJsonObject> RemoveGameFeatureDataActionSchema();
	static TSharedPtr<FJsonObject> ValidatePluginSchema();
};
