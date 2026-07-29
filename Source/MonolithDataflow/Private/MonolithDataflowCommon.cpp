#include "MonolithDataflowCommon.h"

#include "Dataflow/DataflowObject.h"
#include "Misc/PackageName.h"
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
			bool bKnown = false;
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

	FString FTextBudget::Bound(const FString& Value, int32 MaxChars)
	{
		if (Value.Len() <= MaxChars)
		{
			return Value;
		}

		++TruncatedFieldCount;
		if (MaxChars <= 3)
		{
			return Value.Left(FMath::Max(0, MaxChars));
		}
		return Value.Left(MaxChars - 3) + TEXT("...");
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
			Result.bPackageLoadedBefore = true;
			Result.bPackageDirtyBefore = ExistingPackage->IsDirty();
		}

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
			return Result;
		}

		Result.ResolvedPath = Object->GetPathName();
		if (!Result.ResolvedPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_mismatch");
			Result.ErrorDetail = FString::Printf(
				TEXT("Loaded object path '%s' does not exactly match requested path '%s'"),
				*Result.ResolvedPath,
				*ObjectPath);
			return Result;
		}
		if (Object->IsA<UObjectRedirector>())
		{
			Result.ErrorCode = TEXT("redirector_rejected");
			Result.ErrorDetail = FString::Printf(
				TEXT("asset_path resolves to a redirector: %s"),
				*ObjectPath);
			return Result;
		}

		Result.Asset = Cast<UDataflow>(Object);
		if (!Result.Asset)
		{
			Result.ErrorCode = TEXT("wrong_asset_type");
			Result.ErrorDetail = FString::Printf(
				TEXT("Object '%s' is %s, not UDataflow"),
				*ObjectPath,
				*Object->GetClass()->GetPathName());
			return Result;
		}

		Result.Package = Result.Asset->GetOutermost();
		if (!Result.bPackageLoadedBefore && Result.Package)
		{
			Result.bPackageDirtyBefore = false;
		}
		if (!Result.bPackageDirtyBefore && Result.Package && Result.Package->IsDirty())
		{
			Result.ErrorCode = TEXT("read_only_load_dirtied_package");
			Result.ErrorDetail = FString::Printf(
				TEXT("Loading Dataflow asset '%s' dirtied its package"),
				*ObjectPath);
			Result.Asset = nullptr;
			return Result;
		}

		return Result;
	}

	FMonolithActionResult FinalizeReadOnlyResult(
		const FExactDataflowLoad& Load,
		const TSharedPtr<FJsonObject>& Result)
	{
		const bool bDirtyAfter = Load.Package && Load.Package->IsDirty();
		if (!Load.bPackageDirtyBefore && bDirtyAfter)
		{
			return ErrorWithCode(
				TEXT("read_only_postcondition_failed"),
				FString::Printf(
					TEXT("Reading Dataflow asset '%s' dirtied its package"),
					*Load.RequestedPath),
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
