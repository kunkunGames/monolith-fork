#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithDataflowActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListAssets(const TSharedPtr<FJsonObject>& Params);

private:
	static FMonolithActionResult GetDataflowGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowNodeTypes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetDataflowNodeSchema(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateDataflowGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowVariables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowComments(const TSharedPtr<FJsonObject>& Params);
};
