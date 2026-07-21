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
	static FMonolithActionResult SetGraphUserParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSubgraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ReplaceGraphContents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateGraph(const TSharedPtr<FJsonObject>& Params);
};

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MonolithPCG::Private
{
enum class EPCGGraphContentsReplacementTestFault : uint8
{
	None,
	BeforeSave
};

/** Configure one exact disposable target and one consumed-once failure. */
void ConfigureGraphContentsReplacementTestFault(
	const FString& ExactTargetObjectPath,
	EPCGGraphContentsReplacementTestFault Fault);

/** Clear every replacement-action test override. */
void ResetGraphContentsReplacementTestFault();
}
#endif
