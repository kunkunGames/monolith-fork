#pragma once

#include "CoreMinimal.h"

namespace MonolithProjectSearchText
{
constexpr int32 PreviewCodePoints = 240;

inline bool IsHighSurrogateCodeUnit(TCHAR Character)
{
	const uint32 CodeUnit = static_cast<uint16>(Character);
	return CodeUnit >= 0xD800 && CodeUnit <= 0xDBFF;
}

inline bool IsLowSurrogateCodeUnit(TCHAR Character)
{
	const uint32 CodeUnit = static_cast<uint16>(Character);
	return CodeUnit >= 0xDC00 && CodeUnit <= 0xDFFF;
}

inline int32 CountUnicodeCodePoints(const FString& Value)
{
	if constexpr (sizeof(TCHAR) == 4)
	{
		return Value.Len();
	}

	int32 Count = 0;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		if (IsHighSurrogateCodeUnit(Value[Index])
			&& Index + 1 < Value.Len()
			&& IsLowSurrogateCodeUnit(Value[Index + 1]))
		{
			++Index;
		}
		++Count;
	}
	return Count;
}

inline FString LeftUnicodeCodePoints(const FString& Value, int32 MaxCodePoints)
{
	if constexpr (sizeof(TCHAR) == 4)
	{
		return Value.Left(FMath::Max(0, MaxCodePoints));
	}

	int32 CodePoints = 0;
	int32 EndIndex = 0;
	while (EndIndex < Value.Len() && CodePoints < MaxCodePoints)
	{
		if (IsHighSurrogateCodeUnit(Value[EndIndex])
			&& EndIndex + 1 < Value.Len()
			&& IsLowSurrogateCodeUnit(Value[EndIndex + 1]))
		{
			EndIndex += 2;
		}
		else
		{
			++EndIndex;
		}
		++CodePoints;
	}
	return Value.Left(EndIndex);
}
}
