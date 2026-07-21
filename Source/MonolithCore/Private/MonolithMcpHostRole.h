#pragma once

#include "CoreMinimal.h"

/**
 * Describes whether the current Unreal process is a durable owner for the
 * Monolith MCP HTTP listener. All roles still load MonolithCore and register
 * actions; only DurableHost is allowed to bind the listener and own its
 * sentinel file.
 */
enum class EMonolithMcpHostRole : uint8
{
	DurableHost,
	Commandlet,
	PlannedTestExit,
	AutomationExec,
};

/** Pure command-line classifier used by module startup and automation tests. */
class FMonolithMcpHostRole
{
public:
	/** Classify explicit process inputs. Kept pure so startup policy is regression-testable. */
	static EMonolithMcpHostRole Classify(bool bRunningCommandlet, const TCHAR* CommandLine);

	/** Classify the current Unreal process. */
	static EMonolithMcpHostRole ClassifyCurrentProcess();

	/** True only for a persistent editor/headless process that may own the MCP endpoint. */
	static bool IsDurableHost(EMonolithMcpHostRole Role);

	/** Stable diagnostic name for logs and verification records. */
	static const TCHAR* ToString(EMonolithMcpHostRole Role);
};
