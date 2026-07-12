#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

/** PCG graph-asset creation and topology/settings editing actions. */
class FMonolithPCGGraphAuthoringActions
{
  public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult ListNodeTypes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGraphInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConnectNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DisconnectNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetNodeParams(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateGraph(const TSharedPtr<FJsonObject>& Params);
};
