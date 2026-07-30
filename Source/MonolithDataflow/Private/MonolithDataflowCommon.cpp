#include "MonolithDataflowCommon.h"

#include "Dataflow/DataflowObject.h"
#include "Misc/PackageName.h"
#include "MonolithParamSchema.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithDataflow
{
	FStrictParamReader::FStrictParamReader(const TSharedPtr<FJsonObject>& InParams)
		: Params(InParams.IsValid() ? InParams : MakeShared<FJsonObject>())
	{
	}

	bool FStrictParamReader::SetError(const FString& InError)
	{
		if (Error.IsEmpty())
		{
			Error = InError;
		}
		return false;
	}

	bool FStrictParamReader::ReadExactString(
		const TCHAR* FieldName,
		bool bRequired,
		FString& OutValue,
		const FString& DefaultValue,
		int32 MaxChars)
	{
		if (!Error.IsEmpty())
		{
			return false;
		}
		if (!Params->HasField(FieldName))
		{
			if (bRequired)
			{
				return SetError(FString::Printf(TEXT("Missing required param '%s'"), FieldName));
			}
			OutValue = DefaultValue;
			return true;
		}

		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::String
			|| !(*FieldValue)->TryGetString(OutValue))
		{
			return SetError(FString::Printf(TEXT("Param '%s' must be a string"), FieldName));
		}
		if (!OutValue.Equals(OutValue.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' must not contain leading or trailing whitespace"),
				FieldName));
		}
		if (bRequired && OutValue.IsEmpty())
		{
			return SetError(FString::Printf(TEXT("Param '%s' must not be empty"), FieldName));
		}
		if (OutValue.Len() > MaxChars)
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' may contain at most %d characters"),
				FieldName,
				MaxChars));
		}
		return true;
	}

	bool FStrictParamReader::RequiredString(
		const TCHAR* FieldName,
		FString& OutValue,
		int32 MaxChars)
	{
		return ReadExactString(FieldName, true, OutValue, FString(), MaxChars);
	}

	bool FStrictParamReader::OptionalString(
		const TCHAR* FieldName,
		FString& OutValue,
		const FString& DefaultValue,
		int32 MaxChars)
	{
		return ReadExactString(FieldName, false, OutValue, DefaultValue, MaxChars);
	}

	bool FStrictParamReader::OptionalBool(
		const TCHAR* FieldName,
		bool& OutValue,
		bool DefaultValue)
	{
		if (!Error.IsEmpty())
		{
			return false;
		}
		if (!Params->HasField(FieldName))
		{
			OutValue = DefaultValue;
			return true;
		}

		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::Boolean
			|| !(*FieldValue)->TryGetBool(OutValue))
		{
			return SetError(FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName));
		}
		return true;
	}

	bool FStrictParamReader::OptionalInt(
		const TCHAR* FieldName,
		int32& OutValue,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue)
	{
		if (!Error.IsEmpty())
		{
			return false;
		}
		if (!Params->HasField(FieldName))
		{
			OutValue = DefaultValue;
			return true;
		}

		double Number = 0.0;
		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::Number
			|| !(*FieldValue)->TryGetNumber(Number)
			|| !FMath::IsFinite(Number)
			|| FMath::TruncToDouble(Number) != Number
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue))
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' must be an integer in range %d..%d"),
				FieldName,
				MinValue,
				MaxValue));
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool FStrictParamReader::RejectUnknown(
		std::initializer_list<const TCHAR*> AllowedFields)
	{
		if (!Error.IsEmpty())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			bool bKnown =
				FMonolithParamSchema::IsUniversalResponseShapingParam(Pair.Key);
			for (const TCHAR* AllowedField : AllowedFields)
			{
				if (Pair.Key.Equals(AllowedField, ESearchCase::CaseSensitive))
				{
					bKnown = true;
					break;
				}
			}
			if (!bKnown)
			{
				return SetError(FString::Printf(TEXT("Unknown param '%s'"), *Pair.Key));
			}
		}
		return true;
	}

	namespace
	{
		/**
		 * Returns a cut length no greater than DesiredLength that never slices a
		 * UTF-16 surrogate pair. On Windows TCHAR is UTF-16, so cutting between a
		 * high and low surrogate would emit an unpaired surrogate that cannot be
		 * encoded as UTF-8 during JSON serialization.
		 */
		int32 SurrogateSafeCutLength(const FString& Value, int32 DesiredLength)
		{
			if (DesiredLength <= 0 || DesiredLength >= Value.Len())
			{
				return FMath::Clamp(DesiredLength, 0, Value.Len());
			}

			const TCHAR LastKeptChar = Value[DesiredLength - 1];
			const uint32 LastKeptCodeUnit = static_cast<uint32>(LastKeptChar);
			const bool bLastKeptIsHighSurrogate =
				LastKeptCodeUnit >= 0xD800u && LastKeptCodeUnit <= 0xDBFFu;
			return bLastKeptIsHighSurrogate ? DesiredLength - 1 : DesiredLength;
		}
	}

	FString FOutputBudget::Bound(const FString& Value, int32 MaxChars)
	{
		const int32 PerFieldCharacterCount =
			FMath::Min(Value.Len(), FMath::Max(0, MaxChars));
		const int64 RemainingAggregateCharacters =
			FMath::Max<int64>(
				0,
				MaxOutputTextCharacters - ReturnedTextCharacterCount);
		const int32 ReturnedCharacterCount = static_cast<int32>(
			FMath::Min<int64>(
				PerFieldCharacterCount,
				RemainingAggregateCharacters));
		const bool bPerFieldTruncated = Value.Len() > PerFieldCharacterCount;
		const bool bAggregateTruncated =
			PerFieldCharacterCount > ReturnedCharacterCount;
		if (!bPerFieldTruncated && !bAggregateTruncated)
		{
			ReturnedTextCharacterCount += Value.Len();
			return Value;
		}

		++TruncatedFieldCount;
		bTextTruncatedByAggregateBudget |= bAggregateTruncated;
		FString Bounded;
		if (ReturnedCharacterCount <= 3)
		{
			Bounded = Value.Left(
				SurrogateSafeCutLength(Value, ReturnedCharacterCount));
		}
		else
		{
			Bounded =
				Value.Left(
					SurrogateSafeCutLength(Value, ReturnedCharacterCount - 3))
				+ TEXT("...");
		}
		ReturnedTextCharacterCount += Bounded.Len();
		return Bounded;
	}

	bool FOutputBudget::TryReserveRow()
	{
		if (ReturnedRowCount >= MaxOutputRows)
		{
			bRowsTruncated = true;
			return false;
		}
		++ReturnedRowCount;
		return true;
	}

	bool ValidateGamePackagePath(const FString& PackagePath, FString& OutError)
	{
		OutError.Reset();
		if (PackagePath.IsEmpty())
		{
			OutError = TEXT("package_path must not be empty");
			return false;
		}
		if (PackagePath.Len() > MaxPathChars)
		{
			OutError = FString::Printf(
				TEXT("package_path may contain at most %d characters"),
				MaxPathChars);
			return false;
		}
		if (!PackagePath.Equals(PackagePath.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("package_path must not contain leading or trailing whitespace");
			return false;
		}
		if (PackagePath.Contains(TEXT("\\"))
			|| PackagePath.Contains(TEXT("."))
			|| PackagePath.Contains(TEXT(":")))
		{
			OutError = TEXT("package_path must be a canonical long package directory");
			return false;
		}
		if (!PackagePath.Equals(TEXT("/Game"), ESearchCase::CaseSensitive)
			&& !PackagePath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("package_path must be /Game or a directory below /Game/");
			return false;
		}
		if (PackagePath.Len() > 5 && PackagePath.EndsWith(TEXT("/")))
		{
			OutError = TEXT("package_path must not end with '/'");
			return false;
		}

		FText InvalidReason;
		if (!FPackageName::IsValidLongPackageName(PackagePath, false, &InvalidReason))
		{
			OutError = InvalidReason.ToString();
			return false;
		}
		return true;
	}

	FExactDataflowLoad LoadExactDataflowAsset(const FString& ObjectPath)
	{
		FExactDataflowLoad Result;
		Result.RequestedPath = ObjectPath;

		if (ObjectPath.IsEmpty())
		{
			Result.ErrorCode = TEXT("empty_object_path");
			Result.ErrorDetail = TEXT("asset_path must not be empty");
			return Result;
		}
		if (ObjectPath.Len() > MaxPathChars)
		{
			Result.ErrorCode = TEXT("object_path_too_long");
			Result.ErrorDetail = FString::Printf(
				TEXT("asset_path may contain at most %d characters"),
				MaxPathChars);
			return Result;
		}
		if (!ObjectPath.Equals(ObjectPath.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_whitespace");
			Result.ErrorDetail = TEXT("asset_path must not contain leading or trailing whitespace");
			return Result;
		}
		if (ObjectPath.Contains(TEXT("\\"))
			|| ObjectPath.Contains(TEXT(":"))
			|| ObjectPath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
		{
			Result.ErrorCode = TEXT("object_path_noncanonical");
			Result.ErrorDetail = TEXT("asset_path must be a canonical Unreal object path");
			return Result;
		}

		FText InvalidReason;
		if (!FPackageName::IsValidObjectPath(ObjectPath, &InvalidReason))
		{
			Result.ErrorCode = TEXT("invalid_object_path");
			Result.ErrorDetail = InvalidReason.ToString();
			return Result;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!PackageName.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_outside_game");
			Result.ErrorDetail = TEXT("asset_path must resolve below /Game/");
			return Result;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
		{
			Result.Package = ExistingPackage;
			Result.bPackageLoadedBefore = true;
			Result.bPackageDirtyBefore = ExistingPackage->IsDirty();
		}

		const auto ApplyLoadDirtyStatePostcondition =
			[&Result, &ObjectPath]()
			{
				const bool bPackageDirtyAfter =
					Result.Package && Result.Package->IsDirty();
				if (!Result.Package
					|| bPackageDirtyAfter == Result.bPackageDirtyBefore)
				{
					return;
				}

				Result.ErrorCode =
					!Result.bPackageDirtyBefore && bPackageDirtyAfter
						? TEXT("read_only_load_dirtied_package")
						: TEXT("read_only_load_changed_package_dirty_state");
				Result.ErrorDetail = FString::Printf(
					TEXT("Loading object '%s' changed package dirty state from %s to %s"),
					*ObjectPath,
					Result.bPackageDirtyBefore ? TEXT("dirty") : TEXT("clean"),
					bPackageDirtyAfter ? TEXT("dirty") : TEXT("clean"));
				Result.Asset = nullptr;
			};

		UObject* Object = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPath,
			nullptr,
			LOAD_NoWarn | LOAD_NoRedirects);
		if (!Object)
		{
			Result.ErrorCode = TEXT("object_not_found");
			Result.ErrorDetail = FString::Printf(
				TEXT("No object exists at exact path '%s'"),
				*ObjectPath);
			Result.Package = FindPackage(nullptr, *PackageName);
			ApplyLoadDirtyStatePostcondition();
			return Result;
		}

		Result.Package = Object->GetOutermost();
		if (!Result.bPackageLoadedBefore && Result.Package)
		{
			Result.bPackageDirtyBefore = false;
		}
		Result.ResolvedPath = Object->GetPathName();
		if (!Result.ResolvedPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_mismatch");
			Result.ErrorDetail = FString::Printf(
				TEXT("Loaded object path '%s' does not exactly match requested path '%s'"),
				*Result.ResolvedPath,
				*ObjectPath);
		}
		else if (Object->IsA<UObjectRedirector>())
		{
			Result.ErrorCode = TEXT("redirector_rejected");
			Result.ErrorDetail = FString::Printf(
				TEXT("asset_path resolves to a redirector: %s"),
				*ObjectPath);
		}
		else
		{
			Result.Asset = Cast<UDataflow>(Object);
			if (!Result.Asset)
			{
				Result.ErrorCode = TEXT("wrong_asset_type");
				Result.ErrorDetail = FString::Printf(
					TEXT("Object '%s' is %s, not UDataflow"),
					*ObjectPath,
					*Object->GetClass()->GetPathName());
			}
		}

		ApplyLoadDirtyStatePostcondition();

		return Result;
	}

	void AddOutputBudgetFields(
		const TSharedPtr<FJsonObject>& Result,
		const FOutputBudget& OutputBudget)
	{
		Result->SetNumberField(TEXT("output_row_limit"), MaxOutputRows);
		Result->SetNumberField(
			TEXT("output_returned_row_count"),
			OutputBudget.GetReturnedRowCount());
		Result->SetBoolField(
			TEXT("output_rows_truncated"),
			OutputBudget.AreRowsTruncated());
		Result->SetNumberField(
			TEXT("output_bounded_text_character_limit"),
			static_cast<double>(MaxOutputTextCharacters));
		Result->SetNumberField(
			TEXT("output_returned_bounded_text_character_count"),
			static_cast<double>(
				OutputBudget.GetReturnedTextCharacterCount()));
		Result->SetBoolField(
			TEXT("output_bounded_text_truncated"),
			OutputBudget.IsTextTruncatedByAggregateBudget());
		Result->SetBoolField(
			TEXT("output_budget_exhausted"),
			OutputBudget.IsExhausted());
		Result->SetNumberField(
			TEXT("truncated_text_field_count"),
			OutputBudget.GetTruncatedFieldCount());
	}

	FMonolithActionResult FinalizeReadOnlyResult(
		const FExactDataflowLoad& Load,
		const TSharedPtr<FJsonObject>& Result)
	{
		const bool bDirtyAfter = Load.Package && Load.Package->IsDirty();
		if (Load.Package && bDirtyAfter != Load.bPackageDirtyBefore)
		{
			return ErrorWithCode(
				TEXT("read_only_postcondition_failed"),
				FString::Printf(
					TEXT("Reading Dataflow asset '%s' changed package dirty state from %s to %s"),
					*Load.RequestedPath,
					Load.bPackageDirtyBefore ? TEXT("dirty") : TEXT("clean"),
					bDirtyAfter ? TEXT("dirty") : TEXT("clean")),
				Load.RequestedPath);
		}

		Result->SetBoolField(TEXT("package_loaded_before"), Load.bPackageLoadedBefore);
		Result->SetBoolField(TEXT("package_dirty_before"), Load.bPackageDirtyBefore);
		Result->SetBoolField(TEXT("package_dirty_after"), bDirtyAfter);
		Result->SetBoolField(TEXT("package_dirty_state_preserved"), bDirtyAfter == Load.bPackageDirtyBefore);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult InvalidParams(const FString& Detail)
	{
		return FMonolithActionResult::Error(Detail, ErrInvalidParams);
	}

	FMonolithActionResult ErrorWithCode(
		const FString& Code,
		const FString& Detail,
		const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error"), Code);
		ErrorData->SetStringField(TEXT("detail"), Detail);
		if (!AssetPath.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		}
		return FMonolithActionResult::Error(Detail).WithErrorData(ErrorData);
	}
}
