#include "MonolithLocalizationActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithLocalizationActions
{
	constexpr int32 DefaultCulturePageLimit = 100;
	constexpr int32 MaxCulturePageLimit = 500;
	constexpr int32 MaxExplicitCultureNames = 256;
	constexpr int32 DefaultAssetPageLimit = 200;
	constexpr int32 MaxAssetPageLimit = 1000;
	constexpr int32 DefaultEntryPageLimit = 200;
	constexpr int32 MaxEntryPageLimit = 1000;
	constexpr int32 DefaultMetadataBudget = 512;
	constexpr int32 MaxMetadataBudget = 4096;
	constexpr int32 DefaultTextLimit = 4096;
	constexpr int32 MaxTextLimit = 65536;
	constexpr int32 DefaultValidationScanLimit = 4096;
	constexpr int32 MaxValidationScanLimit = 10000;
	constexpr int32 DefaultIssuePageLimit = 200;
	constexpr int32 MaxIssuePageLimit = 1000;
	constexpr int32 MaxCursorLength = 4096;

	struct FBoundedValue
	{
		FString Value;
		int32 OriginalLength = 0;
	};

	struct FMetadataSnapshot
	{
		FString Name;
		FBoundedValue Value;
	};

	struct FValidationIssue
	{
		FString Code;
		FString Severity;
		FString Message;
		FString Key;
	};

	FMonolithActionResult InvalidParam(const TCHAR* Field, const FString& Detail)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), Field, *Detail),
			-32602);
	}

	bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
	{
		return Params.IsValid() && Params->HasField(Field);
	}

	bool ParseBoundedInteger(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FMonolithActionResult& OutError)
	{
		double Number = static_cast<double>(DefaultValue);
		if (HasParam(Params, Field))
		{
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				OutError = InvalidParam(Field, TEXT("expected an integer JSON number"));
				return false;
			}
		}

		if (!FMath::IsFinite(Number)
			|| Number != FMath::TruncToDouble(Number)
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue))
		{
			OutError = InvalidParam(
				Field,
				FString::Printf(TEXT("expected an integer in the range %d..%d"), MinValue, MaxValue));
			return false;
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool ParseOptionalBool(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		bool DefaultValue,
		bool& OutValue,
		FMonolithActionResult& OutError)
	{
		OutValue = DefaultValue;
		if (!HasParam(Params, Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a boolean"));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		FString& OutValue,
		FMonolithActionResult& OutError)
	{
		if (!HasParam(Params, Field))
		{
			OutError = InvalidParam(Field, TEXT("field is required"));
			return false;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid()
			|| Value->Type != EJson::String
			|| !Value->TryGetString(OutValue)
			|| OutValue.IsEmpty())
		{
			OutError = InvalidParam(Field, TEXT("expected a non-empty string"));
			return false;
		}
		return true;
	}

	bool ReadOptionalString(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		const FString& DefaultValue,
		FString& OutValue,
		bool& bOutProvided,
		FMonolithActionResult& OutError)
	{
		OutValue = DefaultValue;
		bOutProvided = HasParam(Params, Field);
		if (!bOutProvided)
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a string"));
			return false;
		}
		return true;
	}

	bool ParseStringArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		int32 MaxCount,
		TArray<FString>& OutValues,
		bool& bOutProvided,
		FMonolithActionResult& OutError)
	{
		OutValues.Reset();
		bOutProvided = HasParam(Params, Field);
		if (!bOutProvided)
		{
			return true;
		}

		const TSharedPtr<FJsonValue> FieldValue = Params->TryGetField(Field);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!FieldValue.IsValid()
			|| FieldValue->Type != EJson::Array
			|| !FieldValue->TryGetArray(Values)
			|| !Values
			|| Values->IsEmpty())
		{
			OutError = InvalidParam(Field, TEXT("expected a non-empty string array"));
			return false;
		}
		if (Values->Num() > MaxCount)
		{
			OutError = InvalidParam(
				Field,
				FString::Printf(TEXT("at most %d values are accepted"), MaxCount));
			return false;
		}

		TSet<FString> Seen;
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			FString Item;
			if (!(*Values)[Index].IsValid()
				|| (*Values)[Index]->Type != EJson::String
				|| !(*Values)[Index]->TryGetString(Item)
				|| Item.IsEmpty())
			{
				OutError = InvalidParam(
					Field,
					FString::Printf(TEXT("element %d must be a non-empty string"), Index));
				return false;
			}
			FString Trimmed = Item;
			Trimmed.TrimStartAndEndInline();
			if (Trimmed != Item)
			{
				OutError = InvalidParam(
					Field,
					FString::Printf(TEXT("element %d has leading or trailing whitespace"), Index));
				return false;
			}
			if (Seen.Contains(Item))
			{
				OutError = InvalidParam(Field, FString::Printf(TEXT("duplicate value '%s'"), *Item));
				return false;
			}
			Seen.Add(Item);
			OutValues.Add(Item);
		}
		OutValues.Sort();
		return true;
	}

	bool ParsePackageFilter(
		const FString& Input,
		FString& OutFilter,
		FMonolithActionResult& OutError)
	{
		OutFilter = Input;
		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != Input)
		{
			OutError = InvalidParam(TEXT("path"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT(":"))
			|| Input.Contains(TEXT("."))
			|| Input.EndsWith(TEXT("/")))
		{
			OutError = InvalidParam(
				TEXT("path"),
				TEXT("expected a canonical Unreal package prefix such as /Game/Localization"));
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(Input))
		{
			OutError = InvalidParam(TEXT("path"), TEXT("expected a valid mounted long package prefix"));
			return false;
		}
		return true;
	}

	bool ParseAssetPath(
		const FString& Input,
		const TCHAR* Field,
		FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != Input)
		{
			OutError = InvalidParam(Field, TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT(":"))
			|| Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = InvalidParam(
				Field,
				TEXT("expected a canonical Unreal package or top-level object path"));
			return false;
		}

		FString PackagePath;
		FString ObjectName;
		const bool bObjectPath = Input.Split(TEXT("."), &PackagePath, &ObjectName);
		if (!bObjectPath)
		{
			PackagePath = Input;
		}
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			OutError = InvalidParam(Field, TEXT("expected a valid mounted long package name"));
			return false;
		}

		const FString PackageLeaf = FPackageName::GetLongPackageAssetName(PackagePath);
		if (PackageLeaf.IsEmpty())
		{
			OutError = InvalidParam(Field, TEXT("asset name is missing"));
			return false;
		}
		if (bObjectPath && ObjectName != PackageLeaf)
		{
			OutError = InvalidParam(
				Field,
				FString::Printf(
					TEXT("object name '%s' must match package leaf '%s'"),
					*ObjectName,
					*PackageLeaf));
			return false;
		}

		OutObjectPath = PackagePath + TEXT(".") + PackageLeaf;
		if (!FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = InvalidParam(Field, TEXT("expected a valid top-level object path"));
			return false;
		}
		return true;
	}

	UStringTable* LoadStringTable(
		const FString& Input,
		const TCHAR* Field,
		FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		if (!ParseAssetPath(Input, Field, OutObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Object = FSoftObjectPath(OutObjectPath).TryLoad();
		if (!Object)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("Asset not found: %s"), *OutObjectPath));
			return nullptr;
		}
		UStringTable* Table = Cast<UStringTable>(Object);
		if (!Table)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Asset '%s' is %s, expected UStringTable"),
					*OutObjectPath,
					*Object->GetClass()->GetName()));
			return nullptr;
		}
		return Table;
	}

	void SetPageMetadata(
		const TSharedPtr<FJsonObject>& Json,
		int32 Total,
		int32 Offset,
		int32 Limit,
		int32 Count)
	{
		Json->SetNumberField(TEXT("total"), Total);
		Json->SetNumberField(TEXT("offset"), Offset);
		Json->SetNumberField(TEXT("limit"), Limit);
		Json->SetNumberField(TEXT("count"), Count);
		Json->SetBoolField(TEXT("has_more"), static_cast<int64>(Offset) + Count < Total);
	}

	void QueryStringTables(const FString& PackageFilter, TArray<FAssetData>& OutAssets)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*PackageFilter));
		Filter.bRecursivePaths = true;

		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Module.Get().GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});
	}

	TSharedPtr<FJsonObject> CultureToJson(const FCultureRef& Culture)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Culture->GetName());
		Json->SetStringField(TEXT("native_name"), Culture->GetNativeName());
		Json->SetStringField(TEXT("english_name"), Culture->GetEnglishName());
		Json->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
		Json->SetStringField(TEXT("two_letter_iso"), Culture->GetTwoLetterISOLanguageName());
		Json->SetStringField(TEXT("three_letter_iso"), Culture->GetThreeLetterISOLanguageName());
		return Json;
	}

	int32 CountEntries(const UStringTable* Table)
	{
		int32 Count = 0;
		if (Table)
		{
			Table->GetStringTable()->EnumerateKeysAndSourceStrings(
				[&Count](const FTextKey&, const FString&)
				{
					++Count;
					return true;
				});
		}
		return Count;
	}

	TSharedPtr<FJsonObject> TableIdentityToJson(const UStringTable* Table, bool bIncludeEntryCount)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Table)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Table->GetPathName());
		Json->SetStringField(TEXT("package_path"), Table->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Table->GetName());
		Json->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
		Json->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
		Json->SetBoolField(TEXT("is_internal"), Table->GetStringTable()->IsInternal());
		if (bIncludeEntryCount)
		{
			Json->SetNumberField(TEXT("entry_count"), CountEntries(Table));
		}
		return Json;
	}

	void CompactSmallestKeys(TArray<FString>& Keys, int32 KeepCount)
	{
		Keys.Sort();
		if (Keys.Num() > KeepCount)
		{
			Keys.SetNum(KeepCount, EAllowShrinking::No);
		}
	}

	void CollectSmallestKeys(
		const UStringTable* Table,
		const FString* AfterKey,
		int32 KeepCount,
		TArray<FString>& OutKeys,
		int32& OutTotalCount,
		int32& OutEligibleCount)
	{
		OutKeys.Reset();
		OutTotalCount = 0;
		OutEligibleCount = 0;
		if (!Table || KeepCount <= 0)
		{
			return;
		}

		Table->GetStringTable()->EnumerateKeysAndSourceStrings(
			[AfterKey, KeepCount, &OutKeys, &OutTotalCount, &OutEligibleCount](
				const FTextKey& Key,
				const FString&)
			{
				++OutTotalCount;
				const FString KeyString = Key.ToString();
				if (AfterKey && KeyString <= *AfterKey)
				{
					return true;
				}

				++OutEligibleCount;
				OutKeys.Add(KeyString);
				if (OutKeys.Num() >= KeepCount * 2)
				{
					CompactSmallestKeys(OutKeys, KeepCount);
				}
				return true;
			});
		CompactSmallestKeys(OutKeys, KeepCount);
	}

	FBoundedValue BoundValue(const FString& Value, int32 Limit)
	{
		FBoundedValue Result;
		Result.OriginalLength = Value.Len();
		Result.Value = Value.Left(Limit);
		return Result;
	}

	void SetBoundedString(
		const TSharedPtr<FJsonObject>& Json,
		const TCHAR* Field,
		const FBoundedValue& Value)
	{
		Json->SetStringField(Field, Value.Value);
		Json->SetNumberField(FString(Field) + TEXT("_length"), Value.OriginalLength);
		Json->SetBoolField(
			FString(Field) + TEXT("_truncated"),
			Value.Value.Len() < Value.OriginalLength);
	}

	void CompactSmallestMetadata(TArray<FMetadataSnapshot>& Rows, int32 KeepCount)
	{
		Rows.Sort([](const FMetadataSnapshot& A, const FMetadataSnapshot& B)
		{
			return A.Name < B.Name;
		});
		if (Rows.Num() > KeepCount)
		{
			Rows.SetNum(KeepCount, EAllowShrinking::No);
		}
	}

	void CollectMetadata(
		const FStringTableConstRef& StringTable,
		const FString& Key,
		int32 KeepCount,
		int32 TextLimit,
		TArray<FMetadataSnapshot>& OutRows,
		int32& OutTotalCount)
	{
		OutRows.Reset();
		OutTotalCount = 0;
		StringTable->EnumerateMetaData(
			FTextKey(Key),
			[KeepCount, TextLimit, &OutRows, &OutTotalCount](
				FName MetadataId,
				const FString& MetadataValue)
			{
				++OutTotalCount;
				if (KeepCount <= 0)
				{
					return true;
				}
				FMetadataSnapshot Row;
				Row.Name = MetadataId.ToString();
				Row.Value = BoundValue(MetadataValue, TextLimit);
				OutRows.Add(MoveTemp(Row));
				if (OutRows.Num() >= KeepCount * 2)
				{
					CompactSmallestMetadata(OutRows, KeepCount);
				}
				return true;
			});
		CompactSmallestMetadata(OutRows, KeepCount);
	}

	TSharedPtr<FJsonObject> MetadataToJson(const FMetadataSnapshot& Metadata)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Metadata.Name);
		SetBoundedString(Json, TEXT("value"), Metadata.Value);
		return Json;
	}

	TSharedPtr<FJsonObject> IssueToJson(const FValidationIssue& Issue)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("code"), Issue.Code);
		Json->SetStringField(TEXT("severity"), Issue.Severity);
		Json->SetStringField(TEXT("message"), Issue.Message);
		if (!Issue.Key.IsEmpty())
		{
			Json->SetStringField(TEXT("key"), Issue.Key);
		}
		return Json;
	}

	void AddIssue(
		TArray<FValidationIssue>& Issues,
		int32& ErrorCount,
		int32& WarningCount,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Message,
		const FString& Key = FString())
	{
		FValidationIssue Issue;
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.Message = Message;
		Issue.Key = Key;
		Issues.Add(MoveTemp(Issue));
		if (FCString::Strcmp(Severity, TEXT("error")) == 0)
		{
			++ErrorCount;
		}
		else
		{
			++WarningCount;
		}
	}
}

void FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	FMonolithDispatcherAnnotations Annotations;
	Annotations.bReadOnlyHint = true;
	Annotations.bIdempotentHint = true;
	Annotations.Title = TEXT("Localization Discovery and StringTable Validation");
	Registry.SetDispatcherAnnotations(TEXT("localization"), Annotations);

	Registry.RegisterAction(
		TEXT("localization"),
		TEXT("list_cultures"),
		TEXT("List Unreal cultures with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::HandleListCultures),
		FParamSchemaBuilder()
			.Optional(TEXT("culture_names"), TEXT("array"), TEXT("Optional culture roots to resolve; maximum 256"))
			.Optional(TEXT("include_derived"), TEXT("boolean"), TEXT("Include derived cultures for explicit culture_names"), TEXT("true"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum cultures to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(
		TEXT("localization"),
		TEXT("list_string_tables"),
		TEXT("Discover StringTable assets with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::HandleListStringTables),
		FParamSchemaBuilder()
			.OptionalAssetPathWithDefault(TEXT("path"), TEXT("Canonical mounted package root"), TEXT("/Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tables to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load only the returned page and include namespace and entry count"), TEXT("false"))
			.Build());

	Registry.RegisterAction(
		TEXT("localization"),
		TEXT("get_string_table"),
		TEXT("Read a stable bounded StringTable entry page with an exclusive key cursor"),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::HandleGetStringTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical StringTable package or object path"))
			.Optional(TEXT("after_key"), TEXT("string"), TEXT("Exclusive stable entry cursor"))
			.Optional(TEXT("entry_limit"), TEXT("integer"), TEXT("Maximum entries to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include metadata rows within a shared budget"), TEXT("false"))
			.Optional(TEXT("metadata_limit"), TEXT("integer"), TEXT("Aggregate metadata rows for this page (0-4096)"), TEXT("512"))
			.Optional(TEXT("text_limit"), TEXT("integer"), TEXT("Maximum source or metadata-value characters per field (1-65536)"), TEXT("4096"))
			.Build());

	Registry.RegisterAction(
		TEXT("localization"),
		TEXT("validate_string_table"),
		TEXT("Validate bounded StringTable keys and source strings with explicit completeness"),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::HandleValidateStringTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical StringTable package or object path"))
			.Optional(TEXT("scan_limit"), TEXT("integer"), TEXT("Maximum entries validated in deterministic key order (1-10000)"), TEXT("4096"))
			.Optional(TEXT("issue_offset"), TEXT("integer"), TEXT("Zero-based issue offset"), TEXT("0"))
			.Optional(TEXT("issue_limit"), TEXT("integer"), TEXT("Maximum issues to return (1-1000)"), TEXT("200"))
			.Build());
}

FMonolithActionResult FMonolithLocalizationActions::HandleListCultures(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RequestedNames;
	bool bNamesProvided = false;
	bool bIncludeDerived = true;
	int32 Offset = 0;
	int32 Limit = MonolithLocalizationActions::DefaultCulturePageLimit;
	FMonolithActionResult Error;
	if (!MonolithLocalizationActions::ParseStringArray(
			Params,
			TEXT("culture_names"),
			MonolithLocalizationActions::MaxExplicitCultureNames,
			RequestedNames,
			bNamesProvided,
			Error)
		|| !MonolithLocalizationActions::ParseOptionalBool(
			Params,
			TEXT("include_derived"),
			true,
			bIncludeDerived,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("offset"),
			0,
			0,
			MAX_int32,
			Offset,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("limit"),
			MonolithLocalizationActions::DefaultCulturePageLimit,
			1,
			MonolithLocalizationActions::MaxCulturePageLimit,
			Limit,
			Error))
	{
		return Error;
	}

	FInternationalization& Internationalization = FInternationalization::Get();
	TArray<FCultureRef> Cultures;
	if (bNamesProvided)
	{
		Cultures = Internationalization.GetAvailableCultures(RequestedNames, bIncludeDerived);
	}
	else
	{
		TArray<FString> CultureNames;
		Internationalization.GetCultureNames(CultureNames);
		for (const FString& CultureName : CultureNames)
		{
			const FCulturePtr Culture = Internationalization.GetCulture(CultureName);
			if (Culture.IsValid())
			{
				Cultures.Add(Culture.ToSharedRef());
			}
		}
	}

	Cultures.Sort([](const FCultureRef& A, const FCultureRef& B)
	{
		return A->GetName() < B->GetName();
	});
	TArray<FCultureRef> UniqueCultures;
	TSet<FString> SeenNames;
	for (const FCultureRef& Culture : Cultures)
	{
		if (!SeenNames.Contains(Culture->GetName()))
		{
			SeenNames.Add(Culture->GetName());
			UniqueCultures.Add(Culture);
		}
	}

	const int32 Start = FMath::Min(Offset, UniqueCultures.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		UniqueCultures.Num(),
		static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(End - Start);
	for (int32 Index = Start; Index < End; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(
			MonolithLocalizationActions::CultureToJson(UniqueCultures[Index])));
	}

	TArray<TSharedPtr<FJsonValue>> UnresolvedRows;
	if (bNamesProvided)
	{
		for (const FString& RequestedName : RequestedNames)
		{
			if (!Internationalization.GetCulture(RequestedName).IsValid())
			{
				UnresolvedRows.Add(MakeShared<FJsonValueString>(RequestedName));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("current_culture"), Internationalization.GetCurrentCulture()->GetName());
	Result->SetStringField(TEXT("current_language"), Internationalization.GetCurrentLanguage()->GetName());
	Result->SetStringField(TEXT("current_locale"), Internationalization.GetCurrentLocale()->GetName());
	Result->SetBoolField(TEXT("explicit_names"), bNamesProvided);
	Result->SetBoolField(TEXT("include_derived"), bIncludeDerived);
	Result->SetArrayField(TEXT("unresolved_names"), UnresolvedRows);
	Result->SetArrayField(TEXT("cultures"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	MonolithLocalizationActions::SetPageMetadata(
		Result,
		UniqueCultures.Num(),
		Offset,
		Limit,
		Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::HandleListStringTables(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RawPath = TEXT("/Game");
	bool bPathProvided = false;
	FString PackageFilter;
	int32 Offset = 0;
	int32 Limit = MonolithLocalizationActions::DefaultAssetPageLimit;
	bool bIncludeDetails = false;
	FMonolithActionResult Error;
	if (!MonolithLocalizationActions::ReadOptionalString(
			Params,
			TEXT("path"),
			TEXT("/Game"),
			RawPath,
			bPathProvided,
			Error)
		|| !MonolithLocalizationActions::ParsePackageFilter(RawPath, PackageFilter, Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("offset"),
			0,
			0,
			MAX_int32,
			Offset,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("limit"),
			MonolithLocalizationActions::DefaultAssetPageLimit,
			1,
			MonolithLocalizationActions::MaxAssetPageLimit,
			Limit,
			Error)
		|| !MonolithLocalizationActions::ParseOptionalBool(
			Params,
			TEXT("include_details"),
			false,
			bIncludeDetails,
			Error))
	{
		return Error;
	}

	TArray<FAssetData> Assets;
	MonolithLocalizationActions::QueryStringTables(PackageFilter, Assets);
	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		Assets.Num(),
		static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(End - Start);
	for (int32 Index = Start; Index < End; ++Index)
	{
		const FAssetData& AssetData = Assets[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UStringTable* Table = Cast<UStringTable>(AssetData.GetAsset()))
			{
				Row = MonolithLocalizationActions::TableIdentityToJson(Table, true);
			}
			else
			{
				Row->SetStringField(TEXT("load_error"), TEXT("Asset could not be loaded as UStringTable"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackageFilter);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetArrayField(TEXT("string_tables"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	MonolithLocalizationActions::SetPageMetadata(Result, Assets.Num(), Offset, Limit, Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::HandleGetStringTable(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ObjectPath;
	FString AfterKey;
	bool bAfterKeyProvided = false;
	int32 EntryLimit = MonolithLocalizationActions::DefaultEntryPageLimit;
	bool bIncludeMetadata = false;
	int32 MetadataLimit = MonolithLocalizationActions::DefaultMetadataBudget;
	int32 TextLimit = MonolithLocalizationActions::DefaultTextLimit;
	FMonolithActionResult Error;
	if (!MonolithLocalizationActions::ReadRequiredString(
			Params,
			TEXT("asset_path"),
			AssetPath,
			Error)
		|| !MonolithLocalizationActions::ReadOptionalString(
			Params,
			TEXT("after_key"),
			FString(),
			AfterKey,
			bAfterKeyProvided,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("entry_limit"),
			MonolithLocalizationActions::DefaultEntryPageLimit,
			1,
			MonolithLocalizationActions::MaxEntryPageLimit,
			EntryLimit,
			Error)
		|| !MonolithLocalizationActions::ParseOptionalBool(
			Params,
			TEXT("include_metadata"),
			false,
			bIncludeMetadata,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("metadata_limit"),
			MonolithLocalizationActions::DefaultMetadataBudget,
			0,
			MonolithLocalizationActions::MaxMetadataBudget,
			MetadataLimit,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("text_limit"),
			MonolithLocalizationActions::DefaultTextLimit,
			1,
			MonolithLocalizationActions::MaxTextLimit,
			TextLimit,
			Error))
	{
		return Error;
	}
	if (AfterKey.Len() > MonolithLocalizationActions::MaxCursorLength)
	{
		return MonolithLocalizationActions::InvalidParam(
			TEXT("after_key"),
			FString::Printf(
				TEXT("must not exceed %d characters"),
				MonolithLocalizationActions::MaxCursorLength));
	}

	UStringTable* Table = MonolithLocalizationActions::LoadStringTable(
		AssetPath,
		TEXT("asset_path"),
		ObjectPath,
		Error);
	if (!Table)
	{
		return Error;
	}

	TArray<FString> Keys;
	int32 EntryCount = 0;
	int32 EligibleCount = 0;
	const FString* Cursor = bAfterKeyProvided ? &AfterKey : nullptr;
	MonolithLocalizationActions::CollectSmallestKeys(
		Table,
		Cursor,
		EntryLimit + 1,
		Keys,
		EntryCount,
		EligibleCount);
	const bool bHasMoreEntries = Keys.Num() > EntryLimit;
	if (bHasMoreEntries)
	{
		Keys.SetNum(EntryLimit, EAllowShrinking::No);
	}

	const FStringTableConstRef StringTable = Table->GetStringTable();
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(Keys.Num());
	int32 RemainingMetadataBudget = bIncludeMetadata ? MetadataLimit : 0;
	int32 AvailableMetadataCount = 0;
	int32 ReturnedMetadataCount = 0;
	bool bMetadataComplete = true;
	for (const FString& Key : Keys)
	{
		FString SourceString;
		if (!StringTable->GetSourceString(FTextKey(Key), SourceString))
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("StringTable entry '%s' disappeared during readback"),
					*Key));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Key);
		Entry->SetNumberField(TEXT("key_length"), Key.Len());
		MonolithLocalizationActions::SetBoundedString(
			Entry,
			TEXT("source_string"),
			MonolithLocalizationActions::BoundValue(SourceString, TextLimit));

		if (bIncludeMetadata)
		{
			TArray<MonolithLocalizationActions::FMetadataSnapshot> MetadataRows;
			int32 MetadataCount = 0;
			MonolithLocalizationActions::CollectMetadata(
				StringTable,
				Key,
				RemainingMetadataBudget,
				TextLimit,
				MetadataRows,
				MetadataCount);
			TArray<TSharedPtr<FJsonValue>> MetadataValues;
			MetadataValues.Reserve(MetadataRows.Num());
			for (const MonolithLocalizationActions::FMetadataSnapshot& Metadata : MetadataRows)
			{
				MetadataValues.Add(MakeShared<FJsonValueObject>(
					MonolithLocalizationActions::MetadataToJson(Metadata)));
			}
			Entry->SetArrayField(TEXT("metadata"), MetadataValues);
			Entry->SetNumberField(TEXT("metadata_count"), MetadataCount);
			Entry->SetNumberField(TEXT("metadata_returned"), MetadataRows.Num());
			Entry->SetBoolField(TEXT("metadata_truncated"), MetadataRows.Num() < MetadataCount);
			AvailableMetadataCount += MetadataCount;
			ReturnedMetadataCount += MetadataRows.Num();
			RemainingMetadataBudget = FMath::Max(0, RemainingMetadataBudget - MetadataRows.Num());
			bMetadataComplete &= MetadataRows.Num() == MetadataCount;
		}
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const bool bAllEntriesCovered = !bAfterKeyProvided && !bHasMoreEntries;
	TSharedPtr<FJsonObject> Result = MonolithLocalizationActions::TableIdentityToJson(Table, false);
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetNumberField(TEXT("entries_after_cursor"), EligibleCount);
	Result->SetNumberField(TEXT("entry_limit"), EntryLimit);
	Result->SetNumberField(TEXT("entries_returned"), Entries.Num());
	Result->SetBoolField(TEXT("has_more_entries"), bHasMoreEntries);
	Result->SetBoolField(TEXT("all_entries_covered"), bAllEntriesCovered);
	Result->SetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	Result->SetNumberField(TEXT("metadata_limit"), MetadataLimit);
	Result->SetNumberField(TEXT("available_metadata_count"), AvailableMetadataCount);
	Result->SetNumberField(TEXT("returned_metadata_count"), ReturnedMetadataCount);
	Result->SetBoolField(TEXT("metadata_complete"), bMetadataComplete);
	Result->SetNumberField(TEXT("text_limit"), TextLimit);
	Result->SetArrayField(TEXT("entries"), Entries);
	Result->SetBoolField(TEXT("complete"), bAllEntriesCovered && bMetadataComplete);
	Result->SetBoolField(TEXT("read_only"), true);
	if (bAfterKeyProvided)
	{
		Result->SetStringField(TEXT("after_key"), AfterKey);
	}
	if (bHasMoreEntries && !Keys.IsEmpty())
	{
		Result->SetStringField(TEXT("next_after_key"), Keys.Last());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::HandleValidateStringTable(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ObjectPath;
	int32 ScanLimit = MonolithLocalizationActions::DefaultValidationScanLimit;
	int32 IssueOffset = 0;
	int32 IssueLimit = MonolithLocalizationActions::DefaultIssuePageLimit;
	FMonolithActionResult Error;
	if (!MonolithLocalizationActions::ReadRequiredString(
			Params,
			TEXT("asset_path"),
			AssetPath,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("scan_limit"),
			MonolithLocalizationActions::DefaultValidationScanLimit,
			1,
			MonolithLocalizationActions::MaxValidationScanLimit,
			ScanLimit,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("issue_offset"),
			0,
			0,
			MAX_int32,
			IssueOffset,
			Error)
		|| !MonolithLocalizationActions::ParseBoundedInteger(
			Params,
			TEXT("issue_limit"),
			MonolithLocalizationActions::DefaultIssuePageLimit,
			1,
			MonolithLocalizationActions::MaxIssuePageLimit,
			IssueLimit,
			Error))
	{
		return Error;
	}

	UStringTable* Table = MonolithLocalizationActions::LoadStringTable(
		AssetPath,
		TEXT("asset_path"),
		ObjectPath,
		Error);
	if (!Table)
	{
		return Error;
	}

	TArray<FString> Keys;
	int32 EntryCount = 0;
	int32 EligibleCount = 0;
	MonolithLocalizationActions::CollectSmallestKeys(
		Table,
		nullptr,
		ScanLimit,
		Keys,
		EntryCount,
		EligibleCount);
	const bool bComplete = EntryCount <= ScanLimit;
	const FStringTableConstRef StringTable = Table->GetStringTable();
	TArray<MonolithLocalizationActions::FValidationIssue> Issues;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const FString& Key : Keys)
	{
		FString SourceString;
		if (!StringTable->GetSourceString(FTextKey(Key), SourceString))
		{
			MonolithLocalizationActions::AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("source_lookup_failed"),
				TEXT("error"),
				TEXT("The entry disappeared while validation was reading it."),
				Key);
			continue;
		}

		if (Key.IsEmpty())
		{
			MonolithLocalizationActions::AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("empty_key"),
				TEXT("error"),
				TEXT("StringTable entry has an empty key."));
		}
		FString TrimmedKey = Key;
		TrimmedKey.TrimStartAndEndInline();
		if (TrimmedKey != Key)
		{
			MonolithLocalizationActions::AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("key_edge_whitespace"),
				TEXT("warning"),
				TEXT("StringTable key has leading or trailing whitespace."),
				Key);
		}
		if (SourceString.IsEmpty())
		{
			MonolithLocalizationActions::AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("empty_source_string"),
				TEXT("warning"),
				TEXT("StringTable entry has an empty source string."),
				Key);
		}
	}

	if (EntryCount == 0)
	{
		MonolithLocalizationActions::AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("empty_table"),
			TEXT("error"),
			TEXT("StringTable has no entries."));
	}
	if (!bComplete)
	{
		MonolithLocalizationActions::AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("scan_limit_exceeded"),
			TEXT("error"),
			FString::Printf(
				TEXT("Validation scanned %d of %d entries."),
				Keys.Num(),
				EntryCount));
	}

	Issues.Sort([](
		const MonolithLocalizationActions::FValidationIssue& A,
		const MonolithLocalizationActions::FValidationIssue& B)
	{
		if (A.Key != B.Key)
		{
			return A.Key < B.Key;
		}
		if (A.Code != B.Code)
		{
			return A.Code < B.Code;
		}
		return A.Message < B.Message;
	});

	const int32 IssueStart = FMath::Min(IssueOffset, Issues.Num());
	const int32 IssueEnd = static_cast<int32>(FMath::Min<int64>(
		Issues.Num(),
		static_cast<int64>(IssueOffset) + IssueLimit));
	TArray<TSharedPtr<FJsonValue>> IssueRows;
	IssueRows.Reserve(IssueEnd - IssueStart);
	for (int32 Index = IssueStart; Index < IssueEnd; ++Index)
	{
		IssueRows.Add(MakeShared<FJsonValueObject>(
			MonolithLocalizationActions::IssueToJson(Issues[Index])));
	}

	TSharedPtr<FJsonObject> Result = MonolithLocalizationActions::TableIdentityToJson(Table, false);
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetNumberField(TEXT("entries_scanned"), Keys.Num());
	Result->SetNumberField(TEXT("scan_limit"), ScanLimit);
	Result->SetBoolField(TEXT("complete"), bComplete);
	Result->SetBoolField(TEXT("valid"), bComplete && ErrorCount == 0);
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetNumberField(TEXT("warnings"), WarningCount);
	Result->SetNumberField(TEXT("issue_total"), Issues.Num());
	Result->SetNumberField(TEXT("issue_offset"), IssueOffset);
	Result->SetNumberField(TEXT("issue_limit"), IssueLimit);
	Result->SetNumberField(TEXT("issues_returned"), IssueRows.Num());
	Result->SetBoolField(
		TEXT("has_more_issues"),
		static_cast<int64>(IssueOffset) + IssueRows.Num() < Issues.Num());
	Result->SetArrayField(TEXT("issues"), IssueRows);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}
