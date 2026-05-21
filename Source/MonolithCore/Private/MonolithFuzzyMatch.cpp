// Copyright tumourlove. All Rights Reserved.
#include "MonolithFuzzyMatch.h"

namespace
{
	bool IsTokenChar(TCHAR Ch)
	{
		return FChar::IsAlnum(Ch);
	}

	bool IsIgnoredToken(const FString& Token)
	{
		static const TSet<FString> IgnoredTokens = {
			TEXT("a"), TEXT("an"), TEXT("and"), TEXT("for"), TEXT("from"), TEXT("in"), TEXT("of"),
			TEXT("on"), TEXT("or"), TEXT("the"), TEXT("to"), TEXT("with"), TEXT("using"), TEXT("use")
		};
		return IgnoredTokens.Contains(Token);
	}

	void AddUniqueToken(TArray<FString>& Tokens, TSet<FString>& Seen, const FString& Token)
	{
		if (Token.Len() < 2 || IsIgnoredToken(Token) || Seen.Contains(Token))
		{
			return;
		}
		Seen.Add(Token);
		Tokens.Add(Token);
	}

	// Typo eligibility + allowed max edit distance, or -1 when ineligible.
	// Mirrors the legacy IsFindTypoMatch gate so scoring stays byte-for-byte.
	int32 GetTypoMaxDistance(const FString& QueryToken, const FString& FieldToken)
	{
		if (QueryToken.Len() < 4 || FieldToken.Len() < 4)
		{
			return -1;
		}
		if (QueryToken[0] != FieldToken[0])
		{
			return -1;
		}
		return FMath::Max(QueryToken.Len(), FieldToken.Len()) >= 7 ? 2 : 1;
	}
}

FString FMonolithFuzzyMatch::NormalizeText(FString Text)
{
	Text.ToLowerInline();
	Text.ReplaceInline(TEXT("c++"), TEXT(" cpp "));
	Text.ReplaceInline(TEXT("c#"), TEXT(" csharp "));
	Text.ReplaceInline(TEXT("blueprintassist"), TEXT(" blueprint assist "));

	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		if (!IsTokenChar(Text[Index]))
		{
			Text[Index] = TEXT(' ');
		}
	}

	TArray<FString> Tokens;
	Text.ParseIntoArrayWS(Tokens);
	return FString::Join(Tokens, TEXT(" "));
}

TArray<FString> FMonolithFuzzyMatch::Tokenize(const FString& Text, const TMap<FString, TArray<FString>>* AliasTable)
{
	TArray<FString> RawTokens;
	NormalizeText(Text).ParseIntoArrayWS(RawTokens);

	TArray<FString> Tokens;
	TSet<FString> Seen;
	for (const FString& Token : RawTokens)
	{
		AddUniqueToken(Tokens, Seen, Token);
	}

	if (AliasTable)
	{
		// Snapshot so freshly added aliases are not themselves re-expanded.
		const TArray<FString> Snapshot = Tokens;
		for (const FString& Token : Snapshot)
		{
			if (const TArray<FString>* Expansions = AliasTable->Find(Token))
			{
				for (const FString& Alias : *Expansions)
				{
					AddUniqueToken(Tokens, Seen, Alias);
				}
			}
		}
	}

	return Tokens;
}

int32 FMonolithFuzzyMatch::EditDistanceBounded(const FString& A, const FString& B, int32 MaxDistance, bool bCaseInsensitive)
{
	const int32 La = A.Len();
	const int32 Lb = B.Len();
	if (FMath::Abs(La - Lb) > MaxDistance)
	{
		return MaxDistance + 1;
	}
	if (La == 0)
	{
		return Lb;
	}
	if (Lb == 0)
	{
		return La;
	}

	TArray<int32> Prev;
	TArray<int32> Curr;
	Prev.SetNumUninitialized(Lb + 1);
	Curr.SetNumUninitialized(Lb + 1);
	for (int32 J = 0; J <= Lb; ++J)
	{
		Prev[J] = J;
	}

	for (int32 I = 1; I <= La; ++I)
	{
		Curr[0] = I;
		int32 RowMin = Curr[0];
		const TCHAR Ca = bCaseInsensitive ? FChar::ToLower(A[I - 1]) : A[I - 1];
		for (int32 J = 1; J <= Lb; ++J)
		{
			const TCHAR Cb = bCaseInsensitive ? FChar::ToLower(B[J - 1]) : B[J - 1];
			const int32 Cost = (Ca == Cb) ? 0 : 1;
			Curr[J] = FMath::Min3(
				Prev[J] + 1,
				Curr[J - 1] + 1,
				Prev[J - 1] + Cost);
			RowMin = FMath::Min(RowMin, Curr[J]);
		}
		if (RowMin > MaxDistance)
		{
			return MaxDistance + 1;
		}
		Swap(Prev, Curr);
	}

	return Prev[Lb];
}

bool FMonolithFuzzyMatch::IsTypoMatch(const FString& QueryToken, const FString& FieldToken)
{
	const int32 MaxDistance = GetTypoMaxDistance(QueryToken, FieldToken);
	if (MaxDistance < 0)
	{
		return false;
	}
	return EditDistanceBounded(QueryToken, FieldToken, MaxDistance) <= MaxDistance;
}

int32 FMonolithFuzzyMatch::ScoreTokens(
	const TArray<FString>& QueryTokens,
	const TArray<FString>& FieldTokens,
	const FString& FieldText,
	const FMonolithFuzzyWeights& Weights,
	const TCHAR* Reason,
	TArray<FString>& OutReasons,
	TSet<FString>& OutMatchedTokens,
	int32* OutBestDistance)
{
	int32 Score = 0;
	bool bAnyMatch = false;
	bool bAnyFuzzyMatch = false;

	for (const FString& Token : QueryTokens)
	{
		bool bMatched = false;
		if (FieldTokens.Contains(Token))
		{
			Score += Weights.Exact;
			bMatched = true;
		}
		else
		{
			for (const FString& FieldToken : FieldTokens)
			{
				if (FieldToken.StartsWith(Token))
				{
					Score += Weights.Prefix;
					bMatched = true;
					break;
				}
			}
		}

		if (!bMatched && FieldText.Contains(Token))
		{
			Score += Weights.Contains;
			bMatched = true;
		}

		if (!bMatched && Weights.Fuzzy > 0)
		{
			for (const FString& FieldToken : FieldTokens)
			{
				const int32 MaxDistance = GetTypoMaxDistance(Token, FieldToken);
				if (MaxDistance < 0)
				{
					continue;
				}
				const int32 Distance = EditDistanceBounded(Token, FieldToken, MaxDistance);
				if (Distance <= MaxDistance)
				{
					Score += Weights.Fuzzy;
					bMatched = true;
					bAnyFuzzyMatch = true;
					if (OutBestDistance && Distance < *OutBestDistance)
					{
						*OutBestDistance = Distance;
					}
					break;
				}
			}
		}

		if (bMatched)
		{
			bAnyMatch = true;
			OutMatchedTokens.Add(Token);
		}
	}

	if (bAnyMatch)
	{
		OutReasons.Add(Reason);
	}
	if (bAnyFuzzyMatch)
	{
		OutReasons.Add(FString::Printf(TEXT("%s_fuzzy"), Reason));
	}
	return Score;
}

FMonolithFuzzyScore FMonolithFuzzyMatch::ScoreCandidate(
	const FString& QueryNormalized,
	const TArray<FString>& QueryTokens,
	TArrayView<const FMonolithFuzzyField> Fields)
{
	FMonolithFuzzyScore Result;

	for (const FMonolithFuzzyField& Field : Fields)
	{
		const FString Stem = Field.ReasonTag ? FString(Field.ReasonTag) : FString(TEXT("field"));

		if (!QueryNormalized.IsEmpty())
		{
			if (Field.ExactPhraseBonus != 0 && Field.Text == QueryNormalized)
			{
				Result.Score += Field.ExactPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_exact"));
			}
			else if (Field.PrefixPhraseBonus != 0 && Field.Text.StartsWith(QueryNormalized))
			{
				Result.Score += Field.PrefixPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_prefix"));
			}
			else if (Field.ContainsPhraseBonus != 0 && Field.Text.Contains(QueryNormalized))
			{
				Result.Score += Field.ContainsPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_phrase"));
			}
		}

		TSet<FString> Matched;
		const TCHAR* TokenReason = Field.ReasonTag ? Field.ReasonTag : TEXT("field");
		Result.Score += ScoreTokens(
			QueryTokens, Field.Tokens, Field.Text, Field.Weights,
			TokenReason, Result.Reasons, Matched, &Result.BestDistance);

		for (const FString& Token : Matched)
		{
			Result.MatchedTokens.AddUnique(Token);
		}
	}

	return Result;
}
