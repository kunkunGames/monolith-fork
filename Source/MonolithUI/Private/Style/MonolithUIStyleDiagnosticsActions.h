// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

#if WITH_COMMONUI

namespace MonolithUI
{
	struct FStyleDiagnosticsActions
	{
		static void Register(FMonolithToolRegistry& Registry);

		static FMonolithActionResult HandleDumpStyleCacheStats(const TSharedPtr<FJsonObject>& Params);
	};
}

#endif // WITH_COMMONUI
