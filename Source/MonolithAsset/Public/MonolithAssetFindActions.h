// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"

class FMonolithToolRegistry;
class FJsonObject;
struct FMonolithActionResult;

/** Request for a fuzzy live-AssetRegistry search (asset.find_assets). */
struct FAssetFindRequest
{
	FString Query;
	FString Path = TEXT("/Game");
	bool bRecursive = true;
	TArray<FString> ClassNames;
	int32 Limit = 20;
	TOptional<int32> Threshold;
	bool bIncludeTags = false;
	bool bIncludeScoreBreakdown = false;
	int32 ScanBudget = 20000;
};

/** One ranked match row. */
struct FAssetFindRow
{
	FString ObjectPath;
	FString PackageName;
	FString AssetName;
	FString ClassName;
	FString ClassPath;
	int32 Score = 0;
	FString Reason;
	TArray<FString> MatchedTokens;
	int32 BestDistance = MAX_int32;
	TMap<FString, int32> FieldScores;
};

/** Aggregate result + counters. */
struct FAssetFindResult
{
	TArray<FAssetFindRow> Matches;
	int32 FilteredCount = 0;
	int32 ScannedCount = 0;
	int32 MatchedCount = 0;
	bool bTruncated = false;
	bool bLimited = false;
};

/**
 * asset.find_assets — fuzzy, scored, typo-tolerant search over the live AssetRegistry.
 * Thin consumer of FMonolithFuzzyMatch (MonolithCore); owns its own corpus, fields,
 * weights, thresholds, and output shape. Intentionally separate from the exact-name
 * FMonolithAssetUtils::FindAssetCandidates and the offline project FTS search.
 */
class FMonolithAssetFindActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/**
	 * Resolve class-name / class-path entries to FTopLevelAssetPath via the project's
	 * FindFirstObject<UClass> convention (no asset loading). Unknown entries are collected
	 * in OutUnknown. Returns true when every entry resolved.
	 */
	static bool ResolveClassNames(const TArray<FString>& InClassNames, TArray<FTopLevelAssetPath>& OutPaths, TArray<FString>& OutUnknown);

	/**
	 * Run the search. Returns false with OutError (+ optional structured OutErrorData) on
	 * semantic validation failure (invalid path, unknown class). Never loads assets; scores
	 * FAssetData metadata only on the game thread.
	 */
	static bool RunAssetFind(const FAssetFindRequest& Request, FAssetFindResult& OutResult, FString& OutError, TSharedPtr<FJsonObject>& OutErrorData);

private:
	static FMonolithActionResult FindAssets(const TSharedPtr<FJsonObject>& Params);
};
