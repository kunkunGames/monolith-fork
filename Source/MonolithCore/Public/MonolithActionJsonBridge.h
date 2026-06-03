#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Shared JSON bridge for Blueprint-facing wrappers that expose registry actions
 * as FString JSON. Keeps domain libraries thin and routes through the same
 * registry validation, execution guard, and action logging as MCP calls.
 */
class MONOLITHCORE_API FMonolithActionJsonBridge
{
public:
	/**
	 * Execute a registered action and return parseable JSON.
	 *
	 * Success returns the action result object serialized directly to preserve
	 * existing Blueprint wrapper payloads. Failure returns:
	 *   {"success":false,"error":"...","code":-32602}
	 */
	static FString ExecuteActionAsJson(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutSuccess,
		FString& OutError);

	static FString MakeErrorEnvelope(const FString& Message, int32 Code);
};
