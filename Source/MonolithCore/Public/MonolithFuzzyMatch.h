// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"

/** Per-field token-scoring weights. Fuzzy=0 disables typo tolerance for the field. */
struct FMonolithFuzzyWeights
{
	int32 Exact = 0;
	int32 Prefix = 0;
	int32 Contains = 0;
	int32 Fuzzy = 0;
};

/** One scored field of a candidate (whole-field phrase text + tokens + weights). */
struct FMonolithFuzzyField
{
	/** Normalized field text, used for whole-query phrase matching. */
	FString Text;
	/** Tokenized field text, used for per-token matching. */
	TArray<FString> Tokens;
	/** Per-token scoring weights. */
	FMonolithFuzzyWeights Weights;
	/** Whole-query phrase bonuses against this field's Text (exact > prefix > contains). */
	int32 ExactPhraseBonus = 0;
	int32 PrefixPhraseBonus = 0;
	int32 ContainsPhraseBonus = 0;
	/** Reason-tag stem, e.g. TEXT("asset_name"); emits "<stem>", "<stem>_exact", etc. */
	const TCHAR* ReasonTag = nullptr;
};

/** Result of scoring one candidate. */
struct FMonolithFuzzyScore
{
	int32 Score = 0;
	TArray<FString> Reasons;
	TArray<FString> MatchedTokens;
	/** Smallest typo edit distance that contributed, or MAX_int32 when no fuzzy match contributed. */
	int32 BestDistance = MAX_int32;
};

/**
 * Shared fuzzy text-matching primitives used by domain search actions.
 *
 * This owns only the genuinely identical primitives: normalization,
 * tokenization, bounded edit distance, typo gating, weighted token scoring,
 * and per-field composition. Callers retain corpus, field, threshold,
 * cross-field bonus, and output policy.
 */
class MONOLITHCORE_API FMonolithFuzzyMatch
{
public:
	/** Lowercase, fold a few code tokens, strip non-alphanumeric characters, and collapse whitespace. */
	static FString NormalizeText(FString Text);

	/**
	 * Normalize, split, drop stopwords and short tokens, then deduplicate.
	 * Optional aliases expand from the original token snapshot only.
	 */
	static TArray<FString> Tokenize(
		const FString& Text,
		const TMap<FString, TArray<FString>>* AliasTable = nullptr);

	/**
	 * Banded Levenshtein distance with an early-out above MaxDistance.
	 * bAllowTransposition switches to the optimal-string-alignment variant.
	 */
	static int32 EditDistanceBounded(
		const FString& A,
		const FString& B,
		int32 MaxDistance,
		bool bCaseInsensitive = false,
		bool bAllowTransposition = false);

	/** Typo gate for tokens of at least four characters with the same first character. */
	static bool IsTypoMatch(
		const FString& QueryToken,
		const FString& FieldToken,
		bool bAllowTransposition = false);

	/** Score query tokens against one field and append deterministic match evidence. */
	static int32 ScoreTokens(
		const TArray<FString>& QueryTokens,
		const TArray<FString>& FieldTokens,
		const FString& FieldText,
		const FMonolithFuzzyWeights& Weights,
		const TCHAR* Reason,
		TArray<FString>& OutReasons,
		TSet<FString>& OutMatchedTokens,
		int32* OutBestDistance = nullptr,
		bool bAllowTransposition = false);

	/** Compose phrase and token scores across fields; cross-field bonuses remain caller policy. */
	static FMonolithFuzzyScore ScoreCandidate(
		const FString& QueryNormalized,
		const TArray<FString>& QueryTokens,
		TArrayView<const FMonolithFuzzyField> Fields,
		bool bAllowTransposition = false);
};

/**
 * Registry-oriented edit-distance scorer retained for unknown namespace/action
 * suggestions. Callers must release the registry lock before invoking it.
 */
namespace MonolithFuzzyMatchDetail
{
	struct FFuzzyCandidate
	{
		FString Key;
		float Score = 0.0f;
	};

	MONOLITHCORE_API TArray<FFuzzyCandidate> ScoreFuzzyMatches(
		const FString& Needle,
		const TArray<FString>& KeysSnapshot,
		int32 TopN);
}
