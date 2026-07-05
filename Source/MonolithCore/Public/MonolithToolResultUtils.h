#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Helpers for MCP tool result envelopes.
 *
 * Legacy clients continue to receive JSON serialized into text content while
 * newer MCP clients can opt into structuredContent through settings-gated call
 * sites. In structured mode, successful responses keep content[] compact and
 * place the JSON payload only in structuredContent.
 *
 * Failure envelopes default to compact (bCompactErrorEnvelope=true): exactly one
 * machine-readable copy of related_actions/hints/error_data per response and no
 * top-level error_data field flattening. With structuredContent enabled the copy
 * lives in structuredContent and text is a one-line pointer; without it the copy
 * stays in the top-level fields and text keeps the full error text for
 * text-only clients. bCompactErrorEnvelope=false reproduces the legacy
 * duplicated shape byte-for-byte for old clients.
 */
class MONOLITHCORE_API FMonolithToolResultUtils
{
public:
	static TSharedPtr<FJsonObject> BuildMcpToolResult(
		const FMonolithActionResult& ActionResult,
		bool bEnableStructuredContent,
		bool bEnableTypedMedia = false,
		bool bCompactErrorEnvelope = true);

private:
	static FString BuildErrorText(const FMonolithActionResult& ActionResult);
	static TSharedPtr<FJsonObject> BuildStructuredErrorContent(const FMonolithActionResult& ActionResult);
	static TSharedPtr<FJsonObject> BuildMetaObject(const FString& ResultKind, const FString& ContentTextMode);
};
