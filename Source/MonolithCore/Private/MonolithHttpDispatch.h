#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace MonolithHttpDispatch
{
	struct FNormalizationResult
	{
		TSharedPtr<FJsonObject> Arguments;
		FString Error;

		bool IsSuccess() const { return Arguments.IsValid() && Error.IsEmpty(); }
	};

	/**
	 * Collapse the supported MCP argument shapes into the flat object consumed by
	 * FMonolithToolRegistry. Reserved transport fields are removed. A `params`
	 * field is treated as the action's own field whenever the registered action
	 * declares that exact name; otherwise only an object (or object-encoded JSON
	 * string) is interpreted as the legacy nested envelope.
	 */
	FNormalizationResult NormalizeActionArguments(
		const TSharedPtr<FJsonObject>& Arguments,
		const TSet<FString>& ReservedTransportFields,
		const TSharedPtr<FJsonObject>& ActionParamSchema);
}
