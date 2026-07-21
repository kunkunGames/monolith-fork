#pragma once

#include "CoreMinimal.h"

namespace MonolithSourceQueryProcessArgs
{
	/** Quote one argument for the Windows command-line parsing contract used by ExecProcess. */
	FString Quote(const FString& Arg);

	/** Build the offline EngineSource-backed graph-search invocation. */
	FString BuildSearchCrgGraph(
		const FString& Query,
		const FString& SourceDbPath,
		const FString& Kind,
		int32 Limit);
}
