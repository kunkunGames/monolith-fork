#pragma once

#include "MonolithAIInternal.h"

class FMonolithAIChooserActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListChooserTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetChooserTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserColumns(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserRows(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateChooserTable(const TSharedPtr<FJsonObject>& Params);
};
