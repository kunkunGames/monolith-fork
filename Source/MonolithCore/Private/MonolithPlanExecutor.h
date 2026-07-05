#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * monolith.execute_plan — generic server-side multi-step composition (v1).
 *
 * Executes a validated sequence of registered actions in one call so agents
 * stop paying one MCP round-trip per authoring step. Every step dispatches
 * through FMonolithToolRegistry::ExecuteAction, so profile gating, alias
 * rewriting, schema validation, execution guards, and invocation logging all
 * apply to each child exactly as they would for a direct call; child log
 * records inherit the plan's trace with the plan action as their parent span.
 *
 * v1 (plan doc P1-1, conservative slice): dry_run validation + plan report,
 * sequential execution with stop_on_error, whole-string step result
 * references ("$steps.<id>.result.<field.path>"), a confirm gate when any
 * step is mutating, and an allow_destructive gate for destructive steps.
 * v2 adds numeric array-index reference segments and an outermost editor
 * transaction (transaction=auto) that is cancelled when stop_on_error halts
 * a mutating plan — undoable object edits roll back; saves, disk writes,
 * source-control, and external-process effects stay applied and the response
 * says so per step and in the transaction caveat.
 */
class FMonolithPlanExecutor
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleExecutePlan(const TSharedPtr<FJsonObject>& Params);
};
