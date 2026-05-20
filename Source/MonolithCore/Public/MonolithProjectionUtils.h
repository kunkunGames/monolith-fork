#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Shared projection/result helpers for action handlers.
 *
 * Domain handlers should return structured JSON objects. The MCP transport
 * layer is responsible for building protocol-level content[] envelopes.
 */
struct MONOLITHCORE_API FMonolithProjectionSpec
{
	int32 Limit = 100;
	int32 Offset = 0;
	int32 MaxChars = 0;
	FString Cursor;
	FString DetailLevel = TEXT("minimal");
	TSet<FString> Fields;
	bool bIncludeDiagnostics = false;

	bool HasFields() const { return Fields.Num() > 0; }
	bool WantsField(const FString& Field) const { return Fields.Num() == 0 || Fields.Contains(Field); }
	TSharedPtr<FJsonObject> ToJson() const;
};

class MONOLITHCORE_API FMonolithProjectionUtils
{
public:
	static bool ReadProjection(
		const TSharedPtr<FJsonObject>& Params,
		FMonolithProjectionSpec& OutSpec,
		FString& OutError,
		int32 DefaultLimit = 100,
		int32 MaxLimit = 1000,
		int32 DefaultMaxChars = 0,
		int32 MaxCharsLimit = 200000);

	static bool ReadBoundedIntegerParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Name,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FString& OutError);

	static bool ReadCursorOffset(const TSharedPtr<FJsonObject>& Params, int32& OutOffset, FString& OutError);
	static bool ReadFields(const TSharedPtr<FJsonObject>& Params, TSet<FString>& OutFields, FString& OutError);

	static TSharedPtr<FJsonObject> ProjectObject(const TSharedPtr<FJsonObject>& Source, const TSet<FString>& Fields);

	static TArray<TSharedPtr<FJsonValue>> ObjectsToValues(
		const TArray<TSharedPtr<FJsonObject>>& Objects,
		int32 Offset,
		int32 Limit,
		const TSet<FString>& Fields,
		bool& bOutTruncated,
		FString& OutNextCursor);

	static TArray<TSharedPtr<FJsonValue>> StringsToValues(const TArray<FString>& Strings);

	static void ApplyPagingFields(
		const TSharedPtr<FJsonObject>& Result,
		int32 TotalCount,
		int32 ReturnedCount,
		bool bTruncated,
		const FString& NextCursor);

	static TSharedPtr<FJsonObject> MakeResult(
		const FString& Status,
		const TSharedPtr<FJsonObject>& Input,
		const FMonolithProjectionSpec& Projection,
		int32 Count,
		bool bTruncated,
		const FString& NextCursor,
		const TArray<FString>& NextActions = TArray<FString>());
};
