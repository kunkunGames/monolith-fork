#pragma once

#include "CoreMinimal.h"

namespace MonolithToolText
{
	/**
	 * Trim a registry description to the bounded one-line form shared by
	 * terse per-namespace and cross-namespace discovery results.
	 */
	FString TerseOneLineDescription(const FString& Full);
}
