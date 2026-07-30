#pragma once

#include "MonolithSourceSubsystem.h"
#include "MonolithToolRegistry.h"

namespace MonolithSourceAvailability
{
	FMonolithActionResult MakeDatabaseUnavailableError(
		const FMonolithSourceDatabaseStatus& Status);

	FMonolithActionResult MakeIndexRequestError(
		const FMonolithSourceDatabaseStatus& Status,
		const FString& RequestedMode);
}
