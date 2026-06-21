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
 */
class MONOLITHCORE_API FMonolithToolResultUtils
{
public:
	static TSharedPtr<FJsonObject> BuildMcpToolResult(
		const FMonolithActionResult& ActionResult,
		bool bEnableStructuredContent,
		bool bEnableTypedMedia = false);

private:
	static FString BuildErrorText(const FMonolithActionResult& ActionResult);
	static TSharedPtr<FJsonObject> BuildStructuredErrorContent(const FMonolithActionResult& ActionResult);
	static TSharedPtr<FJsonObject> BuildMetaObject(const FString& ResultKind, const FString& ContentTextMode);
};
