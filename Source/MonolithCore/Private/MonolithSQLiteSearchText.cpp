#include "MonolithSQLiteSearchText.h"

namespace
{
bool IsIdentifierChar(TCHAR Ch)
{
	return FChar::IsAlnum(Ch);
}

bool IsCamelBoundary(const FString& Text, int32 Index)
{
	if (Index <= 0 || Index >= Text.Len())
	{
		return false;
	}

	const TCHAR Prev = Text[Index - 1];
	const TCHAR Curr = Text[Index];
	const TCHAR Next = Index + 1 < Text.Len() ? Text[Index + 1] : 0;

	return (FChar::IsLower(Prev) && FChar::IsUpper(Curr)) ||
		(FChar::IsUpper(Prev) && FChar::IsUpper(Curr) && Next != 0 && FChar::IsLower(Next));
}

void AddToken(const FString& Token, TArray<FString>& OutTokens, TSet<FString>& Seen)
{
	if (Token.Len() <= 1)
	{
		return;
	}

	const FString Key = Token.ToLower();
	if (!Seen.Contains(Key))
	{
		Seen.Add(Key);
		OutTokens.Add(Token);
	}
}
}

FString BuildMonolithSQLiteSearchText(const FString& Text)
{
	TArray<FString> Tokens;
	TSet<FString> Seen;

	FString Current;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Ch = Text[Index];
		if (!IsIdentifierChar(Ch))
		{
			AddToken(Current, Tokens, Seen);
			Current.Reset();
			continue;
		}

		if (IsCamelBoundary(Text, Index))
		{
			AddToken(Current, Tokens, Seen);
			Current.Reset();
		}

		Current.AppendChar(Ch);
	}
	AddToken(Current, Tokens, Seen);

	if (Tokens.Num() == 0)
	{
		return Text;
	}

	FString Expanded = Text;
	for (const FString& Token : Tokens)
	{
		if (!Text.Equals(Token, ESearchCase::IgnoreCase))
		{
			Expanded.AppendChar(' ');
			Expanded.Append(Token);
		}
	}
	return Expanded;
}
