#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

#include <initializer_list>

class UDataflow;
class UPackage;

namespace MonolithDataflow
{
	inline constexpr int32 ErrInvalidParams = -32602;
	inline constexpr int32 MaxNameChars = 256;
	inline constexpr int32 MaxPathChars = 1024;
	inline constexpr int32 MaxTextChars = 4096;
	inline constexpr int32 MaxAssetRows = 500;
	inline constexpr int32 MaxGraphNodes = 500;
	inline constexpr int32 MaxGraphConnections = 5000;
	inline constexpr int32 MaxPinsPerOwner = 500;
	inline constexpr int32 MaxPropertiesPerOwner = 500;
	inline constexpr int32 MaxNodeTypes = 1000;
	inline constexpr int32 MaxValidationIssues = 1000;
	inline constexpr int32 MaxNodeScan = 100000;
	inline constexpr int32 MaxConnectionScan = 250000;
	inline constexpr int32 MaxVariables = 1000;
	inline constexpr int32 MaxComments = 1000;
	inline constexpr int32 MaxCommentNodes = 500;
	inline constexpr int32 MaxCommentGraphNodeScan = 50000;
	inline constexpr int64 MaxCommentMembershipChecks = 1000000;
	inline constexpr int32 MaxOutputRows = 4096;
	inline constexpr int64 MaxOutputTextCharacters = 1024 * 1024;

	class FStrictParamReader
	{
	public:
		explicit FStrictParamReader(const TSharedPtr<FJsonObject>& InParams);

		bool RequiredString(
			const TCHAR* FieldName,
			FString& OutValue,
			int32 MaxChars = MaxPathChars);
		bool OptionalString(
			const TCHAR* FieldName,
			FString& OutValue,
			const FString& DefaultValue = FString(),
			int32 MaxChars = MaxTextChars);
		bool OptionalBool(const TCHAR* FieldName, bool& OutValue, bool DefaultValue);
		bool OptionalInt(
			const TCHAR* FieldName,
			int32& OutValue,
			int32 DefaultValue,
			int32 MinValue,
			int32 MaxValue);
		bool RejectUnknown(std::initializer_list<const TCHAR*> AllowedFields);

		const FString& GetError() const { return Error; }

	private:
		bool ReadExactString(
			const TCHAR* FieldName,
			bool bRequired,
			FString& OutValue,
			const FString& DefaultValue,
			int32 MaxChars);
		bool SetError(const FString& InError);

		TSharedPtr<FJsonObject> Params;
		FString Error;
	};

	class FOutputBudget
	{
	public:
		FString Bound(const FString& Value, int32 MaxChars = MaxTextChars);
		bool TryReserveRow();
		int32 GetTruncatedFieldCount() const { return TruncatedFieldCount; }
		int32 GetReturnedRowCount() const { return ReturnedRowCount; }
		int64 GetReturnedTextCharacterCount() const
		{
			return ReturnedTextCharacterCount;
		}
		bool AreRowsTruncated() const { return bRowsTruncated; }
		bool IsTextTruncatedByAggregateBudget() const
		{
			return bTextTruncatedByAggregateBudget;
		}
		bool IsExhausted() const
		{
			return bRowsTruncated || bTextTruncatedByAggregateBudget;
		}

	private:
		int32 TruncatedFieldCount = 0;
		int32 ReturnedRowCount = 0;
		int64 ReturnedTextCharacterCount = 0;
		bool bRowsTruncated = false;
		bool bTextTruncatedByAggregateBudget = false;
	};

	struct FExactDataflowLoad
	{
		UDataflow* Asset = nullptr;
		UPackage* Package = nullptr;
		FString RequestedPath;
		FString ResolvedPath;
		FString ErrorCode;
		FString ErrorDetail;
		bool bPackageLoadedBefore = false;
		bool bPackageDirtyBefore = false;

		bool IsExact() const { return Asset != nullptr && ErrorCode.IsEmpty(); }
	};

	bool ValidateGamePackagePath(const FString& PackagePath, FString& OutError);
	FExactDataflowLoad LoadExactDataflowAsset(const FString& ObjectPath);
	void AddOutputBudgetFields(
		const TSharedPtr<FJsonObject>& Result,
		const FOutputBudget& OutputBudget);
	FMonolithActionResult FinalizeReadOnlyResult(
		const FExactDataflowLoad& Load,
		const TSharedPtr<FJsonObject>& Result);
	FMonolithActionResult InvalidParams(const FString& Detail);
	FMonolithActionResult ErrorWithCode(
		const FString& Code,
		const FString& Detail,
		const FString& AssetPath = FString());
}
