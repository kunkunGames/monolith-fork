#pragma once

#include "CoreMinimal.h"
#include "ProjectSearchQueryProjectionCore.h"

namespace MonolithProjectSearchQuery
{
	inline bool IsSyntaxWhitespace(TCHAR Character)
	{
		return Character == TEXT(' ')
			|| Character == TEXT('\t')
			|| Character == TEXT('\n')
			|| Character == TEXT('\r')
			|| Character == TEXT('\v')
			|| Character == TEXT('\f');
	}

	inline void TrimSyntaxWhitespaceInline(FString& Query)
	{
		int32 Begin = 0;
		int32 End = Query.Len();
		while (Begin < End && IsSyntaxWhitespace(Query[Begin]))
		{
			++Begin;
		}
		while (End > Begin && IsSyntaxWhitespace(Query[End - 1]))
		{
			--End;
		}
		if (Begin != 0 || End != Query.Len())
		{
			Query = Query.Mid(Begin, End - Begin);
		}
	}

	enum class EProjectionResult : uint8
	{
		Applicable,
		Inapplicable,
		Invalid
	};

	inline EProjectionResult Project(
		const FString& Query,
		const TArray<FString>& CurrentFields,
		const TSet<FString>& EnabledFields,
		FString& OutQuery,
		FString* OutError = nullptr)
	{
		std::vector<std::string> CurrentUtf8;
		CurrentUtf8.reserve(CurrentFields.Num());
		for (const FString& Field : CurrentFields)
		{
			FTCHARToUTF8 Utf8(*Field);
			CurrentUtf8.emplace_back(Utf8.Get(), Utf8.Length());
		}

		std::set<std::string> EnabledUtf8;
		for (const FString& Field : EnabledFields)
		{
			FTCHARToUTF8 Utf8(*Field);
			EnabledUtf8.emplace(Utf8.Get(), Utf8.Length());
		}

		FTCHARToUTF8 QueryUtf8(*Query);
		const monolith_project_search_query::projection Result =
			monolith_project_search_query::project(
				std::string(QueryUtf8.Get(), QueryUtf8.Length()),
				CurrentUtf8,
				EnabledUtf8);
		OutQuery = UTF8_TO_TCHAR(Result.query.c_str());
		if (OutError)
		{
			*OutError = UTF8_TO_TCHAR(Result.error.c_str());
		}
		switch (Result.result)
		{
		case monolith_project_search_query::projection_result::applicable:
			return EProjectionResult::Applicable;
		case monolith_project_search_query::projection_result::inapplicable:
			return EProjectionResult::Inapplicable;
		case monolith_project_search_query::projection_result::invalid:
		default:
			return EProjectionResult::Invalid;
		}
	}
}
