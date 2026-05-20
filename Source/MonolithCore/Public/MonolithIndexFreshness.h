#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMonolithSettings;

/**
 * Shared, read-only helpers for reporting Monolith index database freshness.
 *
 * These helpers intentionally stay at the file/JSON boundary so Core, Source,
 * and Index can share path/freshness/status summarization without coupling Core
 * to editor subsystem implementation classes.
 */
class MONOLITHCORE_API FMonolithIndexFreshnessUtils
{
public:
	static FString ResolveProjectIndexDbPath(const UMonolithSettings* Settings = nullptr);
	static FString ResolveSourceIndexDbPath(const UMonolithSettings* Settings = nullptr);

	static TSharedPtr<FJsonObject> MakeDatabaseFreshness(const FString& Name, const FString& DbPath);
	static TSharedPtr<FJsonObject> SummarizeHealthResult(const TSharedPtr<FJsonObject>& HealthResult);
};
