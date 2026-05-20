#include "MonolithProjectionUtils.h"

namespace
{
	bool TryReadStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(Name))
		{
			return true;
		}
		if (!Params->TryGetStringField(Name, OutValue))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a string"), Name);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}

	bool TryReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(Name))
		{
			return true;
		}
		if (!Params->TryGetBoolField(Name, OutValue))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a boolean"), Name);
			return false;
		}
		return true;
	}
}

TSharedPtr<FJsonObject> FMonolithProjectionSpec::ToJson() const
{
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Limit);
	Limits->SetNumberField(TEXT("offset"), Offset);
	Limits->SetStringField(TEXT("detail_level"), DetailLevel);
	Limits->SetBoolField(TEXT("include_diagnostics"), bIncludeDiagnostics);
	if (!Cursor.IsEmpty())
	{
		Limits->SetStringField(TEXT("cursor"), Cursor);
	}
	if (MaxChars > 0)
	{
		Limits->SetNumberField(TEXT("max_chars"), MaxChars);
	}
	if (Fields.Num() > 0)
	{
		TArray<FString> FieldList = Fields.Array();
		FieldList.Sort();
		Limits->SetArrayField(TEXT("fields"), FMonolithProjectionUtils::StringsToValues(FieldList));
	}
	return Limits;
}

bool FMonolithProjectionUtils::ReadBoundedIntegerParam(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Name,
	int32 DefaultValue,
	int32 MinValue,
	int32 MaxValue,
	int32& OutValue,
	FString& OutError)
{
	OutValue = FMath::Clamp(DefaultValue, MinValue, MaxValue);
	if (!Params.IsValid() || !Params->HasField(Name))
	{
		return true;
	}

	double RawValue = static_cast<double>(OutValue);
	if (!Params->TryGetNumberField(Name, RawValue))
	{
		OutError = FString::Printf(TEXT("Parameter '%s' must be a number"), Name);
		return false;
	}
	if (!FMath::IsFinite(RawValue))
	{
		OutError = FString::Printf(TEXT("Parameter '%s' must be finite"), Name);
		return false;
	}

	OutValue = FMath::Clamp(FMath::FloorToInt(RawValue), MinValue, MaxValue);
	return true;
}

bool FMonolithProjectionUtils::ReadCursorOffset(const TSharedPtr<FJsonObject>& Params, int32& OutOffset, FString& OutError)
{
	OutOffset = 0;
	FString Cursor;
	if (!TryReadStringParam(Params, TEXT("cursor"), Cursor, OutError))
	{
		return false;
	}
	if (Cursor.IsEmpty())
	{
		return true;
	}
	if (!Cursor.IsNumeric())
	{
		OutError = TEXT("Parameter 'cursor' must be a non-negative integer string");
		return false;
	}
	OutOffset = FMath::Max(0, FCString::Atoi(*Cursor));
	return true;
}

bool FMonolithProjectionUtils::ReadFields(const TSharedPtr<FJsonObject>& Params, TSet<FString>& OutFields, FString& OutError)
{
	OutFields.Reset();
	if (!Params.IsValid() || !Params->HasField(TEXT("fields")))
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* FieldValues = nullptr;
	if (Params->TryGetArrayField(TEXT("fields"), FieldValues) && FieldValues)
	{
		for (const TSharedPtr<FJsonValue>& FieldValue : *FieldValues)
		{
			FString Field;
			if (!FieldValue.IsValid() || !FieldValue->TryGetString(Field))
			{
				OutError = TEXT("Parameter 'fields' must be an array of strings");
				return false;
			}
			Field.TrimStartAndEndInline();
			if (!Field.IsEmpty())
			{
				OutFields.Add(Field);
			}
		}
		return true;
	}

	FString FieldList;
	if (Params->TryGetStringField(TEXT("fields"), FieldList))
	{
		TArray<FString> Parts;
		FieldList.ParseIntoArray(Parts, TEXT(","), true);
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty())
			{
				OutFields.Add(Part);
			}
		}
		return true;
	}

	OutError = TEXT("Parameter 'fields' must be an array of strings or a comma-separated string");
	return false;
}

bool FMonolithProjectionUtils::ReadProjection(
	const TSharedPtr<FJsonObject>& Params,
	FMonolithProjectionSpec& OutSpec,
	FString& OutError,
	int32 DefaultLimit,
	int32 MaxLimit,
	int32 DefaultMaxChars,
	int32 MaxCharsLimit)
{
	OutSpec = FMonolithProjectionSpec();
	OutSpec.Limit = FMath::Clamp(DefaultLimit, 1, MaxLimit);
	OutSpec.MaxChars = FMath::Clamp(DefaultMaxChars, 0, MaxCharsLimit);

	if (!ReadBoundedIntegerParam(Params, TEXT("limit"), OutSpec.Limit, 1, MaxLimit, OutSpec.Limit, OutError))
	{
		return false;
	}
	if (Params.IsValid() && !Params->HasField(TEXT("limit")) && Params->HasField(TEXT("max_results")))
	{
		if (!ReadBoundedIntegerParam(Params, TEXT("max_results"), OutSpec.Limit, 1, MaxLimit, OutSpec.Limit, OutError))
		{
			return false;
		}
	}

	if (!ReadCursorOffset(Params, OutSpec.Offset, OutError)
		|| !ReadFields(Params, OutSpec.Fields, OutError)
		|| !TryReadStringParam(Params, TEXT("detail_level"), OutSpec.DetailLevel, OutError)
		|| !TryReadBoolParam(Params, TEXT("include_diagnostics"), OutSpec.bIncludeDiagnostics, OutError))
	{
		return false;
	}

	if (!OutSpec.DetailLevel.IsEmpty())
	{
		OutSpec.DetailLevel.ToLowerInline();
	}
	if (OutSpec.DetailLevel.IsEmpty())
	{
		OutSpec.DetailLevel = TEXT("minimal");
	}
	if (OutSpec.DetailLevel != TEXT("minimal") && OutSpec.DetailLevel != TEXT("standard") && OutSpec.DetailLevel != TEXT("full"))
	{
		OutError = TEXT("Parameter 'detail_level' must be 'minimal', 'standard', or 'full'");
		return false;
	}

	if (Params.IsValid() && Params->HasField(TEXT("max_chars")))
	{
		if (!ReadBoundedIntegerParam(Params, TEXT("max_chars"), OutSpec.MaxChars > 0 ? OutSpec.MaxChars : MaxCharsLimit, 1, MaxCharsLimit, OutSpec.MaxChars, OutError))
		{
			return false;
		}
	}

	FString Cursor;
	if (!TryReadStringParam(Params, TEXT("cursor"), Cursor, OutError))
	{
		return false;
	}
	OutSpec.Cursor = Cursor;
	return true;
}

TSharedPtr<FJsonObject> FMonolithProjectionUtils::ProjectObject(const TSharedPtr<FJsonObject>& Source, const TSet<FString>& Fields)
{
	if (!Source.IsValid() || Fields.Num() == 0)
	{
		return Source;
	}

	TSharedPtr<FJsonObject> Projected = MakeShared<FJsonObject>();
	for (const FString& Field : Fields)
	{
		TSharedPtr<FJsonValue> Value = Source->TryGetField(Field);
		if (Value.IsValid())
		{
			Projected->SetField(Field, Value);
		}
	}
	return Projected;
}

TArray<TSharedPtr<FJsonValue>> FMonolithProjectionUtils::ObjectsToValues(
	const TArray<TSharedPtr<FJsonObject>>& Objects,
	int32 Offset,
	int32 Limit,
	const TSet<FString>& Fields,
	bool& bOutTruncated,
	FString& OutNextCursor)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	const int32 SafeOffset = FMath::Clamp(Offset, 0, Objects.Num());
	const int32 EndIndex = FMath::Min(SafeOffset + FMath::Max(0, Limit), Objects.Num());
	Values.Reserve(FMath::Max(0, EndIndex - SafeOffset));

	for (int32 Index = SafeOffset; Index < EndIndex; ++Index)
	{
		Values.Add(MakeShared<FJsonValueObject>(ProjectObject(Objects[Index], Fields)));
	}

	bOutTruncated = EndIndex < Objects.Num();
	OutNextCursor = bOutTruncated ? FString::FromInt(EndIndex) : FString();
	return Values;
}

TArray<TSharedPtr<FJsonValue>> FMonolithProjectionUtils::StringsToValues(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Strings.Num());
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

void FMonolithProjectionUtils::ApplyPagingFields(
	const TSharedPtr<FJsonObject>& Result,
	int32 TotalCount,
	int32 ReturnedCount,
	bool bTruncated,
	const FString& NextCursor)
{
	if (!Result.IsValid())
	{
		return;
	}
	Result->SetNumberField(TEXT("count"), ReturnedCount);
	Result->SetNumberField(TEXT("total_count"), TotalCount);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	if (!NextCursor.IsEmpty())
	{
		Result->SetStringField(TEXT("next_cursor"), NextCursor);
	}
}

TSharedPtr<FJsonObject> FMonolithProjectionUtils::MakeResult(
	const FString& Status,
	const TSharedPtr<FJsonObject>& Input,
	const FMonolithProjectionSpec& Projection,
	int32 Count,
	bool bTruncated,
	const FString& NextCursor,
	const TArray<FString>& NextActions)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), Status);
	Result->SetBoolField(TEXT("success"), Status != TEXT("error"));
	if (Input.IsValid())
	{
		Result->SetObjectField(TEXT("input"), Input);
	}
	Result->SetObjectField(TEXT("limits"), Projection.ToJson());
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	if (!NextCursor.IsEmpty())
	{
		Result->SetStringField(TEXT("next_cursor"), NextCursor);
	}
	if (NextActions.Num() > 0)
	{
		Result->SetArrayField(TEXT("next_actions"), StringsToValues(NextActions));
	}
	return Result;
}
