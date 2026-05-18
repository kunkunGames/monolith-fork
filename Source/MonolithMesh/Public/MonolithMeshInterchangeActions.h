#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithMeshInterchangeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult GetSupportedFormats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CanImport(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CanReimport(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetImportData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult UpdateReimportPath(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ReimportAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ReimportAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportAsset(const TSharedPtr<FJsonObject>& Params);
};
