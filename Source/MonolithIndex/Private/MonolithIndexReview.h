#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FMonolithIndexDatabase;

/**
 * CRG-inspired review/navigation helpers over the EXISTING ProjectIndex.db graph.
 *
 * Adapted from code-review-graph (commit 0919071a) — recursive impact traversal,
 * token-efficient minimal output, factor-decomposed risk, and non-fatal health/
 * repair post-processing — but NOT its generic nodes/edges schema, Python runtime,
 * language parser, embeddings, wiki, or daemon. Monolith stays asset-domain native:
 * this only reads `assets`/`dependencies`/`nodes`/`variables`/`parameters`/
 * `tag_references` and the asset FTS tables already present.
 *
 * All methods use the public `FMonolithIndexDatabase` surface (incl. the public
 * `GetRawDatabase()` + transaction helpers), so the 1400-line DB implementation
 * file is untouched and existing direct actions are unaffected (REQ-009).
 *
 * Every result follows the common contract:
 *   status, summary, input, limits, truncated, (impacted_assets|items|impact),
 *   next_actions.
 */
class FMonolithIndexReview
{
public:
	/** Bounded BFS over `dependencies`. direction: in|out|both. */
	static TSharedPtr<FJsonObject> ImpactRadius(
		FMonolithIndexDatabase& Db,
		const FString& AssetPath,
		const FString& Direction,
		int32 MaxDepth,
		int32 MaxResults,
		const FString& DependencyType);

	/** Read-only schema/FTS/orphan/meta diagnostics. Never mutates. */
	static TSharedPtr<FJsonObject> Health(FMonolithIndexDatabase& Db, bool bIncludeCounts);

	/**
	 * FTS rebuild. Default dry-run. Mutates ONLY when bExecute is true.
	 * Caller MUST gate on UMonolithIndexSubsystem::IsIndexing() first.
	 */
	static TSharedPtr<FJsonObject> RepairFts(FMonolithIndexDatabase& Db, const FString& Target, bool bExecute);

	/**
	 * Rebuild derived CRG projection/cache tables. Default dry-run; mutates only
	 * when bExecute is true. Caller MUST gate on active indexing first.
	 */
	static TSharedPtr<FJsonObject> RepairCrgCache(FMonolithIndexDatabase& Db, bool bExecute);

	/** Cached risk scoring when present; query-time fallback -> {score,tier,reasons[],raw_counts}. */
	static TSharedPtr<FJsonObject> RiskScore(
		FMonolithIndexDatabase& Db,
		const FString& SeedPath,
		int32 Limit,
		const FString& MinTier);

	/** Changed asset path triage: changed_entities, direct referencer impact, and risk-prioritized review queue. */
	static TSharedPtr<FJsonObject> DetectChanges(
		FMonolithIndexDatabase& Db,
		const TArray<FString>& ChangedPaths,
		int32 MaxResults,
		const FString& DetailLevel);

	/** Advisory orphan-asset candidates. Read-only; never mutates and never reports high confidence. */
	static TSharedPtr<FJsonObject> FindUnused(
		FMonolithIndexDatabase& Db,
		const FString& Kind,
		int32 Limit,
		const FString& MinConfidence);

	/** Rank global project review hotspots by fan-in, fan-out, risk, graph size, or all. */
	static TSharedPtr<FJsonObject> ReviewHotspots(
		FMonolithIndexDatabase& Db,
		const FString& Kind,
		int32 Limit,
		int32 MinLines,
		bool bIncludeQuestions);

	/** Compose impact + risk + direct details + next actions. detail_level: minimal|standard. */
	static TSharedPtr<FJsonObject> ReviewContext(
		FMonolithIndexDatabase& Db,
		const FString& AssetPath,
		const FString& Direction,
		int32 MaxDepth,
		int32 MaxResults,
		const FString& DetailLevel);

	// Clamp helpers shared with the action layer (kept consistent with existing
	// limit semantics: callers may pass huge values; we clamp, never reject).
	static int32 ClampDepth(int32 In) { return FMath::Clamp(In <= 0 ? 2 : In, 1, 6); }
	static int32 ClampResults(int32 In) { return FMath::Clamp(In <= 0 ? 200 : In, 1, 2000); }

	// Param readers tolerant of MCP string/number/bool encoding.
	static FString PStr(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, const FString& Def = TEXT(""))
	{
		FString S;
		return (P.IsValid() && P->TryGetStringField(Key, S)) ? S : Def;
	}
	static int32 PInt(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, int32 Def)
	{
		if (!P.IsValid()) return Def;
		double D;
		if (P->TryGetNumberField(Key, D)) return static_cast<int32>(D);
		FString S;
		if (P->TryGetStringField(Key, S) && !S.IsEmpty()) return FCString::Atoi(*S);
		return Def;
	}
	static bool PBool(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, bool Def)
	{
		if (!P.IsValid()) return Def;
		bool B;
		if (P->TryGetBoolField(Key, B)) return B;
		FString S;
		if (P->TryGetStringField(Key, S)) return S == TEXT("true") || S == TEXT("1");
		return Def;
	}
};
