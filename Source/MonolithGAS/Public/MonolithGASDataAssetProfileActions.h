#pragma once

#include "MonolithGASInternal.h"

class FMonolithGASDataAssetProfileActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleDescribeDataAssetGASProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateDataAssetGASProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetDataAssetGASFields(const TSharedPtr<FJsonObject>& Params);
};
