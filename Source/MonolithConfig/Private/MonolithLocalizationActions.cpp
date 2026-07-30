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
#include "HAL/PlatformFileManager.h"
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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	constexpr int32 MaxValidationIssueRows = 200;
	const TCHAR* MetadataPresenceCsvHeader = TEXT("__monolith_metadata_presence_v1");
	const TCHAR* SpreadsheetFormulaGuard = TEXT("'__monolith_formula_guard_v1__:");

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

	void SetSourceStringCompat(
		const FStringTableRef& Table,
		const FTextKey& Key,
		const FString& SourceString,
		const FString* PreservedDevNotes = nullptr)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		FString DevNotes = PreservedDevNotes ? *PreservedDevNotes : FString();
		if (!PreservedDevNotes)
		{
			if (const FStringTableEntryConstPtr ExistingEntry = Table->FindEntry(Key))
			{
				DevNotes = ExistingEntry->GetDevNotes();
			}
		}
		Table->SetSourceString(Key, SourceString, DevNotes);
#else
		(void)PreservedDevNotes;
		Table->SetSourceString(Key, SourceString);
#endif
	}

	FMonolithActionResult InvalidParams(const FString& Message)
	{
		return FMonolithActionResult::Error(Message, -32602);
	}

	bool GetClampedLimit(const TSharedPtr<FJsonObject>& Params, int32 DefaultValue, int32& OutLimit, FString& OutError)
	{
		double LimitNumber = static_cast<double>(DefaultValue);
		if (Params.IsValid())
		{
			const TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
			if (LimitField.IsValid() &&
				(LimitField->Type != EJson::Number ||
				 !LimitField->TryGetNumber(LimitNumber) ||
				 !FMath::IsFinite(LimitNumber) ||
				 FMath::TruncToDouble(LimitNumber) != LimitNumber))
			{
				OutError = TEXT("Malformed parameter: limit must be an integer");
				return false;
			}
		}
		const double ClampedLimit = FMath::Clamp(LimitNumber, 1.0, 1000.0);
		OutLimit = static_cast<int32>(ClampedLimit);
		return true;
	}

	bool GetPathParam(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutError)
	{
		OutPath = TEXT("/Game");
		if (Params.IsValid())
		{
			const TSharedPtr<FJsonValue> PathField = Params->TryGetField(TEXT("path"));
			if (PathField.IsValid() && (PathField->Type != EJson::String || !PathField->TryGetString(OutPath)))
			{
				OutError = TEXT("Malformed parameter: path must be a string");
				return false;
			}
		}
		if (OutPath.IsEmpty())
		{
			OutPath = TEXT("/Game");
		}
		return true;
	}

	bool IsProjectContentPath(const FString& Path)
	{
		return Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"));
	}

	bool ResolveProjectContentPath(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutPath, FString& OutError)
	{
		FString Path;
		if (!GetPathParam(Params, Path, OutError))
		{
			return false;
		}
		if (FieldName && Params.IsValid())
		{
			const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
			if (Field.IsValid() && (Field->Type != EJson::String || !Field->TryGetString(Path)))
			{
				OutError = FString::Printf(TEXT("Malformed parameter: %s must be a string"), FieldName);
				return false;
			}
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

		if (Field->Type != EJson::String || !Field->TryGetString(OutValue))
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
			if (Field.IsValid() && (Field->Type != EJson::Boolean || !Field->TryGetBool(OutValue)))
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

		const FString PackageLeafName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (!OutAssetName.Equals(PackageLeafName, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("StringTable object name '%s' must match package leaf '%s' in asset path '%s'"),
				*OutAssetName,
				*PackageLeafName,
				*RawPath);
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

	bool IsLexicallyUnderRoot(FString Path, FString Root)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Path);
		FPaths::NormalizeDirectoryName(Root);
#if PLATFORM_WINDOWS
		constexpr ESearchCase::Type PathCase = ESearchCase::IgnoreCase;
#else
		constexpr ESearchCase::Type PathCase = ESearchCase::CaseSensitive;
#endif
		return Path.Equals(Root, PathCase) || FPaths::IsUnderDirectory(Path, Root);
	}

	bool PathTraversesLinkBelowRoot(FString Path, FString Root)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeFilename(Path);
		FPaths::NormalizeDirectoryName(Root);
		if (!IsLexicallyUnderRoot(Path, Root))
		{
			return false;
		}

		FString RelativePath = Path;
		FString RelativeBase = Root;
		if (!RelativeBase.EndsWith(TEXT("/")))
		{
			RelativeBase += TEXT("/");
		}
		if (!FPaths::MakePathRelativeTo(RelativePath, *RelativeBase))
		{
			return true;
		}

		FPaths::NormalizeFilename(RelativePath);
		TArray<FString> Components;
		RelativePath.ParseIntoArray(Components, TEXT("/"), true);
		FString CurrentPath = Root;
		for (const FString& Component : Components)
		{
			if (Component.IsEmpty() || Component == TEXT("."))
			{
				continue;
			}
			if (Component == TEXT(".."))
			{
				return true;
			}

			CurrentPath /= Component;
			if (FPlatformFileManager::Get().GetPlatformPhysical().IsSymlink(*CurrentPath) == ESymlinkResult::Symlink)
			{
				return true;
			}
		}
		return false;
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

		OutFilePath = FPaths::IsRelative(RawPath)
			? FPaths::ConvertRelativePathToFull(ProjectDir, RawPath)
			: FPaths::ConvertRelativePathToFull(RawPath);
		FPaths::NormalizeFilename(OutFilePath);

		if (!IsLexicallyUnderRoot(OutFilePath, ProjectDir))
		{
			OutError = FString::Printf(TEXT("CSV file path '%s' must stay under project directory '%s'"), *OutFilePath, *ProjectDir);
			return false;
		}
		if (PathTraversesLinkBelowRoot(OutFilePath, ProjectDir))
		{
			OutError = FString::Printf(
				TEXT("CSV file path '%s' traverses a symlink or junction below project directory '%s'"),
				*OutFilePath,
				*ProjectDir);
			return false;
		}
		// The contract describes file_path as a CSV destination, and export
		// overwrites the target. Without an extension check a confirmed export
		// could replace Config/DefaultEngine.ini or the .uproject with CSV text.
		const FString Extension = FPaths::GetExtension(OutFilePath);
		if (!Extension.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("CSV file path '%s' must use the .csv extension; refusing to read or overwrite a non-CSV file"),
				*OutFilePath);
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
		// Tool actions must surface filesystem/save failures as structured
		// results. Without SAVE_NoError, commandlet-backed automation can route
		// an ordinary access-denied failure through the fatal error device before
		// this function has a chance to roll the mutation back.
		SaveArgs.SaveFlags = SAVE_NoError;
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

	/**
	 * These exports are opened directly by translators in Excel or LibreOffice,
	 * which evaluate any cell starting with one of these characters as a formula
	 * regardless of CSV quoting. Localization source strings are attacker
	 * influenced content, so a leading formula character is neutralized with a
	 * versioned single-quote marker. A raw value that already begins with the
	 * marker is escaped by doubling it, making import unambiguous even for literal
	 * values such as "'=not-a-formula".
	 */
	bool CellStartsSpreadsheetFormula(const FString& Cell)
	{
		if (Cell.IsEmpty())
		{
			return false;
		}
		const TCHAR First = Cell[0];
		return First == TEXT('=')
			|| First == TEXT('+')
			|| First == TEXT('-')
			|| First == TEXT('@')
			|| First == TEXT('\t')
			|| First == TEXT('\r');
	}

	FString EscapeCsvCell(const FString& Cell)
	{
		FString Escaped = Cell;
		if (Escaped.StartsWith(SpreadsheetFormulaGuard, ESearchCase::CaseSensitive))
		{
			Escaped.InsertAt(0, SpreadsheetFormulaGuard);
		}
		else if (CellStartsSpreadsheetFormula(Escaped))
		{
			Escaped.InsertAt(0, SpreadsheetFormulaGuard);
		}
		Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""));
		if (Escaped.Contains(TEXT(",")) || Escaped.Contains(TEXT("\"")) || Escaped.Contains(TEXT("\r")) || Escaped.Contains(TEXT("\n")))
		{
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}
		return Escaped;
	}

	/**
	 * Reverses only the versioned marker emitted by EscapeCsvCell. Literal leading
	 * apostrophes are never stripped, and a doubled marker decodes to one literal
	 * marker.
	 */
	FString UnescapeCsvFormulaGuard(const FString& Cell)
	{
		if (!Cell.StartsWith(SpreadsheetFormulaGuard, ESearchCase::CaseSensitive))
		{
			return Cell;
		}

		const FString Remainder =
			Cell.Mid(FCString::Strlen(SpreadsheetFormulaGuard));
		if (Remainder.StartsWith(SpreadsheetFormulaGuard, ESearchCase::CaseSensitive) ||
			CellStartsSpreadsheetFormula(Remainder))
		{
			return Remainder;
		}
		return Cell;
	}

	FString CsvCellAt(const FCsvParser::FRows::ElementType& Row, int32 Index)
	{
		if (!Row.IsValidIndex(Index) || Row[Index] == nullptr)
		{
			return FString();
		}
		return FString(Row[Index]);
	}

	bool ValidateMetadataKeyText(const FString& MetadataKey, FString& OutError)
	{
		const FString TrimmedKey = MetadataKey.TrimStartAndEnd();
		if (TrimmedKey.IsEmpty())
		{
			OutError = TEXT("Metadata keys must not be empty");
			return false;
		}
		if (TrimmedKey != MetadataKey)
		{
			OutError = FString::Printf(
				TEXT("Metadata key '%s' must not contain leading or trailing whitespace"),
				*MetadataKey);
			return false;
		}
		if (MetadataKey.Equals(MetadataPresenceCsvHeader, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Metadata key '%s' is reserved for lossless CSV metadata presence"),
				*MetadataKey);
			return false;
		}
		return true;
	}

	FString SerializeMetadataPresence(const TMap<FString, FString>& Metadata)
	{
		TArray<FString> MetadataKeys;
		Metadata.GenerateKeyArray(MetadataKeys);
		MetadataKeys.Sort();

		TArray<TSharedPtr<FJsonValue>> JsonKeys;
		JsonKeys.Reserve(MetadataKeys.Num());
		for (const FString& MetadataKey : MetadataKeys)
		{
			JsonKeys.Add(MakeShared<FJsonValueString>(MetadataKey));
		}

		FString Serialized;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(JsonKeys, Writer);
		return Serialized;
	}

	bool ParseMetadataPresence(
		const FString& Serialized,
		const TSet<FName>& MetadataColumnIds,
		int32 CsvRowNumber,
		TSet<FName>& OutPresentMetadataIds,
		FString& OutError)
	{
		OutPresentMetadataIds.Reset();
		if (Serialized.IsEmpty())
		{
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> JsonKeys;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
		if (!FJsonSerializer::Deserialize(Reader, JsonKeys))
		{
			OutError = FString::Printf(
				TEXT("CSV row %d has malformed %s JSON"),
				CsvRowNumber,
				MetadataPresenceCsvHeader);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& JsonKey : JsonKeys)
		{
			FString MetadataKey;
			if (!JsonKey.IsValid() || JsonKey->Type != EJson::String || !JsonKey->TryGetString(MetadataKey))
			{
				OutError = FString::Printf(
					TEXT("CSV row %d %s must be an array of metadata-key strings"),
					CsvRowNumber,
					MetadataPresenceCsvHeader);
				return false;
			}

			const FName MetadataId(*MetadataKey);
			if (!MetadataColumnIds.Contains(MetadataId))
			{
				OutError = FString::Printf(
					TEXT("CSV row %d marks unknown metadata column '%s' as present"),
					CsvRowNumber,
					*MetadataKey);
				return false;
			}
			if (OutPresentMetadataIds.Contains(MetadataId))
			{
				OutError = FString::Printf(
					TEXT("CSV row %d repeats metadata key '%s' in %s"),
					CsvRowNumber,
					*MetadataKey,
					MetadataPresenceCsvHeader);
				return false;
			}
			OutPresentMetadataIds.Add(MetadataId);
		}
		return true;
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
		int32 MetadataPresenceIndex = INDEX_NONE;
		TArray<TPair<FString, int32>> MetadataColumns;
		TSet<FName> HeaderIds;
		TSet<FName> MetadataColumnIds;
		for (int32 ColumnIndex = 0; ColumnIndex < Header.Num(); ++ColumnIndex)
		{
			const FString RawHeaderName = FString(Header[ColumnIndex]);
			const FString HeaderName = RawHeaderName.TrimStartAndEnd();
			if (!RawHeaderName.IsEmpty() && RawHeaderName != HeaderName)
			{
				OutError = FString::Printf(
					TEXT("CSV header '%s' must not contain leading or trailing whitespace"),
					*RawHeaderName);
				return ParsedRows;
			}

			// An unnamed column was previously ignored, so every value beneath it was
			// silently discarded while its row still reported "accepted" - a
			// translator's data could be lost without any signal.
			if (HeaderName.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("CSV header column %d has no name; every column must be 'key', 'source_string', %s, or a metadata key"),
					ColumnIndex + 1,
					MetadataPresenceCsvHeader);
				return ParsedRows;
			}

			const FName HeaderId(*HeaderName);
			if (HeaderIds.Contains(HeaderId))
			{
				OutError = FString::Printf(TEXT("CSV header '%s' is duplicated"), *HeaderName);
				return ParsedRows;
			}
			HeaderIds.Add(HeaderId);
			if (HeaderName.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				KeyIndex = ColumnIndex;
			}
			else if (HeaderName.Equals(TEXT("source_string"), ESearchCase::IgnoreCase))
			{
				SourceIndex = ColumnIndex;
			}
			else if (HeaderName.Equals(MetadataPresenceCsvHeader, ESearchCase::IgnoreCase))
			{
				MetadataPresenceIndex = ColumnIndex;
			}
			else
			{
				if (!ValidateMetadataKeyText(HeaderName, OutError))
				{
					return ParsedRows;
				}
				MetadataColumns.Add(TPair<FString, int32>(HeaderName, ColumnIndex));
				MetadataColumnIds.Add(HeaderId);
			}
		}

		if (KeyIndex == INDEX_NONE || SourceIndex == INDEX_NONE)
		{
			OutError = TEXT("CSV header must include 'key' and 'source_string'");
			return ParsedRows;
		}

		TSet<FTextKey> SeenEntryKeys;
		for (int32 RowIndex = 1; RowIndex < Rows.Num(); ++RowIndex)
		{
			const TArray<const TCHAR*>& Row = Rows[RowIndex];
			FStringTableCsvRow ParsedRow;
			ParsedRow.Key = UnescapeCsvFormulaGuard(CsvCellAt(Row, KeyIndex));
			ParsedRow.SourceString =
				UnescapeCsvFormulaGuard(CsvCellAt(Row, SourceIndex));

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

			// Accepting the same key twice produces an entry that no single CSV row
			// represents: with replace_existing the table is cleared once, so the
			// later row wins the source string while the earlier row's metadata
			// survives. Reject the ambiguity rather than defining a merge.
			bool bKeyAlreadySeen = false;
			SeenEntryKeys.Add(FTextKey(ParsedRow.Key), &bKeyAlreadySeen);
			if (bKeyAlreadySeen)
			{
				OutError = FString::Printf(
					TEXT("CSV row %d repeats entry key '%s'; entry keys must be unique"),
					RowIndex + 1,
					*ParsedRow.Key);
				return TArray<FStringTableCsvRow>();
			}

			TSet<FName> PresentMetadataIds;
			if (MetadataPresenceIndex != INDEX_NONE &&
				!ParseMetadataPresence(
					CsvCellAt(Row, MetadataPresenceIndex),
					MetadataColumnIds,
					RowIndex + 1,
					PresentMetadataIds,
					OutError))
			{
				return TArray<FStringTableCsvRow>();
			}

			for (const TPair<FString, int32>& MetadataColumn : MetadataColumns)
			{
				const FString Value =
					UnescapeCsvFormulaGuard(CsvCellAt(Row, MetadataColumn.Value));
				const bool bMetadataIsPresent = MetadataPresenceIndex != INDEX_NONE
					? PresentMetadataIds.Contains(FName(*MetadataColumn.Key))
					: !Value.IsEmpty();
				if (MetadataPresenceIndex != INDEX_NONE && !bMetadataIsPresent && !Value.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("CSV row %d has a value for metadata column '%s' but does not mark it present in %s"),
						RowIndex + 1,
						*MetadataColumn.Key,
						MetadataPresenceCsvHeader);
					return TArray<FStringTableCsvRow>();
				}
				if (bMetadataIsPresent)
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

	bool TryGetStringTableMetaData(
		FStringTableConstRef TableRef,
		const FTextKey& Key,
		const FString& MetadataKey,
		FString& OutValue);

	/**
	 * Returns true when applying Rows would actually alter the table.
	 *
	 * Treating "at least one accepted row" as a change meant re-importing an
	 * unchanged export still called Modify, dirtied the package, reported
	 * changed=true, and rewrote the .uasset under save=true - pure source-control
	 * churn for a no-op.
	 */
	bool ImportWouldChangeStringTable(
		const UStringTable* Table,
		const TArray<FStringTableCsvRow>& Rows,
		bool bReplaceExisting)
	{
		if (!Table)
		{
			return false;
		}
		const FStringTableConstRef TableRef = Table->GetStringTable();

		if (bReplaceExisting)
		{
			// Replacement drops every entry the CSV does not carry.
			TSet<FTextKey> IncomingKeys;
			IncomingKeys.Reserve(Rows.Num());
			for (const FStringTableCsvRow& Row : Rows)
			{
				IncomingKeys.Add(FTextKey(Row.Key));
			}

			bool bHasRemovedEntry = false;
			TableRef->EnumerateKeysAndSourceStrings(
				[&IncomingKeys, &bHasRemovedEntry](const FTextKey& Key, const FString&)
				{
					if (!IncomingKeys.Contains(Key))
					{
						bHasRemovedEntry = true;
						return false;
					}
					return true;
				});
			if (bHasRemovedEntry)
			{
				return true;
			}
		}

		for (const FStringTableCsvRow& Row : Rows)
		{
			const FTextKey TextKey(Row.Key);
			FString ExistingSource;
			if (!TableRef->GetSourceString(TextKey, ExistingSource))
			{
				return true;
			}
			if (!ExistingSource.Equals(Row.SourceString, ESearchCase::CaseSensitive))
			{
				return true;
			}

			// Metadata identity is an FName, so compare by id rather than by the
			// display spelling used in the CSV header.
			TSet<FName> IncomingMetadataIds;
			IncomingMetadataIds.Reserve(Row.Metadata.Num());
			for (const TPair<FString, FString>& MetadataPair : Row.Metadata)
			{
				const FName MetadataId(*MetadataPair.Key);
				IncomingMetadataIds.Add(MetadataId);
				FString ExistingMetadataValue;
				if (!TryGetStringTableMetaData(
						TableRef,
						TextKey,
						MetadataPair.Key,
						ExistingMetadataValue) ||
					!ExistingMetadataValue.Equals(
						MetadataPair.Value,
						ESearchCase::CaseSensitive))
				{
					return true;
				}
			}

			if (bReplaceExisting)
			{
				bool bHasRemovedMetadata = false;
				TableRef->EnumerateMetaData(
					TextKey,
					[&IncomingMetadataIds, &bHasRemovedMetadata](
						FName MetadataId, const FString&)
					{
						if (!IncomingMetadataIds.Contains(MetadataId))
						{
							bHasRemovedMetadata = true;
							return false;
						}
						return true;
					});
				if (bHasRemovedMetadata)
				{
					return true;
				}
			}
		}

		return false;
	}

	void CollectStringTableRows(UStringTable* Table, bool bIncludeMetadata, TArray<FStringTableCsvRow>& OutRows, TArray<FString>& OutMetadataKeys)
	{
		if (!Table)
		{
			return;
		}

		// Metadata identity is an FName, so Owner and owner are the same column.
		// Collecting display strings in a case-sensitive TSet emitted both, and the
		// importer - which compares headers as FName - then rejected the export it
		// had just produced as having duplicate columns. Key the set by FName and
		// keep the first display spelling encountered.
		TMap<FName, FString> MetadataKeysById;
		FStringTableConstRef TableRef = Table->GetStringTable();
		TableRef->EnumerateKeysAndSourceStrings(
			[TableRef, bIncludeMetadata, &OutRows, &MetadataKeysById](const FTextKey& Key, const FString& SourceString)
			{
				FStringTableCsvRow Row;
				Row.Key = Key.ToString();
				Row.SourceString = SourceString;
				if (bIncludeMetadata)
				{
					TableRef->EnumerateMetaData(Key,
						[&Row, &MetadataKeysById](FName MetadataId, const FString& MetadataValue)
						{
							const FString& CanonicalKey =
								MetadataKeysById.FindOrAdd(
									MetadataId,
									MetadataId.ToString());
							Row.Metadata.Add(CanonicalKey, MetadataValue);
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

		for (const TPair<FName, FString>& MetadataKeyPair : MetadataKeysById)
		{
			OutMetadataKeys.Add(MetadataKeyPair.Value);
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

	bool BuildStringTableCsv(
		UStringTable* Table,
		bool bIncludeMetadata,
		FString& OutCsvText,
		int32& OutRowCount,
		FString& OutError)
	{
		TArray<FStringTableCsvRow> Rows;
		TArray<FString> MetadataKeys;
		CollectStringTableRows(Table, bIncludeMetadata, Rows, MetadataKeys);
		OutRowCount = Rows.Num();
		for (const FString& MetadataKey : MetadataKeys)
		{
			if (!ValidateMetadataKeyText(MetadataKey, OutError))
			{
				return false;
			}
			if (MetadataKey.Equals(TEXT("key"), ESearchCase::IgnoreCase) ||
				MetadataKey.Equals(TEXT("source_string"), ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(
					TEXT("Cannot export metadata key '%s' because it conflicts with a reserved CSV header"),
					*MetadataKey);
				return false;
			}
		}

		TArray<FString> Lines;
		TArray<FString> Header;
		Header.Add(TEXT("key"));
		Header.Add(TEXT("source_string"));
		if (MetadataKeys.Num() > 0)
		{
			Header.Add(MetadataPresenceCsvHeader);
		}
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
			if (MetadataKeys.Num() > 0)
			{
				Cells.Add(SerializeMetadataPresence(Row.Metadata));
			}
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

		OutCsvText = FString::Join(Lines, TEXT("\n"));
		return true;
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

		if (Limit <= 0)
		{
			// A summary-only caller needs the total and no rows. Snapshotting and
			// sorting every entry just to discard the result made list_string_tables
			// cost O(n log n) per table for a count it can get directly.
			StringTable->EnumerateKeysAndSourceStrings(
				[&OutTotalCount](const FTextKey&, const FString&)
				{
					++OutTotalCount;
					return true;
				});
			return Rows;
		}

		TArray<FStringTableCsvRow> EntrySnapshots;
		StringTable->EnumerateKeysAndSourceStrings(
			[&EntrySnapshots](const FTextKey& Key, const FString& SourceString)
			{
				FStringTableCsvRow Entry;
				Entry.Key = Key.ToString();
				Entry.SourceString = SourceString;
				EntrySnapshots.Add(MoveTemp(Entry));
				return true;
			});
		EntrySnapshots.Sort([](const FStringTableCsvRow& A, const FStringTableCsvRow& B)
		{
			return A.Key < B.Key;
		});
		OutTotalCount = EntrySnapshots.Num();

		const int32 ReturnCount = FMath::Min(Limit, EntrySnapshots.Num());
		Rows.Reserve(ReturnCount);
		for (int32 Index = 0; Index < ReturnCount; ++Index)
		{
			const FStringTableCsvRow& Entry = EntrySnapshots[Index];
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("key"), Entry.Key);
			Row->SetStringField(TEXT("source_string"), Entry.SourceString);
			Row->SetNumberField(TEXT("source_length"), Entry.SourceString.Len());

			if (bIncludeMetadata)
			{
				TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
				StringTable->EnumerateMetaData(FTextKey(Entry.Key),
					[&Metadata](FName MetadataId, const FString& MetadataValue)
					{
						Metadata->SetStringField(MetadataId.ToString(), MetadataValue);
						return true;
					});
				Row->SetObjectField(TEXT("metadata"), Metadata);
			}

			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
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
		TArray<TSharedPtr<FJsonValue>> Entries = StringTableEntriesToJson(Table, bIncludeEntries ? Limit : 0, bIncludeMetadata, EntryCount);
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
		if (!ReadRequiredStringParam(Params, TEXT("asset_path"), OutAssetPath, OutError))
		{
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
			.OptionalExactType(TEXT("culture_names"), TEXT("array"), TEXT("Optional culture names to resolve; omitted returns configured/default culture context"))
			.Optional(TEXT("include_derived"), TEXT("boolean"), TEXT("Include derived cultures when resolving culture_names"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("list_string_tables"),
		TEXT("List StringTable assets under a project content path."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListStringTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to scan"), TEXT("/Game"))
			.Optional(TEXT("include_entries"), TEXT("boolean"), TEXT("Include capped entry rows"), TEXT("false"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata when entries are included"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum table summaries and aggregate entry rows to return"), TEXT("100"))
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
			.OptionalExactType(TEXT("metadata"), TEXT("object"), TEXT("String metadata fields"))
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
		TEXT("Import key, source_string, and per-metadata-key CSV columns into a StringTable. Requires dry_run=true or confirm=true."),
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
	FString Error;
	if (!ReadOptionalBoolParam(Params, TEXT("include_derived"), bIncludeDerived, Error))
	{
		return InvalidParams(Error);
	}
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonValue> CultureNamesField = Params->TryGetField(TEXT("culture_names"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (CultureNamesField.IsValid() &&
			(CultureNamesField->Type != EJson::Array || !CultureNamesField->TryGetArray(Values) || Values == nullptr))
		{
			return InvalidParams(TEXT("Malformed parameter: culture_names must be an array"));
		}
		if (Values)
		{
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				FString Name;
				if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Name) ||
					Name.TrimStartAndEnd().IsEmpty())
				{
					return InvalidParams(FString::Printf(
						TEXT("Malformed parameter: culture_names[%d] must be a non-empty string"), Index));
				}
				CultureNames.Add(Name);
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
		return InvalidParams(Error);
	}

	int32 Limit;
	FString LimitError;
	if (!GetClampedLimit(Params, 100, Limit, LimitError))
	{
		return InvalidParams(LimitError);
	}
	bool bIncludeEntries = false;
	bool bIncludeMetadata = false;
	if (!ReadOptionalBoolParam(Params, TEXT("include_entries"), bIncludeEntries, Error) ||
		!ReadOptionalBoolParam(Params, TEXT("include_metadata"), bIncludeMetadata, Error))
	{
		return InvalidParams(Error);
	}

	TArray<UStringTable*> Tables = LoadStringTablesUnderPath(Path);
	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 RemainingEntryBudget = bIncludeEntries ? Limit : 0;
	int64 AvailableEntryCount = 0;
	int32 ReturnedEntryCount = 0;
	for (UStringTable* Table : Tables)
	{
		if (Rows.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Summary = StringTableSummaryToJson(
			Table,
			bIncludeEntries,
			bIncludeEntries ? RemainingEntryBudget : 0,
			bIncludeMetadata);
		if (bIncludeEntries)
		{
			const int32 TableEntryCount = static_cast<int32>(Summary->GetIntegerField(TEXT("entry_count")));
			const int32 TableReturnedCount = static_cast<int32>(Summary->GetIntegerField(TEXT("returned_count")));
			AvailableEntryCount += TableEntryCount;
			ReturnedEntryCount += TableReturnedCount;
			RemainingEntryBudget = FMath::Max(0, RemainingEntryBudget - TableReturnedCount);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Summary));
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
	if (bIncludeEntries)
	{
		Result->SetNumberField(TEXT("entry_budget"), Limit);
		Result->SetNumberField(TEXT("available_entry_count"), AvailableEntryCount);
		Result->SetNumberField(TEXT("returned_entry_count"), ReturnedEntryCount);
		Result->SetNumberField(TEXT("truncated_entry_count"), AvailableEntryCount - ReturnedEntryCount);
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
		return InvalidParams(Error);
	}

	bool bIncludeMetadata = true;
	if (!ReadOptionalBoolParam(Params, TEXT("include_metadata"), bIncludeMetadata, Error))
	{
		return InvalidParams(Error);
	}
	int32 Limit;
	FString LimitError;
	if (!GetClampedLimit(Params, 200, Limit, LimitError))
	{
		return InvalidParams(LimitError);
	}

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
		return InvalidParams(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 IssueCount = 0;
	auto AddIssue = [&Issues, &IssueCount](const FString& Code, const FString& Message, const FString& Key = FString())
	{
		++IssueCount;
		if (Issues.Num() >= MaxValidationIssueRows)
		{
			return;
		}

		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Key.IsEmpty())
		{
			Issue->SetStringField(TEXT("key"), Key);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	// EnumerateKeysAndSourceStrings has no stable order, so capping issues at
	// MaxValidationIssueRows during enumeration made an unchanged table return
	// different issue keys across loads or map rehashes. Snapshot and sort by key
	// first so both the issue selection and the duplicate-key report reproduce.
	TArray<FStringTableCsvRow> EntrySnapshots;
	Table->GetStringTable()->EnumerateKeysAndSourceStrings(
		[&EntrySnapshots](const FTextKey& Key, const FString& SourceString)
		{
			FStringTableCsvRow Entry;
			Entry.Key = Key.ToString();
			Entry.SourceString = SourceString;
			EntrySnapshots.Add(MoveTemp(Entry));
			return true;
		});
	EntrySnapshots.Sort([](const FStringTableCsvRow& A, const FStringTableCsvRow& B)
	{
		return A.Key < B.Key;
	});

	TSet<FString> LowerKeys;
	const int32 EntryCount = EntrySnapshots.Num();
	for (const FStringTableCsvRow& Entry : EntrySnapshots)
	{
		const FString& KeyString = Entry.Key;
		if (KeyString.TrimStartAndEnd().IsEmpty())
		{
			AddIssue(TEXT("empty_key"), TEXT("StringTable entry has an empty key."));
		}
		if (Entry.SourceString.IsEmpty())
		{
			AddIssue(TEXT("empty_source_string"), TEXT("StringTable entry has an empty source string."), KeyString);
		}

		const FString Lower = KeyString.ToLower();
		if (LowerKeys.Contains(Lower))
		{
			AddIssue(TEXT("case_insensitive_duplicate_key"), TEXT("Another key differs only by case."), KeyString);
		}
		LowerKeys.Add(Lower);
	}

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
	Result->SetNumberField(TEXT("issue_count"), IssueCount);
	Result->SetNumberField(TEXT("returned_issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("truncated_issue_count"), IssueCount - Issues.Num());
	Result->SetBoolField(TEXT("valid"), IssueCount == 0);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::CreateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	FString RawAssetPath;
	if (!ReadRequiredStringParam(Params, TEXT("asset_path"), RawAssetPath, Error))
	{
		return InvalidParams(Error);
	}

	FString PackagePath, AssetName, AssetPath;
	if (!SplitStringTableAssetPath(RawAssetPath, PackagePath, AssetName, AssetPath, Error))
	{
		return InvalidParams(Error);
	}

	FString Namespace = AssetName;
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonValue> NamespaceField = Params->TryGetField(TEXT("namespace"));
		if (NamespaceField.IsValid() &&
			(NamespaceField->Type != EJson::String || !NamespaceField->TryGetString(Namespace)))
		{
			return InvalidParams(TEXT("Malformed parameter: namespace must be a string"));
		}
	}
	if (Namespace.TrimStartAndEnd().IsEmpty())
	{
		Namespace = AssetName;
	}

	if (DoesStringTableAssetExist(PackagePath, AssetName))
	{
		return InvalidParams(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	const FName TableId = MakeStringTableAssetId(PackagePath, AssetName);
	// A live registry entry under this asset-style id was previously unregistered
	// before CreateAsset was attempted. That destroyed a registration which was
	// never proven stale, the dry run neither performed nor previewed the removal,
	// and a factory failure left the original entry permanently lost. Treat it as
	// a collision instead; the caller can choose a different path or unregister
	// the table deliberately.
	const bool bRegistryIdInUse =
		FStringTableRegistry::Get().FindStringTable(TableId).IsValid();
	if (bRegistryIdInUse)
	{
		return InvalidParams(FString::Printf(
			TEXT("A StringTable is already registered with id '%s'; refusing to unregister a live table to create '%s'"),
			*TableId.ToString(),
			*AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddMutationBaseFields(Result, AssetPath, Options, false, false);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("string_table_id_available"), true);
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
		return InvalidParams(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return InvalidParams(Error);
	}

	FString SourceString;
	if (!ReadRequiredStringParam(Params, TEXT("source_string"), SourceString, Error, true))
	{
		return InvalidParams(Error);
	}

	const TSharedPtr<FJsonObject>* MetadataObject = nullptr;
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonValue> MetadataField = Params->TryGetField(TEXT("metadata"));
		if (MetadataField.IsValid() &&
			(MetadataField->Type != EJson::Object ||
			 !MetadataField->TryGetObject(MetadataObject) ||
			 MetadataObject == nullptr))
		{
			return InvalidParams(TEXT("Malformed parameter: metadata must be an object"));
		}
	}

	TMap<FString, FString> MetadataToSet;
	TSet<FString> NormalizedMetadataKeys;
	if (MetadataObject && MetadataObject->IsValid())
	{
		for (const auto& Pair : (*MetadataObject)->Values)
		{
			const FString MetadataKey = MonolithKeyToString(Pair.Key);
			if (!ValidateMetadataKeyText(MetadataKey, Error))
			{
				return InvalidParams(Error);
			}

			const FString NormalizedMetadataKey = MetadataKey.ToLower();
			if (NormalizedMetadataKeys.Contains(NormalizedMetadataKey))
			{
				return InvalidParams(FString::Printf(
					TEXT("Metadata keys collide case-insensitively as FName: '%s'"),
					*MetadataKey));
			}
			NormalizedMetadataKeys.Add(NormalizedMetadataKey);

			FString MetadataValue;
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(MetadataValue))
			{
				return InvalidParams(FString::Printf(TEXT("Malformed metadata value for '%s': expected string"), *MetadataKey));
			}
			MetadataToSet.Add(MetadataKey, MetadataValue);
		}
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return InvalidParams(Error);
	}

	FString ExistingSource;
	const FTextKey TextKey(Key);
	const bool bHadEntry = Table->GetStringTable()->GetSourceString(TextKey, ExistingSource);

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
	SetSourceStringCompat(MutableTable, TextKey, SourceString);
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
		return InvalidParams(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return InvalidParams(Error);
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return InvalidParams(Error);
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
		return InvalidParams(Error);
	}

	FString Key;
	if (!ReadRequiredStringParam(Params, TEXT("key"), Key, Error))
	{
		return InvalidParams(Error);
	}

	FString MetadataKey;
	if (!ReadRequiredStringParam(Params, TEXT("metadata_key"), MetadataKey, Error))
	{
		return InvalidParams(Error);
	}
	if (!ValidateMetadataKeyText(MetadataKey, Error))
	{
		return InvalidParams(Error);
	}

	bool bRemove = false;
	if (!ReadOptionalBoolParam(Params, TEXT("remove"), bRemove, Error))
	{
		return InvalidParams(Error);
	}

	FString MetadataValue;
	if (!bRemove && !ReadRequiredStringParam(Params, TEXT("metadata_value"), MetadataValue, Error, true))
	{
		return InvalidParams(Error);
	}
	if (bRemove && Params.IsValid())
	{
		const TSharedPtr<FJsonValue> MetadataValueField = Params->TryGetField(TEXT("metadata_value"));
		if (MetadataValueField.IsValid() &&
			(MetadataValueField->Type != EJson::String || !MetadataValueField->TryGetString(MetadataValue)))
		{
			return InvalidParams(TEXT("Malformed parameter: metadata_value must be a string"));
		}
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return InvalidParams(Error);
	}

	const FTextKey TextKey(Key);
	FString SourceString;
	if (!Table->GetStringTable()->GetSourceString(TextKey, SourceString))
	{
		return InvalidParams(FString::Printf(TEXT("StringTable entry '%s' does not exist"), *Key));
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
		Result->SetBoolField(TEXT("would_change"), bRemove ? bHadMetadata : !bHadMetadata || PreviousValue != MetadataValue);
		return FMonolithActionResult::Success(Result);
	}

	const bool bChanged = bRemove ? bHadMetadata : !bHadMetadata || PreviousValue != MetadataValue;
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
		return InvalidParams(Error);
	}

	FString RawFilePath;
	if (!ReadRequiredStringParam(Params, TEXT("file_path"), RawFilePath, Error))
	{
		return InvalidParams(Error);
	}

	FString FilePath;
	if (!ResolveProjectFilePath(RawFilePath, FilePath, Error))
	{
		return InvalidParams(Error);
	}

	bool bReplaceExisting = false;
	if (!ReadOptionalBoolParam(Params, TEXT("replace_existing"), bReplaceExisting, Error))
	{
		return InvalidParams(Error);
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
		return InvalidParams(Error);
	}
	if (bReplaceExisting && Rows.Num() == 0)
	{
		return InvalidParams(TEXT("replace_existing=true requires at least one accepted CSV row; refusing to clear the StringTable from an empty or fully skipped import"));
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return InvalidParams(Error);
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

	const bool bChanged =
		ImportWouldChangeStringTable(Table, Rows, bReplaceExisting);
	bool bSaved = false;
	FString SavedPath;
	if (bChanged)
	{
		UPackage* TablePackage = Table->GetOutermost();
		const bool bPackageWasDirty = TablePackage && TablePackage->IsDirty();

		// Snapshot the current contents so a save failure after a destructive
		// replace can restore them instead of leaving a dirty, rebuilt table that
		// the caller believes was untouched.
		TArray<FStringTableCsvRow> RollbackRows;
		TArray<FString> RollbackMetadataKeys;
		CollectStringTableRows(Table, true, RollbackRows, RollbackMetadataKeys);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		// Developer notes live outside the CSV projection, so they are snapshotted
		// separately or a rollback would silently drop translator context.
		TMap<FString, FString> RollbackDevNotes;
		{
			const FStringTableConstRef SnapshotTable = Table->GetStringTable();
			for (const FStringTableCsvRow& RollbackRow : RollbackRows)
			{
				if (const FStringTableEntryConstPtr ExistingEntry =
					SnapshotTable->FindEntry(FTextKey(RollbackRow.Key)))
				{
					RollbackDevNotes.Add(
						RollbackRow.Key,
						ExistingEntry->GetDevNotes());
				}
			}
		}
#endif

		Table->Modify();
		FStringTableRef MutableTable = Table->GetMutableStringTable();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		TMap<FString, FString> PreservedDevNotes;
		if (bReplaceExisting)
		{
			for (const FStringTableCsvRow& Row : Rows)
			{
				if (const FStringTableEntryConstPtr ExistingEntry = MutableTable->FindEntry(FTextKey(Row.Key)))
				{
					PreservedDevNotes.Add(Row.Key, ExistingEntry->GetDevNotes());
				}
			}
		}
#endif
		if (bReplaceExisting)
		{
			MutableTable->ClearSourceStrings();
		}
		for (const FStringTableCsvRow& Row : Rows)
		{
			const FTextKey TextKey(Row.Key);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
			SetSourceStringCompat(MutableTable, TextKey, Row.SourceString, PreservedDevNotes.Find(Row.Key));
#else
			SetSourceStringCompat(MutableTable, TextKey, Row.SourceString);
#endif
			for (const TPair<FString, FString>& MetadataPair : Row.Metadata)
			{
				MutableTable->SetMetaData(TextKey, FName(*MetadataPair.Key), MetadataPair.Value);
			}
		}
		Table->MarkPackageDirty();

		if (!SaveAssetIfRequested(Table, Options.bSave, bSaved, SavedPath, Error))
		{
			// Restore the snapshot so the failed action is a genuine no-op rather
			// than a committed in-memory mutation waiting to be saved later.
			FStringTableRef RollbackTable = Table->GetMutableStringTable();
			RollbackTable->ClearSourceStrings();
			for (const FStringTableCsvRow& RollbackRow : RollbackRows)
			{
				const FTextKey RollbackKey(RollbackRow.Key);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
				SetSourceStringCompat(
					RollbackTable,
					RollbackKey,
					RollbackRow.SourceString,
					RollbackDevNotes.Find(RollbackRow.Key));
#else
				SetSourceStringCompat(
					RollbackTable,
					RollbackKey,
					RollbackRow.SourceString);
#endif
				for (const TPair<FString, FString>& MetadataPair : RollbackRow.Metadata)
				{
					RollbackTable->SetMetaData(
						RollbackKey,
						FName(*MetadataPair.Key),
						MetadataPair.Value);
				}
			}
			if (TablePackage)
			{
				TablePackage->SetDirtyFlag(bPackageWasDirty);
			}
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
		return InvalidParams(Error);
	}

	FString RawFilePath;
	if (!ReadRequiredStringParam(Params, TEXT("file_path"), RawFilePath, Error))
	{
		return InvalidParams(Error);
	}

	FString FilePath;
	if (!ResolveProjectFilePath(RawFilePath, FilePath, Error))
	{
		return InvalidParams(Error);
	}

	bool bIncludeMetadata = true;
	if (!ReadOptionalBoolParam(Params, TEXT("include_metadata"), bIncludeMetadata, Error))
	{
		return InvalidParams(Error);
	}

	FString AssetPath;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return InvalidParams(Error);
	}

	int32 RowCount = 0;
	FString CsvText;
	if (!BuildStringTableCsv(Table, bIncludeMetadata, CsvText, RowCount, Error))
	{
		return InvalidParams(Error);
	}

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
