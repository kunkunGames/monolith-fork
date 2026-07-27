#include "MonolithToolText.h"

FString MonolithToolText::TerseOneLineDescription(const FString& Full)
{
	constexpr int32 HardCap = 150;
	constexpr int32 MinSentence = 25;

	const int32 Len = Full.Len();

	// Prefer a real sentence boundary, but skip punctuation in short forms such
	// as "e.g." and versions such as "5.8".
	int32 SentenceEnd = MAX_int32;
	for (int32 Index = MinSentence; Index < Len; ++Index)
	{
		const TCHAR Ch = Full[Index];
		if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?'))
		{
			const bool bFollowedBySpaceOrEnd =
				(Index + 1 >= Len) || FChar::IsWhitespace(Full[Index + 1]);
			if (bFollowedBySpaceOrEnd)
			{
				SentenceEnd = Index + 1;
				break;
			}
		}
	}

	int32 Cut = FMath::Min(SentenceEnd, HardCap);
	if (Cut >= Len)
	{
		return Full;
	}

	if (Cut == HardCap && !FChar::IsWhitespace(Full[Cut]))
	{
		int32 WordBoundary = Cut;
		while (WordBoundary > 0 && !FChar::IsWhitespace(Full[WordBoundary - 1]))
		{
			--WordBoundary;
		}
		if (WordBoundary > 0)
		{
			Cut = WordBoundary;
		}
	}

	FString Trimmed = Full.Left(Cut);
	int32 Tail = Trimmed.Len();
	while (Tail > 0)
	{
		const TCHAR Ch = Trimmed[Tail - 1];
		if (
			FChar::IsWhitespace(Ch) ||
			Ch == TEXT('.') ||
			Ch == TEXT('!') ||
			Ch == TEXT('?'))
		{
			--Tail;
		}
		else
		{
			break;
		}
	}
	Trimmed.LeftInline(Tail);
	Trimmed += TEXT("...");
	return Trimmed;
}
