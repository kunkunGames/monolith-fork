#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Monolith-native context mention surface backed by the existing project/source
 * indexes. This intentionally stays lexical/local; external embedding providers
 * belong outside the default tool surface.
 */
class FMonolithSourceContextActions
{
public:
	static void RegisterAll();

private:
	static FMonolithActionResult HandleGetIndexStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStartIndexing(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchItems(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildAttachment(const TSharedPtr<FJsonObject>& Params);
};
