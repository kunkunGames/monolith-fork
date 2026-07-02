#include "MonolithLocalizationActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/ObjectLibrary.h"
#include "Factories/StringTableFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Csv/CsvParser.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	struct FLocalizationMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = false;
	};

	struct FStringTableCsvRow
	{
		FString Key;
		FString SourceString;
		TMap<FString, FString> Metadata;
	};

	int32 GetClampedLimit(const TSharedPtr<FJsonObject>& Params, int32 DefaultValue)
	{
		double LimitNumber = static_cast<double>(DefaultValue);
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		}
		return FMath::Clamp(static_cast<int32>(LimitNumber), 1, 1000);
	}

	FString GetPathParam(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path = TEXT("/Game");
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("path"), Path);
		}
		if (Path.IsEmpty())
		{
			Path = TEXT("/Game");
		}
		return Path;
	}

	bool IsProjectContentPath(const FString& Path)
	{
		return Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"));
	}

	bool ResolveProjectContentPath(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutPath, FString& OutError)
	{
		FString Path = GetPathParam(Params);
		if (FieldName && Params.IsValid())
		{
			Params->TryGetStringField(FieldName, Path);
		}

		OutPath = FMonolithAssetUtils::ResolveAssetPath(Path);
		if (!IsProjectContentPath(OutPath))
		{
			OutError = FString::Printf(TEXT("Path '%s' must resolve under /Game"), *Path);
			return false;
		}

		return true;
	}

	bool ReadRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError, bool bAllowEmpty = false)
	{
		if (!Params.IsValid())
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->IsNull())
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}

		if (!Field->TryGetString(OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a string"), FieldName);
			return false;
		}

		if (!bAllowEmpty && OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& OutValue, FString& OutError)
	{
			if (Params.IsValid())
		{
				const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
				if (Field.IsValid() && !Field->IsNull() && !Field->TryGetBool(OutValue))
				{
					OutError = FString::Printf(TEXT("Malformed parameter: %s must be a boolean"), FieldName);
					return false;
				}
		}
		return true;
	}

	bool ReadMutationOptions(const TSharedPtr<FJsonObject>& Params, FLocalizationMutationOptions& OutOptions, FString& OutError)
	{
		if (!ReadOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError) ||
			!ReadOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError) ||
			!ReadOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError))
		{
			return false;
		}

		if (!OutOptions.bDryRun && !OutOptions.bConfirm)
		{
			OutError = TEXT("Mutating localization actions require dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

	bool SplitStringTableAssetPath(const FString& RawPath, FString& OutPackagePath, FString& OutAssetName, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(RawPath);
		if (!IsProjectContentPath(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("StringTable asset path '%s' must resolve under /Game"), *OutAssetPath);
			return false;
		}

		int32 DotIndex = INDEX_NONE;
		if (OutAssetPath.FindLastChar(TEXT('.'), DotIndex))
		{
			OutPackagePath = OutAssetPath.Left(DotIndex);
			OutAssetName = OutAssetPath.Mid(DotIndex + 1);
			OutAssetPath = OutPackagePath;
		}
		else
		{
			OutPackagePath = OutAssetPath;
			OutAssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		}

		if (OutAssetName.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot derive StringTable asset name from '%s'"), *RawPath);
			return false;
		}

		FText Reason;
		if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &Reason))
		{
			OutError = FString::Printf(TEXT("Invalid StringTable package path '%s': %s"), *OutPackagePath, *Reason.ToString());
			return false;
		}
		return true;
	}

	bool DoesStringTableAssetExist(const FString& PackagePath, const FString& AssetName)
	{
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			return true;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath))
		{
			return FindObject<UObject>(ExistingPackage, *AssetName) != nullptr;
		}
		return false;
	}

	FName MakeStringTableAssetId(const FString& PackagePath, const FString& AssetName)
	{
		return FName(*FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));
	}

	bool ResolveProjectFilePath(const FString& RawPath, FString& OutFilePath, FString& OutError)
	{
		if (RawPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Missing required param 'file_path'");
			return false;
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDir);
		FString ProjectPrefix = ProjectDir;
		if (!ProjectPrefix.EndsWith(TEXT("/")))
		{
			ProjectPrefix += TEXT("/");
		}

		OutFilePath = FPaths::IsRelative(RawPath)
			? FPaths::ConvertRelativePathToFull(ProjectDir, RawPath)
			: FPaths::ConvertRelativePathToFull(RawPath);
		FPaths::NormalizeFilename(OutFilePath);

		if (!(OutFilePath.Equals(ProjectDir, ESearchCase::IgnoreCase) || OutFilePath.StartsWith(ProjectPrefix, ESearchCase::IgnoreCase)))
		{
			OutError = FString::Printf(TEXT("CSV file path '%s' must stay under project directory '%s'"), *OutFilePath, *ProjectDir);
			return false;
		}
		return true;
	}

	bool SaveAssetIfRequested(UObject* Asset, bool bSave, bool& bOutSaved, FString& OutSavedPath, FString& OutError)
	{
		bOutSaved = false;
		OutSavedPath.Reset();
		if (!bSave)
		{
			return true;
		}

		if (!Asset || !Asset->GetPackage())
		{
			OutError = TEXT("Cannot save null asset or asset package");
			return false;
		}

		if (!FPackageName::TryConvertLongPackageNameToFilename(Asset->GetPackage()->GetName(), OutSavedPath, FPackageName::GetAssetPackageExtension()))
		{
			OutError = FString::Printf(TEXT("Could not convert package '%s' to a package filename"), *Asset->GetPackage()->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		bOutSaved = UPackage::SavePackage(Asset->GetPackage(), Asset, *OutSavedPath, SaveArgs);
		if (!bOutSaved)
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *OutSavedPath);
			return false;
		}
		return true;
	}

	void AddMutationBaseFields(TSharedPtr<FJsonObject>& Result, const FString& AssetPath, const FLocalizationMutationOptions& Options, bool bChanged, bool bSaved)
	{
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
		Result->SetBoolField(TEXT("confirm_received"), Options.bConfirm);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetBoolField(TEXT("saved"), bSaved);
	}

	FString EscapeCsvCell(const FString& Cell)
	{
		FString Escaped = Cell;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""));
		if (Escaped.Contains(TEXT(",")) || Escaped.Contains(TEXT("\"")) || Escaped.Contains(TEXT("\r")) || Escaped.Contains(TEXT("\n")))
		{
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}
		return Escaped;
	}

	FString CsvCellAt(const FCsvParser::FRows::ElementType& Row, int32 Index)
	{
		if (!Row.IsValidIndex(Index) || Row[Index] == nullptr)
		{
			return FString();
		}
		return FString(Row[Index]);
	}

	TArray<FStringTableCsvRow> ParseStringTableCsv(const FString& CsvText, TArray<TSharedPtr<FJsonValue>>& OutRowResults, int32& OutSkippedCount, FString& OutError)
	{
		TArray<FStringTableCsvRow> ParsedRows;
		OutSkippedCount = 0;

		FCsvParser Parser(CsvText);
		const FCsvParser::FRows& Rows = Parser.GetRows();
		if (Rows.Num() == 0)
		{
			OutError = TEXT("CSV file is empty");
			return ParsedRows;
		}

		const TArray<const TCHAR*>& Header = Rows[0];
		int32 KeyIndex = INDEX_NONE;
		int32 SourceIndex = INDEX_NONE;
		TArray<TPair<FString, int32>> MetadataColumns;
		for (int32 ColumnIndex = 0; ColumnIndex < Header.Num(); ++ColumnIndex)
		{
			const FString HeaderName = FString(Header[ColumnIndex]).TrimStartAndEnd();
			if (HeaderName.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				KeyIndex = ColumnIndex;
			}
			else if (HeaderName.Equals(TEXT("source_string"), ESearchCase::IgnoreCase))
			{
				SourceIndex = ColumnIndex;
			}
			else if (!HeaderName.IsEmpty())
			{
				MetadataColumns.Add(TPair<FString, int32>(HeaderName, ColumnIndex));
			}
		}

		if (KeyIndex == INDEX_NONE || SourceIndex == INDEX_NONE)
		{
			OutError = TEXT("CSV header must include 'key' and 'source_string'");
			return ParsedRows;
		}

		for (int32 RowIndex = 1; RowIndex < Rows.Num(); ++RowIndex)
		{
			const TArray<const TCHAR*>& Row = Rows[RowIndex];
			FStringTableCsvRow ParsedRow;
			ParsedRow.Key = CsvCellAt(Row, KeyIndex);
			ParsedRow.SourceString = CsvCellAt(Row, SourceIndex);

			TSharedPtr<FJsonObject> RowResult = MakeShared<FJsonObject>();
			RowResult->SetNumberField(TEXT("row"), RowIndex + 1);
			RowResult->SetStringField(TEXT("key"), ParsedRow.Key);

			if (ParsedRow.Key.TrimStartAndEnd().IsEmpty())
			{
				++OutSkippedCount;
				RowResult->SetStringField(TEXT("status"), TEXT("skipped"));
				RowResult->SetStringField(TEXT("reason"), TEXT("empty_key"));
				if (OutRowResults.Num() < 200)
				{
					OutRowResults.Add(MakeShared<FJsonValueObject>(RowResult));
				}
				continue;
			}

			for (const TPair<FString, int32>& MetadataColumn : MetadataColumns)
			{
				const FString Value = CsvCellAt(Row, MetadataColumn.Value);
				if (!Value.IsEmpty())
				{
					ParsedRow.Metadata.Add(MetadataColumn.Key, Value);
				}
			}

			ParsedRows.Add(MoveTemp(ParsedRow));
			RowResult->SetStringField(TEXT("status"), TEXT("accepted"));
			if (OutRowResults.Num() < 200)
			{
				OutRowResults.Add(MakeShared<FJsonValueObject>(RowResult));
			}
		}

		return ParsedRows;
	}

	void CollectStringTableRows(UStringTable* Table, bool bIncludeMetadata, TArray<FStringTableCsvRow>& OutRows, TArray<FString>& OutMetadataKeys)
	{
		if (!Table)
		{
			return;
		}

		TSet<FString> MetadataKeySet;
		FStringTableConstRef TableRef = Table->GetStringTable();
		TableRef->EnumerateKeysAndSourceStrings(
			[TableRef, bIncludeMetadata, &OutRows, &MetadataKeySet](const FTextKey& Key, const FString& SourceString)
			{
				FStringTableCsvRow Row;
				Row.Key = Key.ToString();
				Row.SourceString = SourceString;
				if (bIncludeMetadata)
				{
					TableRef->EnumerateMetaData(Key,
						[&Row, &MetadataKeySet](FName MetadataId, const FString& MetadataValue)
						{
							const FString MetadataKey = MetadataId.ToString();
							Row.Metadata.Add(MetadataKey, MetadataValue);
							MetadataKeySet.Add(MetadataKey);
							return true;
						});
				}
				OutRows.Add(MoveTemp(Row));
				return true;
			});

		OutRows.Sort([](const FStringTableCsvRow& A, const FStringTableCsvRow& B)
		{
			return A.Key < B.Key;
		});

		for (const FString& MetadataKey : MetadataKeySet)
		{
			OutMetadataKeys.Add(MetadataKey);
		}
		OutMetadataKeys.Sort();
	}

	bool TryGetStringTableMetaData(FStringTableConstRef TableRef, const FTextKey& Key, const FString& MetadataKey, FString& OutValue)
	{
		bool bFound = false;
		const FName TargetKey(*MetadataKey);
		TableRef->EnumerateMetaData(Key,
			[&OutValue, &bFound, TargetKey](FName MetadataId, const FString& MetadataValue)
			{
				if (MetadataId == TargetKey)
				{
					OutValue = MetadataValue;
					bFound = true;
					return false;
				}
				return true;
			});
		return bFound;
	}

	FString BuildStringTableCsv(UStringTable* Table, bool bIncludeMetadata, int32& OutRowCount)
	{
		TArray<FStringTableCsvRow> Rows;
		TArray<FString> MetadataKeys;
		CollectStringTableRows(Table, bIncludeMetadata, Rows, MetadataKeys);
		OutRowCount = Rows.Num();

		TArray<FString> Lines;
		TArray<FString> Header;
		Header.Add(TEXT("key"));
		Header.Add(TEXT("source_string"));
		for (const FString& MetadataKey : MetadataKeys)
		{
			Header.Add(MetadataKey);
		}
		Lines.Add(FString::JoinBy(Header, TEXT(","), [](const FString& Cell) { return EscapeCsvCell(Cell); }));

		for (const FStringTableCsvRow& Row : Rows)
		{
			TArray<FString> Cells;
			Cells.Add(Row.Key);
			Cells.Add(Row.SourceString);
			for (const FString& MetadataKey : MetadataKeys)
			{
				if (const FString* MetadataValue = Row.Metadata.Find(MetadataKey))
				{
					Cells.Add(*MetadataValue);
				}
				else
				{
					Cells.Add(FString());
				}
			}
			Lines.Add(FString::JoinBy(Cells, TEXT(","), [](const FString& Cell) { return EscapeCsvCell(Cell); }));
		}

		return FString::Join(Lines, TEXT("\n"));
	}

	TSharedPtr<FJsonObject> CultureToJson(const FCultureRef& Culture)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("name"), Culture->GetName());
		Obj->SetStringField(TEXT("native_name"), Culture->GetNativeName());
		Obj->SetStringField(TEXT("english_name"), Culture->GetEnglishName());
		Obj->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
		Obj->SetStringField(TEXT("two_letter_iso"), Culture->GetTwoLetterISOLanguageName());
		Obj->SetStringField(TEXT("three_letter_iso"), Culture->GetThreeLetterISOLanguageName());
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> StringTableEntriesToJson(const UStringTable* Table, int32 Limit, bool bIncludeMetadata, int32& OutTotalCount)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		OutTotalCount = 0;
		if (!Table)
		{
			return Rows;
		}

		const FStringTableConstRef StringTable = Table->GetStringTable();
		StringTable->EnumerateKeysAndSourceStrings(
			[&Rows, &OutTotalCount, Limit, bIncludeMetadata, StringTable](const FTextKey& Key, const FString& SourceString)
			{
				++OutTotalCount;
				if (Rows.Num() >= Limit)
				{
					return true;
				}

				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Key.ToString());
				Row->SetStringField(TEXT("source_string"), SourceString);
				Row->SetNumberField(TEXT("source_length"), SourceString.Len());

				if (bIncludeMetadata)
				{
					TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
					StringTable->EnumerateMetaData(Key,
						[&Metadata](FName MetadataId, const FString& MetadataValue)
						{
							Metadata->SetStringField(MetadataId.ToString(), MetadataValue);
							return true;
						});
					Row->SetObjectField(TEXT("metadata"), Metadata);
				}

				Rows.Add(MakeShared<FJsonValueObject>(Row));
				return true;
			});

		return Rows;
	}

	TSharedPtr<FJsonObject> StringTableSummaryToJson(UStringTable* Table, bool bIncludeEntries, int32 Limit, bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Table)
		{
			Obj->SetBoolField(TEXT("loaded"), false);
			return Obj;
		}

		Obj->SetBoolField(TEXT("loaded"), true);
		Obj->SetStringField(TEXT("asset_path"), Table->GetPathName());
		Obj->SetStringField(TEXT("package_path"), Table->GetOutermost() ? Table->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("name"), Table->GetName());
		Obj->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
		Obj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());

		int32 EntryCount = 0;
		TArray<TSharedPtr<FJsonValue>> Entries = StringTableEntriesToJson(Table, bIncludeEntries ? Limit : 1, bIncludeMetadata, EntryCount);
		Obj->SetNumberField(TEXT("entry_count"), EntryCount);
		if (bIncludeEntries)
		{
			Obj->SetArrayField(TEXT("entries"), Entries);
			Obj->SetNumberField(TEXT("returned_count"), Entries.Num());
			if (EntryCount > Entries.Num())
			{
				Obj->SetNumberField(TEXT("truncated_remaining"), EntryCount - Entries.Num());
			}
		}
		return Obj;
	}

	TArray<UStringTable*> LoadStringTablesUnderPath(const FString& Path)
	{
		TArray<UStringTable*> Tables;
		UObjectLibrary* Library = UObjectLibrary::CreateLibrary(UStringTable::StaticClass(), false, true);
		if (!Library)
		{
			return Tables;
		}

		Library->LoadAssetsFromPath(Path);
		TArray<UObject*> Objects;
		Library->GetObjects(Objects);
		for (UObject* Object : Objects)
		{
			if (UStringTable* Table = Cast<UStringTable>(Object))
			{
				Tables.Add(Table);
			}
		}

		Tables.Sort([](const UStringTable& A, const UStringTable& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		return Tables;
	}

	UStringTable* LoadStringTableFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), OutAssetPath) || OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}

		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
		if (!IsProjectContentPath(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("StringTable asset path '%s' must resolve under /Game"), *OutAssetPath);
			return nullptr;
		}

		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(OutAssetPath);
		UStringTable* Table = Cast<UStringTable>(Asset);
		if (!Table)
		{
			OutError = FString::Printf(TEXT("StringTable asset not found at '%s'"), *OutAssetPath);
		}
		return Table;
	}
}

void FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("localization"), TEXT("list_cultures"),
		TEXT("List available cultures known to Unreal internationalization."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListCultures),
		FParamSchemaBuilder()
			.Optional(TEXT("culture_names"), TEXT("array"), TEXT("Optional culture names to resolve; omitted returns configured/default culture context"))
			.Optional(TEXT("include_derived"), TEXT("boolean"), TEXT("Include derived cultures when resolving culture_names"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("list_string_tables"),
		TEXT("List StringTable assets under a project content path."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListStringTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to scan"), TEXT("/Game"))
			.Optional(TEXT("include_entries"), TEXT("boolean"), TEXT("Include capped entry rows"), TEXT("false"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata when entries are included"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tables or entries to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("get_string_table"),
		TEXT("Inspect a StringTable asset and return capped entries."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::GetStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata"), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum entries to return"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("validate_string_table"),
		TEXT("Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ValidateStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("create_string_table"),
		TEXT("Create a StringTable asset under /Game. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::CreateStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("New StringTable asset path"))
			.Optional(TEXT("namespace"), TEXT("string"), TEXT("StringTable namespace; defaults to asset name"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after creation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("set_string_entry"),
		TEXT("Add or replace one StringTable entry and optional metadata. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::SetStringEntry),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Entry key"))
			.Required(TEXT("source_string"), TEXT("string"), TEXT("Source string"))
			.Optional(TEXT("metadata"), TEXT("object"), TEXT("String metadata fields"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("remove_string_entry"),
		TEXT("Remove one StringTable entry by key. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::RemoveStringEntry),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Entry key"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("set_string_metadata"),
		TEXT("Add, replace, or remove metadata on one StringTable entry. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::SetStringMetadata),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Entry key"))
			.Required(TEXT("metadata_key"), TEXT("string"), TEXT("Metadata key"))
			.Optional(TEXT("metadata_value"), TEXT("string"), TEXT("Metadata value to set"))
			.Optional(TEXT("remove"), TEXT("boolean"), TEXT("Remove metadata_key instead of setting metadata_value"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("import_string_table_csv"),
		TEXT("Import key,source_string,metadata CSV rows into a StringTable. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ImportStringTableCsv),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Required(TEXT("file_path"), TEXT("string"), TEXT("CSV path under the project directory"))
			.Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("Clear existing entries before import"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("export_string_table_csv"),
		TEXT("Export a StringTable to CSV under the project directory. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ExportStringTableCsv),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Required(TEXT("file_path"), TEXT("string"), TEXT("Destination CSV path under the project directory"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include metadata columns"), TEXT("true"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithLocalizationActions::ListCultures(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> CultureNames;
	bool bIncludeDerived = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_derived"), bIncludeDerived);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params->TryGetArrayField(TEXT("culture_names"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
				{
					CultureNames.Add(Name);
				}
			}
		}
	}

	TArray<FCultureRef> Cultures;
	if (CultureNames.Num() > 0)
	{
		Cultures = FInternationalization::Get().GetAvailableCultures(CultureNames, bIncludeDerived);
	}
	else
	{
		const FCultureRef Current = FInternationalization::Get().GetCurrentCulture();
		Cultures.Add(Current);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FCultureRef& Culture : Cultures)
	{
		Rows.Add(MakeShared<FJsonValueObject>(CultureToJson(Culture)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("current_culture"), FInternationalization::Get().GetCurrentCulture()->GetName());
	Result->SetStringField(TEXT("current_language"), FInternationalization::Get().GetCurrentLanguage()->GetName());
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("cultures"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ListStringTables(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	if (!ResolveProjectContentPath(Params, TEXT("path"), Path, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const int32 Limit = GetClampedLimit(Params, 100);
	bool bIncludeEntries = false;
	bool bIncludeMetadata = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_entries"), bIncludeEntries);
		Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	}

	TArray<UStringTable*> Tables = LoadStringTablesUnderPath(Path);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (UStringTable* Table : Tables)
	{
		if (Rows.Num() >= Limit)
		{
			break;
		}
		Rows.Add(MakeShared<FJsonValueObject>(StringTableSummaryToJson(Table, bIncludeEntries, Limit, bIncludeMetadata)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("matched_count"), Tables.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetArrayField(TEXT("string_tables"), Rows);
	if (Tables.Num() > Rows.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), Tables.Num() - Rows.Num());
	}
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::GetStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeMetadata = true;
	Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	const int32 Limit = GetClampedLimit(Params, 200);

	TSharedPtr<FJsonObject> Result = StringTableSummaryToJson(Table, true, Limit, bIncludeMetadata);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ValidateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	auto AddIssue = [&Issues](const FString& Code, const FString& Message, const FString& Key = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Key.IsEmpty())
		{
			Issue->SetStringField(TEXT("key"), Key);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	TSet<FString> LowerKeys;
	int32 EntryCount = 0;
	Table->GetStringTable()->EnumerateKeysAndSourceStrings(
		[&EntryCount, &LowerKeys, &AddIssue](const FTextKey& Key, const FString& SourceString)
		{
			++EntryCount;
			const FString KeyString = Key.ToString();
			if (KeyString.TrimStartAndEnd().IsEmpty())
			{
				AddIssue(TEXT("empty_key"), TEXT("StringTable entry has an empty key."));
			}
			if (SourceString.IsEmpty())
			{
				AddIssue(TEXT("empty_source_string"), TEXT("StringTable entry has an empty source string."), KeyString);
			}

			const FString Lower = KeyString.ToLower();
			if (LowerKeys.Contains(Lower))
			{
				AddIssue(TEXT("case_insensitive_duplicate_key"), TEXT("Another key differs only by case."), KeyString);
			}
			LowerKeys.Add(Lower);
			return true;
		});

	if (EntryCount == 0)
	{
		AddIssue(TEXT("empty_table"), TEXT("StringTable has no entries."));
	}
	if (EntryCount > 1000)
	{
		AddIssue(TEXT("large_table"), TEXT("StringTable has more than 1000 entries; use capped reads for agent workflows."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
	Result->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::CreateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RawAssetPath;
	if (!ReadRequiredStringParam(Params, TEXT("asset_path"), RawAssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString PackagePath, AssetName, AssetPath;
	if (!SplitStringTableAssetPath(RawAssetPath, PackagePath, AssetName, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Namespace = AssetName;
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonValue> NamespaceField = Params->TryGetField(TEXT("namespace"));
		if (NamespaceField.IsValid() && !NamespaceField->IsNull() && !NamespaceField->TryGetString(Namespace))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: namespace must be a string"));
		}
	}
	if (Namespace.TrimStartAndEnd().IsEmpty())
	{
		Namespace = AssetName;
	}

	if (DoesStringTableAssetExist(PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	const FName TableId = MakeStringTableAssetId(PackagePath, AssetName);
	bool bUnregisteredStaleTable = false;
	if (!Options.bDryRun && FStringTableRegistry::Get().FindStringTable(TableId).IsValid())
	{
		bUnregisteredStaleTable = FStringTableRegistry::Get().UnregisterStringTable(TableId);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("unregistered_stale_string_table"), bUnregisteredStaleTable);
	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_create"), true);
		return FMonolithActionResult::Success(Result);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UStringTableFactory* Factory = NewObject<UStringTableFactory>();
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct UStringTableFactory"));
	}

	const FString ParentPath = FPackageName::GetLongPackagePath(PackagePath);
	UObject* Created = AssetTools.CreateAsset(AssetName, ParentPath, UStringTable::StaticClass(), Factory);
	UStringTable* Table = Cast<UStringTable>(Created);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AssetTools.CreateAsset returned no UStringTable for '%s'"), *AssetPath));
	}

	Table->Modify();
	Table->GetMutableStringTable()->SetNamespace(FTextKey(Namespace));
	Table->MarkPackageDirty();

	bool bSaved = false;
	FString SavedPath;
	if (!SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	AddMutationBaseFields(Result, AssetPath, Options, true, bSaved);
	Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
	if (!SavedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::SetStringEntry(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString SourceString;
	if (!ReadRequiredStringParam(Params, TEXT("source_string"), SourceString, Error, true))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	const TSharedPtr<FJsonObject>* MetadataObject = nullptr;
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonValue> MetadataField = Params->TryGetField(TEXT("metadata"));
		if (MetadataField.IsValid() && !MetadataField->IsNull() && !Params->TryGetObjectField(TEXT("metadata"), MetadataObject))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: metadata must be an object"));
		}
	}

	FString ExistingSource;
	const FTextKey TextKey(Key);
	const bool bHadEntry = Table->GetStringTable()->GetSourceString(TextKey, ExistingSource);

	TMap<FString, FString> MetadataToSet;
	if (MetadataObject && MetadataObject->IsValid())
	{
		for (const auto& Pair : FMonolithJsonUtils::GetFields(*MetadataObject))
		{
			FString MetadataValue;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(MetadataValue))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Malformed metadata value for '%s': expected string"), *Pair.Key));
			}
			MetadataToSet.Add(Pair.Key, MetadataValue);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetBoolField(TEXT("entry_existed"), bHadEntry);
	Result->SetStringField(TEXT("previous_source_string"), ExistingSource);
	Result->SetStringField(TEXT("source_string"), SourceString);

	bool bChanged = !bHadEntry || ExistingSource != SourceString;
	for (const TPair<FString, FString>& Pair : MetadataToSet)
	{
		FString PreviousMetadataValue;
		const bool bHadMetadata = TryGetStringTableMetaData(Table->GetStringTable(), TextKey, Pair.Key, PreviousMetadataValue);
		if (!bHadMetadata || PreviousMetadataValue != Pair.Value)
		{
			bChanged = true;
			break;
		}
	}

	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_set"), bChanged);
		Result->SetBoolField(TEXT("would_change"), bChanged);
		return FMonolithActionResult::Success(Result);
	}

	if (!bChanged)
	{
		Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
		return FMonolithActionResult::Success(Result);
	}

	Table->Modify();
	FStringTableRef MutableTable = Table->GetMutableStringTable();
	MutableTable->SetSourceString(TextKey, SourceString, FString());
	for (const TPair<FString, FString>& Pair : MetadataToSet)
	{
		MutableTable->SetMetaData(TextKey, FName(*Pair.Key), Pair.Value);
	}
	Table->MarkPackageDirty();

	bool bSaved = false;
	FString SavedPath;
	if (!SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	AddMutationBaseFields(Result, AssetPath, Options, true, bSaved);
	Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
	if (!SavedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::RemoveStringEntry(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FTextKey TextKey(Key);
	FString ExistingSource;
	const bool bHadEntry = Table->GetStringTable()->GetSourceString(TextKey, ExistingSource);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetBoolField(TEXT("entry_existed"), bHadEntry);
	Result->SetStringField(TEXT("previous_source_string"), ExistingSource);
	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_remove"), bHadEntry);
		return FMonolithActionResult::Success(Result);
	}

	if (bHadEntry)
	{
		Table->Modify();
		Table->GetMutableStringTable()->RemoveSourceString(TextKey);
		Table->MarkPackageDirty();
	}

	bool bSaved = false;
	FString SavedPath;
	if (bHadEntry && !SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	AddMutationBaseFields(Result, AssetPath, Options, bHadEntry, bSaved);
	Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
	if (!SavedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::SetStringMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString MetadataKey;
	if (!ReadRequiredStringParam(Params, TEXT("metadata_key"), MetadataKey, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bRemove = false;
	if (!ReadOptionalBoolParam(Params, TEXT("remove"), bRemove, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString MetadataValue;
	if (!bRemove && !ReadRequiredStringParam(Params, TEXT("metadata_value"), MetadataValue, Error, true))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (bRemove && Params.IsValid())
	{
		const TSharedPtr<FJsonValue> MetadataValueField = Params->TryGetField(TEXT("metadata_value"));
		if (MetadataValueField.IsValid() && !MetadataValueField->IsNull() && !MetadataValueField->TryGetString(MetadataValue))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: metadata_value must be a string"));
		}
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FTextKey TextKey(Key);
	FString SourceString;
	if (!Table->GetStringTable()->GetSourceString(TextKey, SourceString))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("StringTable entry '%s' does not exist"), *Key));
	}

	FString PreviousValue;
	const bool bHadMetadata = TryGetStringTableMetaData(Table->GetStringTable(), TextKey, MetadataKey, PreviousValue);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("metadata_key"), MetadataKey);
	Result->SetStringField(TEXT("previous_metadata_value"), PreviousValue);
	Result->SetBoolField(TEXT("remove"), bRemove);
	if (!bRemove)
	{
		Result->SetStringField(TEXT("metadata_value"), MetadataValue);
	}
	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_change"), bRemove ? bHadMetadata : PreviousValue != MetadataValue);
		return FMonolithActionResult::Success(Result);
	}

	const bool bChanged = bRemove ? bHadMetadata : PreviousValue != MetadataValue;
	bool bSaved = false;
	FString SavedPath;
	if (bChanged)
	{
		Table->Modify();
		if (bRemove)
		{
			Table->GetMutableStringTable()->RemoveMetaData(TextKey, FName(*MetadataKey));
		}
		else
		{
			Table->GetMutableStringTable()->SetMetaData(TextKey, FName(*MetadataKey), MetadataValue);
		}
		Table->MarkPackageDirty();

		if (!SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	AddMutationBaseFields(Result, AssetPath, Options, bChanged, bSaved);
	Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
	if (!SavedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ImportStringTableCsv(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RawFilePath;
	if (!ReadRequiredStringParam(Params, TEXT("file_path"), RawFilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString FilePath;
	if (!ResolveProjectFilePath(RawFilePath, FilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bReplaceExisting = false;
	if (!ReadOptionalBoolParam(Params, TEXT("replace_existing"), bReplaceExisting, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString CsvText;
	if (!FFileHelper::LoadFileToString(CsvText, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read CSV file '%s'"), *FilePath));
	}

	TArray<TSharedPtr<FJsonValue>> RowResults;
	int32 SkippedCount = 0;
	TArray<FStringTableCsvRow> Rows = ParseStringTableCsv(CsvText, RowResults, SkippedCount, Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	if (bReplaceExisting && Rows.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("replace_existing=true requires at least one accepted CSV row; refusing to clear the StringTable from an empty or fully skipped import"));
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("file_path"), FilePath);
	Result->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	Result->SetNumberField(TEXT("accepted_count"), Rows.Num());
	Result->SetNumberField(TEXT("skipped_count"), SkippedCount);
	Result->SetArrayField(TEXT("row_results"), RowResults);
	if (Rows.Num() + SkippedCount > RowResults.Num())
	{
		Result->SetNumberField(TEXT("truncated_row_results"), Rows.Num() + SkippedCount - RowResults.Num());
	}
	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_import"), Rows.Num() > 0 || bReplaceExisting);
		return FMonolithActionResult::Success(Result);
	}

	const bool bChanged = Rows.Num() > 0 || bReplaceExisting;
	bool bSaved = false;
	FString SavedPath;
	if (bChanged)
	{
		Table->Modify();
		FStringTableRef MutableTable = Table->GetMutableStringTable();
		if (bReplaceExisting)
		{
			MutableTable->ClearSourceStrings();
		}
		for (const FStringTableCsvRow& Row : Rows)
		{
			const FTextKey TextKey(Row.Key);
			MutableTable->SetSourceString(TextKey, Row.SourceString, FString());
			for (const TPair<FString, FString>& MetadataPair : Row.Metadata)
			{
				MutableTable->SetMetaData(TextKey, FName(*MetadataPair.Key), MetadataPair.Value);
			}
		}
		Table->MarkPackageDirty();

		if (!SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	AddMutationBaseFields(Result, AssetPath, Options, bChanged, bSaved);
	Result->SetObjectField(TEXT("string_table"), StringTableSummaryToJson(Table, true, 200, true));
	if (!SavedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ExportStringTableCsv(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RawFilePath;
	if (!ReadRequiredStringParam(Params, TEXT("file_path"), RawFilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString FilePath;
	if (!ResolveProjectFilePath(RawFilePath, FilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeMetadata = true;
	if (!ReadOptionalBoolParam(Params, TEXT("include_metadata"), bIncludeMetadata, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 RowCount = 0;
	const FString CsvText = BuildStringTableCsv(Table, bIncludeMetadata, RowCount);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("file_path"), FilePath);
	Result->SetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	const FTCHARToUTF8 CsvUtf8(*CsvText);
	Result->SetNumberField(TEXT("byte_count"), CsvUtf8.Length());
	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("would_export"), true);
		return FMonolithActionResult::Success(Result);
	}

	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}
	if (!FFileHelper::SaveStringToFile(CsvText, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write CSV file '%s'"), *FilePath));
	}
	const int64 ActualByteCount = IFileManager::Get().FileSize(*FilePath);
	if (ActualByteCount >= 0)
	{
		Result->SetNumberField(TEXT("byte_count"), static_cast<double>(ActualByteCount));
	}

	AddMutationBaseFields(Result, AssetPath, Options, true, true);
	return FMonolithActionResult::Success(Result);
}
