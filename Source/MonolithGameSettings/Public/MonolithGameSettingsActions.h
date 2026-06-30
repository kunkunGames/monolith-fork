#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithGameSettingsActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeRegistryTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateSettingClassContract(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateDataSourceBindings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateVisualData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidatePlayerMappableInputSettings(const TSharedPtr<FJsonObject>& Params);
};
