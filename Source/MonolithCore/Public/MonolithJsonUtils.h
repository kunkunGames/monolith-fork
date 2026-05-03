#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

MONOLITHCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogMonolith, Log, All);

class MONOLITHCORE_API FMonolithJsonUtils
{
public:
	/** Create a success JSON-RPC response wrapping a result object */
	static TSharedPtr<FJsonObject> SuccessResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result);

	/** Create an error JSON-RPC response */
	static TSharedPtr<FJsonObject> ErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonValue>& Data = nullptr);

	/** Convenience: wrap a JSON object as a success result */
	static TSharedPtr<FJsonObject> SuccessObject(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj);

	/** Convenience: wrap a simple string as a success result */
	static TSharedPtr<FJsonObject> SuccessString(const TSharedPtr<FJsonValue>& Id, const FString& Message);

	/** Serialize a FJsonObject to a compact JSON string */
	static FString Serialize(const TSharedPtr<FJsonObject>& JsonObject);

	/** Parse a JSON string into a FJsonObject. Returns nullptr on failure. */
	static TSharedPtr<FJsonObject> Parse(const FString& JsonString);

	/** Create a JSON array from a TArray of strings */
	static TSharedRef<FJsonValueArray> StringArrayToJson(const TArray<FString>& Strings);

	// --- JSON-RPC 2.0 Error Codes ---
	static constexpr int32 ErrParseError = -32700;
	static constexpr int32 ErrInvalidRequest = -32600;
	static constexpr int32 ErrMethodNotFound = -32601;
	static constexpr int32 ErrInvalidParams = -32602;
	static constexpr int32 ErrInternalError = -32603;

	// --- Monolith server-defined error codes (JSON-RPC -32000..-32099 range) ---
	//
	// R3b / §5.5 Error Contract — emitted when an action's underlying
	// optional sibling/marketplace plugin is absent. The action exists in
	// the registry; the call cannot be served because the underlying
	// type/asset/subsystem is missing. First consumer: the EffectSurface
	// action handlers when the optional provider is not loaded. Distinct from
	// `ErrInternalError` so telemetry / LLM error counters do not conflate
	// "feature gracefully unavailable" with "server choked".
	//
	// Reserved range -32011..-32019 left open for future "optional dep"
	// codes (e.g., `ErrFeatureGated` if we later distinguish "missing
	// plugin" from "feature flag off").
	static constexpr int32 ErrOptionalDepUnavailable = -32010;
};
