#pragma once

#include "CoreMinimal.h"

struct FMonolithActionResult;
class FMonolithToolRegistry;

class FMonolithDataflowActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetDataflowGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowNodeTypes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetDataflowNodeSchema(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateDataflowGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowVariables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListDataflowComments(const TSharedPtr<FJsonObject>& Params);
};
