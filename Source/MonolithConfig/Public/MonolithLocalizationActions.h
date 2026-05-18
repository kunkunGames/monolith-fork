#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithLocalizationActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListCultures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListStringTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetStringEntry(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveStringEntry(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetStringMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportStringTableCsv(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportStringTableCsv(const TSharedPtr<FJsonObject>& Params);
};
