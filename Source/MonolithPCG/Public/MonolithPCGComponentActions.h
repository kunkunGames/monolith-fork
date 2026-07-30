#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

/**
 * Typed PCG component lifecycle actions for editor-world instances and exact
 * Blueprint SCS component templates.
 *
 * Every action resolves the exact actor/component object path in the active
 * editor world or Blueprint asset for each invocation. Raw UObject pointers are
 * deliberately not cached across calls because Blueprint reconstruction,
 * compilation, and undo/redo can replace component instances or templates.
 */
class FMonolithPCGComponentActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult CreateComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetComponentGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetBlueprintComponentGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetComponentSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GenerateComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RefreshComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CancelComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CleanupComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetComponentOutput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetComponentUserParameters(const TSharedPtr<FJsonObject>& Params);
};

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MonolithPCG::Private
{
/** Inject one consumed-once owning-level save failure for an exact disposable actor. */
void ConfigureComponentLevelSaveTestFault(const FString& ExactActorPath);

/** Clear the component-level save fault target. */
void ResetComponentLevelSaveTestFault();
}
#endif
