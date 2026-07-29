#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UObject;

namespace MonolithGameplayMessage
{
	inline constexpr int32 ErrInvalidParams = -32602;
	inline constexpr int32 MaxSourceRoots = 256;
	inline constexpr int32 MaxSourceFiles = 5000;
	inline constexpr int32 DefaultSourceFiles = 2000;
	inline constexpr int32 MaxTraceResults = 1000;
	inline constexpr int32 DefaultTraceResults = 500;
	inline constexpr int64 MaxSourceFileBytes = 2 * 1024 * 1024;
	inline constexpr int32 MaxCandidatesPerLine = 32;
	inline constexpr int32 MaxIssues = 1000;
	inline constexpr int32 MaxJsonTextChars = 4096;

	class FStrictParamReader
	{
	public:
		explicit FStrictParamReader(const TSharedPtr<FJsonObject>& InParams);

		bool RequiredString(const TCHAR* FieldName, FString& OutValue);
		bool OptionalString(const TCHAR* FieldName, FString& OutValue, const FString& DefaultValue = FString());
		bool OptionalBool(const TCHAR* FieldName, bool& OutValue, bool DefaultValue);
		bool OptionalInt(const TCHAR* FieldName, int32& OutValue, int32 DefaultValue, int32 MinValue, int32 MaxValue);
		bool OptionalStringArray(const TCHAR* FieldName, TArray<FString>& OutValues, int32 MaxValues);

		const FString& GetError() const { return Error; }

	private:
		bool ReadExactString(const TCHAR* FieldName, bool bRequired, FString& OutValue, const FString& DefaultValue);
		bool SetError(const FString& InError);

		TSharedPtr<FJsonObject> Params;
		FString Error;
	};

	struct FExactObjectLoad
	{
		UObject* Object = nullptr;
		FString RequestedPath;
		FString ResolvedPath;
		FString ErrorCode;
		FString ErrorDetail;

		bool IsExact() const { return Object != nullptr && ErrorCode.IsEmpty(); }
	};

	FExactObjectLoad LoadExactObjectPath(const FString& ObjectPath);
	bool IsCanonicalGameplayTagString(const FString& TagString, FString& OutError);
	bool ResolveProjectSourceRoot(const FString& Input, FString& OutResolvedRoot, FString& OutError);
	bool ResolveEngineGameplayMessageSourceRoot(FString& OutResolvedRoot, FString& OutError);
	bool IsPathWithinDirectory(const FString& Path, const FString& Directory);
	bool IsMonolithSourcePath(const FString& Path);
	bool HasSupportedSourceExtension(const FString& File);
	FString MakeProjectRelativePath(const FString& File);
	FString BoundText(FString Value, int32 MaxChars = MaxJsonTextChars);
	TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values);
}
