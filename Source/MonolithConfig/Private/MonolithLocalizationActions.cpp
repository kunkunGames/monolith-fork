#include "MonolithLocalizationActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithLocalizationTargetConfig.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithStringTableCompat.h"

#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "Commandlets/CommandletHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/ObjectLibrary.h"
#include "Factories/StringTableFactory.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "IAssetTools.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "ISourceControlChangelist.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "LocalizationModule.h"
#include "LocalizationTargetTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Csv/CsvParser.h"
#include "UnrealEdMisc.h"
#include "UObject/Class.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StructOnScope.h"

namespace
{
	FMonolithActionExecutionPolicy LocalizationTargetConfigurationExecutionPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("track_dirty_packages");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		// The handler owns exact numbered-changelist validation plus byte
		// snapshots and rollback for its two project-config files.
		Policy.bTransactionWrapping = false;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}
	constexpr int32 MaxValidationIssueRows = 200;
	const TCHAR* MetadataPresenceCsvHeader = TEXT("__monolith_metadata_presence_v1");
	const TCHAR* SpreadsheetFormulaGuard = TEXT("'__monolith_formula_guard_v1__:");

	struct FLocalizationMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = false;
	};

	struct FPersistedProjectLocalizationTarget
	{
		FString Name;
		TArray<FGatherTextSearchDirectory> TextSearchDirectories;
	};

	struct FStringTableCsvRow
	{
		FString Key;
		FString SourceString;
		TMap<FString, FString> Metadata;
	};

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

	struct FLocalizationPipelinePlan
	{
		FString TargetName;
		TArray<FString> Operations;
		TArray<FString> ConfigPaths;
		FString ContentDirectory;
		FString ProjectDirectory;
		FString ProjectFilePath;
		FString CommandletExecutable;
		FString LogDirectory;
		TArray<FString> ExistingOutputFiles;
		TArray<FString> ReadOnlyOutputFiles;
		int32 TimeoutSeconds = 900;
	};

	struct FLocalizationConfigRunResult
	{
		FString Operation;
		FString ConfigPath;
		FString LogPath;
		FString ProcessArguments;
		FString OutputTail;
		FString Error;
		int32 ExitCode = MIN_int32;
		double DurationSeconds = 0.0;
		bool bLaunched = false;
		bool bCancelled = false;
		bool bTimedOut = false;
	};

	FCriticalSection GLocalizationPipelineStateLock;
	FString GActiveLocalizationPipelineJobId;
	TFuture<void> GLocalizationPipelineFuture;
	bool GLocalizationPipelineShuttingDown = false;
	bool GLocalizationTargetConfigurationActive = false;

	struct FLocalizationSourceControlFileAudit
	{
		FString File;
		FString ActualChangelist;
		FString CheckedOutOtherBy;
		TArray<FString> Blockers;
		bool bStateValid = false;
		bool bStateUnknown = true;
		bool bSourceControlled = false;
		bool bCurrent = false;
		bool bCheckedOut = false;
		bool bAdded = false;
		bool bDeleted = false;
		bool bIgnored = false;
		bool bConflicted = false;
		bool bCanCheckout = false;
		bool bCheckedOutOther = false;
		bool bDefaultChangelist = false;
		bool bMatchesExpectedChangelist = false;
	};

	TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> LocalizationSourceControlFilesToJson(
		const TArray<FLocalizationSourceControlFileAudit>& Files)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Files.Num());
		for (const FLocalizationSourceControlFileAudit& File : Files)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("file"), File.File);
			Row->SetBoolField(TEXT("state_valid"), File.bStateValid);
			Row->SetBoolField(TEXT("state_unknown"), File.bStateUnknown);
			Row->SetBoolField(TEXT("source_controlled"), File.bSourceControlled);
			Row->SetBoolField(TEXT("current"), File.bCurrent);
			Row->SetBoolField(TEXT("checked_out"), File.bCheckedOut);
			Row->SetBoolField(TEXT("added"), File.bAdded);
			Row->SetBoolField(TEXT("deleted"), File.bDeleted);
			Row->SetBoolField(TEXT("ignored"), File.bIgnored);
			Row->SetBoolField(TEXT("conflicted"), File.bConflicted);
			Row->SetBoolField(TEXT("can_checkout"), File.bCanCheckout);
			Row->SetBoolField(TEXT("checked_out_other"), File.bCheckedOutOther);
			Row->SetStringField(
				TEXT("checked_out_other_by"),
				File.CheckedOutOtherBy);
			Row->SetStringField(
				TEXT("actual_changelist"),
				File.ActualChangelist);
			Row->SetBoolField(
				TEXT("default_changelist"),
				File.bDefaultChangelist);
			Row->SetBoolField(
				TEXT("matches_expected_changelist"),
				File.bMatchesExpectedChangelist);
			Row->SetArrayField(TEXT("blockers"), StringsToJson(File.Blockers));
			Result.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Result;
	}

	struct FLocalizationTargetConfigPreview
	{
		TArray<TPair<FString, FString>> DesiredConfigContents;
		TArray<FString> ChangedConfigFiles;
		TArray<FString> MissingFiles;
		TArray<FString> ReadOnlyFiles;
		TArray<FString> SourceControlBlockers;
		TArray<FLocalizationSourceControlFileAudit> SourceControlFiles;
		FString SettingsFile;
		FString SourceControlProvider;
		int32 ExpectedTargetChangelist = 0;
		bool bSourceControlEnabled = false;
		bool bSourceControlProviderEnabled = false;
		bool bSourceControlAvailable = false;
		bool bSourceControlUsesCheckout = false;
		bool bSourceControlUsesChangelists = false;
		bool bSourceControlForceUpdated = false;
		bool bSourceControlReady = false;
		bool bSettingsChanged = false;
	};

	TSharedPtr<FJsonObject> LocalizationSourceControlAuditToJson(
		const FLocalizationTargetConfigPreview& Preview)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("module_enabled"), Preview.bSourceControlEnabled);
		Result->SetBoolField(
			TEXT("provider_enabled"),
			Preview.bSourceControlProviderEnabled);
		Result->SetBoolField(
			TEXT("provider_available"),
			Preview.bSourceControlAvailable);
		Result->SetBoolField(
			TEXT("provider_ready"),
			Preview.bSourceControlEnabled &&
				Preview.bSourceControlProviderEnabled &&
				Preview.bSourceControlAvailable &&
				Preview.bSourceControlUsesCheckout &&
				Preview.bSourceControlUsesChangelists);
		Result->SetStringField(TEXT("provider"), Preview.SourceControlProvider);
		Result->SetBoolField(
			TEXT("uses_checkout"),
			Preview.bSourceControlUsesCheckout);
		Result->SetBoolField(
			TEXT("uses_changelists"),
			Preview.bSourceControlUsesChangelists);
		Result->SetBoolField(
			TEXT("force_updated"),
			Preview.bSourceControlForceUpdated);
		Result->SetNumberField(
			TEXT("expected_changelist"),
			Preview.ExpectedTargetChangelist);
		Result->SetBoolField(TEXT("ready"), Preview.bSourceControlReady);
		Result->SetNumberField(
			TEXT("blocker_count"),
			Preview.SourceControlBlockers.Num());
		Result->SetArrayField(
			TEXT("blockers"),
			StringsToJson(Preview.SourceControlBlockers));
		Result->SetArrayField(
			TEXT("files"),
			LocalizationSourceControlFilesToJson(
				Preview.SourceControlFiles));
		return Result;
	}

	TSharedPtr<FJsonObject> BuildLocalizationHandlerOwnedSourceControlPrepare(
		const FLocalizationTargetConfigPreview& Preview,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Prepare = MakeShared<FJsonObject>();
		Prepare->SetStringField(
			TEXT("mode"),
			TEXT("handler_owned_pre_mutation"));
		Prepare->SetStringField(TEXT("status"), Status);
		Prepare->SetStringField(
			TEXT("expected_changelist"),
			FString::FromInt(
				Preview.ExpectedTargetChangelist));
		Prepare->SetObjectField(
			TEXT("before_action"),
			LocalizationSourceControlAuditToJson(Preview));
		return Prepare;
	}

	struct FLocalizationFileSnapshot
	{
		FString Path;
		TArray<uint8> Bytes;
		bool bExisted = false;
	};

	FString LocalizationGatherDirectoryToString(const FGatherTextSearchDirectory& Directory)
	{
		FString Path = Directory.Path;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Path.StartsWith(TEXT("/")))
		{
			Path.RightChopInline(1);
		}
		while (Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}

		FString RootToken;
		switch (Directory.PathRoot)
		{
		case ELocalizationGatherPathRoot::Engine:
			RootToken = TEXT("%LOCENGINEROOT%");
			break;
		case ELocalizationGatherPathRoot::Project:
			RootToken = TEXT("%LOCPROJECTROOT%");
			break;
		default:
			RootToken = TEXT("%LOCAUTOROOT%");
			break;
		}
		return RootToken + Path;
	}

	TArray<FString> LocalizationGatherDirectoriesToStrings(
		const TArray<FGatherTextSearchDirectory>& Directories)
	{
		TArray<FString> Result;
		Result.Reserve(Directories.Num());
		for (const FGatherTextSearchDirectory& Directory : Directories)
		{
			Result.Add(LocalizationGatherDirectoryToString(Directory));
		}
		return Result;
	}

	bool AreLocalizationGatherDirectoriesEqual(
		const TArray<FGatherTextSearchDirectory>& Left,
		const TArray<FGatherTextSearchDirectory>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (Left[Index].PathRoot != Right[Index].PathRoot ||
				!Left[Index].Path.Equals(Right[Index].Path, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	bool ParseLocalizationGatherDirectory(
		FString Input,
		FGatherTextSearchDirectory& OutDirectory,
		FString& OutCanonical,
		FString& OutError)
	{
		MonolithLocalizationTargetConfig::FParsedSearchDirectory Parsed;
		if (!MonolithLocalizationTargetConfig::ParseSearchDirectory(
				MoveTemp(Input),
				Parsed,
				OutError))
		{
			return false;
		}
		OutDirectory.PathRoot =
			Parsed.Root == MonolithLocalizationTargetConfig::EGatherPathRoot::Engine
				? ELocalizationGatherPathRoot::Engine
				: ELocalizationGatherPathRoot::Project;
		OutDirectory.Path = MoveTemp(Parsed.RelativePath);
		OutCanonical = MoveTemp(Parsed.Canonical);
		return true;
	}

	bool ReadLocalizationGatherDirectories(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FGatherTextSearchDirectory>& OutDirectories,
		TArray<FString>& OutCanonical,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing required param 'search_directories'");
			return false;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("search_directories"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Field.IsValid() || Field->IsNull())
		{
			OutError = TEXT("Missing required param 'search_directories'");
			return false;
		}
		if (!Field->TryGetArray(Values) || Values == nullptr)
		{
			OutError = TEXT("Malformed parameter: search_directories must be an array");
			return false;
		}
		if (Values->IsEmpty() || Values->Num() > 64)
		{
			OutError = TEXT("search_directories must contain between 1 and 64 entries");
			return false;
		}

		TSet<FString> UniqueDirectories;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Input;
			if (!Value.IsValid() || !Value->TryGetString(Input))
			{
				OutError = TEXT("Malformed parameter: every search_directories entry must be a string");
				return false;
			}

			FGatherTextSearchDirectory Directory;
			FString Canonical;
			if (!ParseLocalizationGatherDirectory(Input, Directory, Canonical, OutError))
			{
				return false;
			}

			FString UniqueKey = Canonical.ToLower();
			if (UniqueDirectories.Contains(UniqueKey))
			{
				OutError = FString::Printf(
					TEXT("Duplicate search_directories entry after normalization: %s"),
					*Canonical);
				return false;
			}
			UniqueDirectories.Add(MoveTemp(UniqueKey));
			OutDirectories.Add(MoveTemp(Directory));
			OutCanonical.Add(MoveTemp(Canonical));
		}
		return true;
	}

	FString GetLocalizationSettingsFile()
	{
		FString SettingsFile =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEditor.ini")));
		FPaths::NormalizeFilename(SettingsFile);
		return SettingsFile;
	}

	bool ReadPersistedProjectLocalizationTargets(
		const FString& SettingsFile,
		TArray<FPersistedProjectLocalizationTarget>& OutTargets,
		FString& OutError)
	{
		if (!IFileManager::Get().FileExists(*SettingsFile))
		{
			OutError = FString::Printf(
				TEXT("Localization settings file is missing: %s"),
				*SettingsFile);
			return false;
		}

		UScriptStruct* SettingsStruct = FindObject<UScriptStruct>(
			nullptr,
			TEXT("/Script/Localization.LocalizationTargetSettings"));
		if (!SettingsStruct)
		{
			OutError =
				TEXT("LocalizationTargetSettings reflection type is unavailable after loading the Localization module");
			return false;
		}

		FConfigFile Config;
		Config.Read(SettingsFile);
		TArray<FString> SerializedTargets;
		Config.GetArray(
			TEXT("/Script/Localization.LocalizationSettings"),
			TEXT("GameTargetsSettings"),
			SerializedTargets);

		TSet<FString> UniqueTargetNames;
		for (int32 Index = 0; Index < SerializedTargets.Num(); ++Index)
		{
			FStructOnScope ParsedSettingsScope(SettingsStruct);
			uint8* ParsedSettingsMemory = ParsedSettingsScope.GetStructMemory();
			if (!ParsedSettingsMemory)
			{
				OutError = FString::Printf(
					TEXT("Failed to initialize GameTargetsSettings entry %d through LocalizationTargetSettings reflection"),
					Index);
				return false;
			}

			const TCHAR* ParseEnd = SettingsStruct->ImportText(
				*SerializedTargets[Index],
				ParsedSettingsMemory,
				nullptr,
				PPF_None,
				nullptr,
				TEXT("GameTargetsSettings"));
			if (!ParseEnd)
			{
				OutError = FString::Printf(
					TEXT("Failed to parse GameTargetsSettings entry %d from '%s'"),
					Index,
					*SettingsFile);
				return false;
			}
			const FLocalizationTargetSettings* ParsedSettings =
				reinterpret_cast<const FLocalizationTargetSettings*>(
					ParsedSettingsMemory);
			while (*ParseEnd != TEXT('\0') && FChar::IsWhitespace(*ParseEnd))
			{
				++ParseEnd;
			}
			if (*ParseEnd != TEXT('\0') || ParsedSettings->Name.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("GameTargetsSettings entry %d in '%s' has trailing data or no target name"),
					Index,
					*SettingsFile);
				return false;
			}

			const FString UniqueName = ParsedSettings->Name.ToLower();
			if (UniqueTargetNames.Contains(UniqueName))
			{
				OutError = FString::Printf(
					TEXT("Localization settings contain duplicate project target '%s'"),
					*ParsedSettings->Name);
				return false;
			}
			UniqueTargetNames.Add(UniqueName);

			FPersistedProjectLocalizationTarget Target;
			Target.Name = ParsedSettings->Name;
			Target.TextSearchDirectories =
				ParsedSettings->GatherFromTextFiles.SearchDirectories;
			OutTargets.Add(MoveTemp(Target));
		}
		return true;
	}

	bool ReadLocalizationSourceControlContract(
		const TSharedPtr<FJsonObject>& Params,
		int32& OutChangelist,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError =
				TEXT("source_control_policy=require_checked_out and target_changelist are required");
			return false;
		}
		FString SourceControlPolicy;
		if (!Params->TryGetStringField(
				TEXT("source_control_policy"),
				SourceControlPolicy) ||
			!SourceControlPolicy.Equals(
				TEXT("require_checked_out"),
				ESearchCase::CaseSensitive))
		{
			OutError =
				TEXT("source_control_policy currently permits only require_checked_out");
			return false;
		}

		const TSharedPtr<FJsonValue> Field =
			Params->TryGetField(TEXT("target_changelist"));
		double Number = 0.0;
		if (!Field.IsValid() || Field->IsNull())
		{
			OutError =
				TEXT("target_changelist is required and must be an exact positive numbered changelist; default is forbidden");
			return false;
		}
		if (!Field->TryGetNumber(Number) ||
			!FMath::IsFinite(Number) ||
			Number < 1.0 ||
			Number > static_cast<double>(MAX_int32) ||
			Number != FMath::FloorToDouble(Number))
		{
			OutError =
				TEXT("target_changelist is required and must be an exact positive numbered changelist; default is forbidden");
			return false;
		}

		OutChangelist = static_cast<int32>(Number);
		return true;
	}

	bool FindProjectLocalizationTarget(
		const FString& TargetName,
		const FString& SettingsFile,
		ULocalizationTarget*& OutTarget,
		TArray<FString>& OutAvailableTargets,
		FString& OutError)
	{
		OutTarget = nullptr;
		ILocalizationModule* LocalizationModule =
			FModuleManager::LoadModulePtr<ILocalizationModule>(TEXT("Localization"));
		if (!LocalizationModule)
		{
			OutError =
				TEXT("The engine Localization module could not be loaded; project target configuration is unavailable");
			return false;
		}

		TArray<FPersistedProjectLocalizationTarget> PersistedTargets;
		if (!ReadPersistedProjectLocalizationTargets(
				SettingsFile,
				PersistedTargets,
				OutError))
		{
			return false;
		}

		const FPersistedProjectLocalizationTarget* PersistedMatch = nullptr;
		for (const FPersistedProjectLocalizationTarget& Settings : PersistedTargets)
		{
			OutAvailableTargets.Add(Settings.Name);
			if (Settings.Name.Equals(TargetName, ESearchCase::IgnoreCase))
			{
				PersistedMatch = &Settings;
			}
		}
		OutAvailableTargets.Sort();
		if (!PersistedMatch)
		{
			return true;
		}

		OutTarget = LocalizationModule->GetLocalizationTargetByName(
			PersistedMatch->Name,
			/*bIsEngineTarget=*/false);
		if (!OutTarget)
		{
			OutError = FString::Printf(
				TEXT("Project localization target '%s' exists in DefaultEditor.ini but is absent from the live Localization Dashboard model"),
				*PersistedMatch->Name);
			return false;
		}
		if (!OutTarget->Settings.Name.Equals(
				PersistedMatch->Name,
				ESearchCase::CaseSensitive) ||
			!AreLocalizationGatherDirectoriesEqual(
				OutTarget->Settings.GatherFromTextFiles.SearchDirectories,
				PersistedMatch->TextSearchDirectories))
		{
			OutError = FString::Printf(
				TEXT("Project localization target '%s' live model is stale relative to DefaultEditor.ini; reload the editor before mutating it"),
				*PersistedMatch->Name);
			OutTarget = nullptr;
			return false;
		}
		return true;
	}

	void AddLocalizationSourceControlBlocker(
		FLocalizationTargetConfigPreview& Preview,
		FLocalizationSourceControlFileAudit& File,
		const FString& Blocker)
	{
		File.Blockers.AddUnique(Blocker);
		Preview.SourceControlBlockers.AddUnique(Blocker);
	}

	void AuditLocalizationSourceControlOwnership(
		const TArray<FString>& WriteFiles,
		const int32 TargetChangelist,
		FLocalizationTargetConfigPreview& OutPreview)
	{
		OutPreview.ExpectedTargetChangelist = TargetChangelist;
		if (WriteFiles.IsEmpty())
		{
			OutPreview.bSourceControlReady = true;
			return;
		}

		ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
		OutPreview.bSourceControlEnabled = SourceControlModule.IsEnabled();
		if (!OutPreview.bSourceControlEnabled)
		{
			OutPreview.SourceControlProvider = TEXT("disabled");
			for (const FString& FilePath : WriteFiles)
			{
				FLocalizationSourceControlFileAudit& File =
					OutPreview.SourceControlFiles.AddDefaulted_GetRef();
				File.File = FilePath;
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("provider_disabled"));
			}
			return;
		}

		ISourceControlProvider& Provider = SourceControlModule.GetProvider();
		OutPreview.SourceControlProvider = Provider.GetName().ToString();
		OutPreview.bSourceControlProviderEnabled = Provider.IsEnabled();
		OutPreview.bSourceControlAvailable = Provider.IsAvailable();
		OutPreview.bSourceControlUsesCheckout = Provider.UsesCheckout();
		OutPreview.bSourceControlUsesChangelists = Provider.UsesChangelists();

		TArray<FString> ProviderBlockers;
		if (!OutPreview.bSourceControlProviderEnabled)
		{
			ProviderBlockers.Add(TEXT("provider_disabled"));
		}
		if (!OutPreview.bSourceControlAvailable)
		{
			ProviderBlockers.Add(TEXT("provider_unavailable"));
		}
		if (!OutPreview.bSourceControlUsesCheckout)
		{
			ProviderBlockers.Add(TEXT("provider_without_checkout"));
		}
		if (!OutPreview.bSourceControlUsesChangelists)
		{
			ProviderBlockers.Add(TEXT("provider_without_changelists"));
		}

		const FString ExpectedIdentifier =
			FString::FromInt(TargetChangelist);
		for (const FString& FilePath : WriteFiles)
		{
			FLocalizationSourceControlFileAudit& File =
				OutPreview.SourceControlFiles.AddDefaulted_GetRef();
			File.File = FilePath;
			for (const FString& ProviderBlocker : ProviderBlockers)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					ProviderBlocker);
			}
			if (!ProviderBlockers.IsEmpty())
			{
				continue;
			}

			const FSourceControlStatePtr State = Provider.GetState(
				FilePath,
				EStateCacheUsage::ForceUpdate);
			OutPreview.bSourceControlForceUpdated = true;
			File.bStateValid = State.IsValid();
			if (!State.IsValid())
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("state_unavailable"));
				continue;
			}

			File.bStateUnknown = State->IsUnknown();
			File.bSourceControlled = State->IsSourceControlled();
			File.bCurrent = State->IsCurrent();
			File.bAdded = State->IsAdded();
			File.bDeleted = State->IsDeleted();
			File.bIgnored = State->IsIgnored();
			File.bConflicted = State->IsConflicted();
			File.bCanCheckout = State->CanCheckout();
			File.bCheckedOut = State->IsCheckedOut();
			File.bCheckedOutOther =
				State->IsCheckedOutOther(&File.CheckedOutOtherBy);

			const FSourceControlChangelistPtr OpenedChangelist =
				State->GetCheckInIdentifier();
			if (OpenedChangelist.IsValid())
			{
				File.ActualChangelist =
					OpenedChangelist->GetIdentifier();
				File.bDefaultChangelist =
					OpenedChangelist->IsDefault();
				File.bMatchesExpectedChangelist =
					!File.bDefaultChangelist &&
					File.ActualChangelist.Equals(
						ExpectedIdentifier,
						ESearchCase::CaseSensitive);
			}

			if (File.bStateUnknown)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("state_unknown"));
			}
			if (!File.bSourceControlled)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("not_source_controlled"));
			}
			if (File.bAdded)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("opened_for_add"));
			}
			if (File.bDeleted)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("opened_for_delete"));
			}
			if (File.bIgnored)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("ignored"));
			}
			if (!File.bCurrent)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("not_current"));
			}
			if (File.bConflicted)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("conflicted"));
			}
			if (File.bCheckedOutOther)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("checked_out_other"));
			}
			if (!File.bCheckedOut)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("not_checked_out_by_current_client"));
			}
			if (!OpenedChangelist.IsValid())
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("opened_changelist_unavailable"));
			}
			else if (File.bDefaultChangelist)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("opened_in_default_changelist"));
			}
			else if (!File.bMatchesExpectedChangelist)
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("opened_changelist_mismatch"));
			}
			if (IFileManager::Get().IsReadOnly(*FilePath))
			{
				AddLocalizationSourceControlBlocker(
					OutPreview,
					File,
					TEXT("filesystem_read_only"));
			}
		}

		OutPreview.bSourceControlReady =
			OutPreview.SourceControlBlockers.IsEmpty();
	}

	bool BuildLocalizationTargetConfigPreview(
		ULocalizationTarget* Target,
		const TArray<FGatherTextSearchDirectory>& DesiredDirectories,
		const int32 TargetChangelist,
		FLocalizationTargetConfigPreview& OutPreview,
		FString& OutError)
	{
		if (!Target)
		{
			OutError = TEXT("Cannot preview localization configs for a null target");
			return false;
		}

		OutPreview.SettingsFile = GetLocalizationSettingsFile();
		OutPreview.bSettingsChanged = !AreLocalizationGatherDirectoriesEqual(
			Target->Settings.GatherFromTextFiles.SearchDirectories,
			DesiredDirectories);

		FString GatherConfigPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectConfigDir(),
				TEXT("Localization"),
				FString::Printf(
					TEXT("%s_Gather.ini"),
					*Target->Settings.Name)));
		FPaths::NormalizeFilename(GatherConfigPath);
		if (!IFileManager::Get().FileExists(*GatherConfigPath))
		{
			OutPreview.ChangedConfigFiles.Add(GatherConfigPath);
			OutPreview.MissingFiles.Add(GatherConfigPath);
		}
		else
		{
			FString ExistingContents;
			if (!FFileHelper::LoadFileToString(
					ExistingContents,
					*GatherConfigPath))
			{
				OutError = FString::Printf(
					TEXT("Failed to read existing localization gather config '%s'"),
					*GatherConfigPath);
				return false;
			}

			MonolithLocalizationTargetConfig::FGatherConfigPatch Patch;
			if (!MonolithLocalizationTargetConfig::BuildGatherConfigPatch(
					ExistingContents,
					Target->Settings.Name,
					LocalizationGatherDirectoriesToStrings(DesiredDirectories),
					Patch,
					OutError))
			{
				OutError = FString::Printf(
					TEXT("Localization gather config '%s' is not canonical enough for a targeted patch: %s"),
					*GatherConfigPath,
					*OutError);
				return false;
			}
			OutPreview.DesiredConfigContents.Emplace(
				GatherConfigPath,
				MoveTemp(Patch.DesiredContents));
			if (Patch.bChanged)
			{
				OutPreview.ChangedConfigFiles.Add(GatherConfigPath);
				if (IFileManager::Get().IsReadOnly(*GatherConfigPath))
				{
					OutPreview.ReadOnlyFiles.Add(GatherConfigPath);
				}
			}
		}

		if (OutPreview.bSettingsChanged)
		{
			if (!IFileManager::Get().FileExists(*OutPreview.SettingsFile))
			{
				OutPreview.MissingFiles.Add(OutPreview.SettingsFile);
			}
			else if (IFileManager::Get().IsReadOnly(*OutPreview.SettingsFile))
			{
				OutPreview.ReadOnlyFiles.Add(OutPreview.SettingsFile);
			}
		}

		TArray<FString> WriteFiles = OutPreview.ChangedConfigFiles;
		if (OutPreview.bSettingsChanged)
		{
			WriteFiles.AddUnique(OutPreview.SettingsFile);
		}
		WriteFiles.Sort();
		AuditLocalizationSourceControlOwnership(
			WriteFiles,
			TargetChangelist,
			OutPreview);

		OutPreview.ChangedConfigFiles.Sort();
		OutPreview.MissingFiles.Sort();
		OutPreview.ReadOnlyFiles.Sort();
		OutPreview.SourceControlBlockers.Sort();
		return true;
	}

	bool WriteLocalizationTargetConfigs(
		const FLocalizationTargetConfigPreview& Preview,
		FString& OutError)
	{
		for (const TPair<FString, FString>& Pair : Preview.DesiredConfigContents)
		{
			if (!Preview.ChangedConfigFiles.Contains(Pair.Key))
			{
				continue;
			}
			if (!FFileHelper::SaveStringToFile(
					Pair.Value,
					*Pair.Key,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(
					TEXT("Failed to write targeted localization gather config '%s'"),
					*Pair.Key);
				return false;
			}
		}
		return true;
	}

	bool VerifyLocalizationTargetConfigContents(
		const FLocalizationTargetConfigPreview& Preview,
		FString& OutError)
	{
		for (const TPair<FString, FString>& Pair : Preview.DesiredConfigContents)
		{
			FString ActualContents;
			if (!FFileHelper::LoadFileToString(ActualContents, *Pair.Key) ||
				!ActualContents.Equals(Pair.Value, ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(
					TEXT("Localization gather-config readback did not match the targeted patch text: %s"),
					*Pair.Key);
				return false;
			}
		}
		return true;
	}

	bool VerifyPersistedLocalizationTargetDirectories(
		const FString& SettingsFile,
		const FString& TargetName,
		const TArray<FGatherTextSearchDirectory>& ExpectedDirectories,
		FString& OutError)
	{
		TArray<FPersistedProjectLocalizationTarget> PersistedTargets;
		if (!ReadPersistedProjectLocalizationTargets(
				SettingsFile,
				PersistedTargets,
				OutError))
		{
			return false;
		}

		for (const FPersistedProjectLocalizationTarget& PersistedTarget : PersistedTargets)
		{
			if (!PersistedTarget.Name.Equals(TargetName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (!AreLocalizationGatherDirectoriesEqual(
					PersistedTarget.TextSearchDirectories,
					ExpectedDirectories))
			{
				OutError = FString::Printf(
					TEXT("Persisted localization target '%s' search directories do not match the requested model"),
					*TargetName);
				return false;
			}
			return true;
		}

		OutError = FString::Printf(
			TEXT("Persisted localization target '%s' was not found in '%s'"),
			*TargetName,
			*SettingsFile);
		return false;
	}

	bool CaptureLocalizationFileSnapshots(
		const TArray<FString>& Paths,
		TArray<FLocalizationFileSnapshot>& OutSnapshots,
		FString& OutError)
	{
		for (const FString& Path : Paths)
		{
			FLocalizationFileSnapshot Snapshot;
			Snapshot.Path = Path;
			Snapshot.bExisted = IFileManager::Get().FileExists(*Path);
			if (Snapshot.bExisted && !FFileHelper::LoadFileToArray(Snapshot.Bytes, *Path))
			{
				OutError = FString::Printf(
					TEXT("Failed to snapshot localization file before mutation: %s"),
					*Path);
				return false;
			}
			OutSnapshots.Add(MoveTemp(Snapshot));
		}
		return true;
	}

	bool RestoreLocalizationFileSnapshots(
		const TArray<FLocalizationFileSnapshot>& Snapshots,
		FString& OutError)
	{
		TArray<FString> Failures;
		for (const FLocalizationFileSnapshot& Snapshot : Snapshots)
		{
			const bool bRestored = Snapshot.bExisted
				? FFileHelper::SaveArrayToFile(Snapshot.Bytes, *Snapshot.Path)
				: (!IFileManager::Get().FileExists(*Snapshot.Path) ||
					IFileManager::Get().Delete(*Snapshot.Path, false, true, true));
			if (!bRestored)
			{
				Failures.Add(Snapshot.Path);
			}
		}

		if (!Failures.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Failed to restore localization files after mutation failure: %s"),
				*FString::Join(Failures, TEXT(", ")));
			return false;
		}
		return true;
	}

	bool IsValidLocalizationTargetName(const FString& TargetName, FString& OutError)
	{
		if (TargetName.IsEmpty() || TargetName.Len() > 64)
		{
			OutError = TEXT("target must contain 1-64 ASCII letters, digits, underscores, or hyphens");
			return false;
		}

		for (const TCHAR Character : TargetName)
		{
			const bool bAsciiLetter =
				(Character >= 'A' && Character <= 'Z') ||
				(Character >= 'a' && Character <= 'z');
			const bool bAsciiDigit = Character >= '0' && Character <= '9';
			if (!bAsciiLetter && !bAsciiDigit && Character != '_' && Character != '-')
			{
				OutError = TEXT("target must contain only ASCII letters, digits, underscores, or hyphens");
				return false;
			}
		}
		return true;
	}

	bool ReadLocalizationPipelineOperations(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutOperations,
		FString& OutError)
	{
		OutOperations = {TEXT("gather"), TEXT("compile")};
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> OperationsField = Params->TryGetField(TEXT("operations"));
		if (!OperationsField.IsValid() || OperationsField->IsNull())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!OperationsField->TryGetArray(Values) || Values == nullptr)
		{
			OutError = TEXT("Malformed parameter: operations must be an array");
			return false;
		}
		if (Values->IsEmpty() || Values->Num() > 2)
		{
			OutError = TEXT("operations must contain one or two entries from: gather, compile");
			return false;
		}

		OutOperations.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Operation;
			if (!Value.IsValid() || !Value->TryGetString(Operation))
			{
				OutError = TEXT("Malformed parameter: every operations entry must be a string");
				return false;
			}

			Operation = Operation.TrimStartAndEnd().ToLower();
			if (Operation != TEXT("gather") && Operation != TEXT("compile"))
			{
				OutError = FString::Printf(
					TEXT("Unsupported localization pipeline operation '%s'; allowed values are gather and compile"),
					*Operation);
				return false;
			}
			if (OutOperations.Contains(Operation))
			{
				OutError = FString::Printf(TEXT("Duplicate localization pipeline operation '%s'"), *Operation);
				return false;
			}
			OutOperations.Add(Operation);
		}

		const int32 GatherIndex = OutOperations.IndexOfByKey(TEXT("gather"));
		const int32 CompileIndex = OutOperations.IndexOfByKey(TEXT("compile"));
		if (GatherIndex != INDEX_NONE && CompileIndex != INDEX_NONE && CompileIndex < GatherIndex)
		{
			OutError = TEXT("operations must place gather before compile");
			return false;
		}
		return true;
	}

	bool ReadLocalizationPipelineTimeout(
		const TSharedPtr<FJsonObject>& Params,
		int32& OutTimeoutSeconds,
		FString& OutError)
	{
		OutTimeoutSeconds = 900;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> TimeoutField = Params->TryGetField(TEXT("timeout_seconds"));
		if (!TimeoutField.IsValid() || TimeoutField->IsNull())
		{
			return true;
		}

		double TimeoutNumber = 0.0;
		if (!TimeoutField->TryGetNumber(TimeoutNumber) ||
			!FMath::IsFinite(TimeoutNumber) ||
			!FMath::IsNearlyZero(FMath::Frac(TimeoutNumber)))
		{
			OutError = TEXT("Malformed parameter: timeout_seconds must be an integer");
			return false;
		}

		if (TimeoutNumber < 30.0 || TimeoutNumber > 3600.0)
		{
			OutError = TEXT("timeout_seconds must be between 30 and 3600");
			return false;
		}

		OutTimeoutSeconds = static_cast<int32>(TimeoutNumber);
		return true;
	}

	FString NormalizeLocalizationContentSetting(FString Value)
	{
		Value = Value.TrimStartAndEnd();
		Value.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Value.EndsWith(TEXT("/")))
		{
			Value.LeftChopInline(1);
		}
		return Value;
	}

	bool ValidateLocalizationPipelineConfig(
		const FString& ConfigPath,
		const FString& TargetName,
		const FString& Operation,
		FString& OutError)
	{
		FConfigFile Config;
		Config.Read(ConfigPath);

		FString SourcePath;
		FString DestinationPath;
		FString CommandletClass;
		if (!Config.GetString(TEXT("CommonSettings"), TEXT("SourcePath"), SourcePath) ||
			!Config.GetString(TEXT("CommonSettings"), TEXT("DestinationPath"), DestinationPath))
		{
			OutError = FString::Printf(
				TEXT("Localization config '%s' must define CommonSettings SourcePath and DestinationPath"),
				*ConfigPath);
			return false;
		}
		if (!Config.GetString(TEXT("GatherTextStep0"), TEXT("CommandletClass"), CommandletClass) ||
			CommandletClass.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Localization config '%s' must define GatherTextStep0 CommandletClass"),
				*ConfigPath);
			return false;
		}

		const FString ExpectedContentPath =
			FString::Printf(TEXT("Content/Localization/%s"), *TargetName);
		SourcePath = NormalizeLocalizationContentSetting(SourcePath);
		DestinationPath = NormalizeLocalizationContentSetting(DestinationPath);
		if (!SourcePath.Equals(ExpectedContentPath, ESearchCase::IgnoreCase) ||
			!DestinationPath.Equals(ExpectedContentPath, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Localization config '%s' must keep SourcePath and DestinationPath at '%s'"),
				*ConfigPath,
				*ExpectedContentPath);
			return false;
		}

		if (Operation == TEXT("compile") &&
			!CommandletClass.Equals(TEXT("GenerateTextLocalizationResource"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Compile config '%s' must start with GenerateTextLocalizationResource, not '%s'"),
				*ConfigPath,
				*CommandletClass);
			return false;
		}
		return true;
	}

	bool IsExistingLocalizationPipelineOutput(
		const FString& FilePath,
		const TArray<FString>& Operations)
	{
		const FString Extension = FString::Printf(TEXT(".%s"), *FPaths::GetExtension(FilePath, false)).ToLower();
		if (Operations.Contains(TEXT("gather")) &&
			(Extension == TEXT(".manifest") ||
			 Extension == TEXT(".archive") ||
			 Extension == TEXT(".csv") ||
			 Extension == TEXT(".txt")))
		{
			return true;
		}
		if (Operations.Contains(TEXT("compile")) &&
			(Extension == TEXT(".locres") || Extension == TEXT(".locmeta")))
		{
			return true;
		}
		return false;
	}

	bool BuildLocalizationPipelinePlan(
		const TSharedPtr<FJsonObject>& Params,
		FLocalizationPipelinePlan& OutPlan,
		FString& OutError)
	{
		if (!ReadRequiredStringParam(Params, TEXT("target"), OutPlan.TargetName, OutError))
		{
			return false;
		}
		OutPlan.TargetName = OutPlan.TargetName.TrimStartAndEnd();
		if (!IsValidLocalizationTargetName(OutPlan.TargetName, OutError) ||
			!ReadLocalizationPipelineOperations(Params, OutPlan.Operations, OutError) ||
			!ReadLocalizationPipelineTimeout(Params, OutPlan.TimeoutSeconds, OutError))
		{
			return false;
		}

		OutPlan.ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(OutPlan.ProjectDirectory);
		OutPlan.ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		FPaths::NormalizeFilename(OutPlan.ProjectFilePath);
		if (!IFileManager::Get().FileExists(*OutPlan.ProjectFilePath))
		{
			OutError = FString::Printf(TEXT("Project file does not exist: %s"), *OutPlan.ProjectFilePath);
			return false;
		}

		OutPlan.CommandletExecutable =
			FPaths::ConvertRelativePathToFull(FUnrealEdMisc::Get().GetExecutableForCommandlets());
		FPaths::NormalizeFilename(OutPlan.CommandletExecutable);
		if (!IFileManager::Get().FileExists(*OutPlan.CommandletExecutable))
		{
			OutError = FString::Printf(
				TEXT("Unreal commandlet executable does not exist: %s"),
				*OutPlan.CommandletExecutable);
			return false;
		}

		const FString ConfigDirectory =
			FPaths::Combine(OutPlan.ProjectDirectory, TEXT("Config/Localization"));
		for (const FString& Operation : OutPlan.Operations)
		{
			const FString OperationSuffix = Operation == TEXT("gather") ? TEXT("Gather") : TEXT("Compile");
			FString ConfigPath = FPaths::Combine(
				ConfigDirectory,
				FString::Printf(TEXT("%s_%s.ini"), *OutPlan.TargetName, *OperationSuffix));
			ConfigPath = FPaths::ConvertRelativePathToFull(ConfigPath);
			FPaths::NormalizeFilename(ConfigPath);
			if (!IFileManager::Get().FileExists(*ConfigPath))
			{
				OutError = FString::Printf(
					TEXT("Localization %s config does not exist: %s"),
					*Operation,
					*ConfigPath);
				return false;
			}
			if (!ValidateLocalizationPipelineConfig(ConfigPath, OutPlan.TargetName, Operation, OutError))
			{
				return false;
			}
			OutPlan.ConfigPaths.Add(ConfigPath);
		}

		OutPlan.ContentDirectory = FPaths::Combine(
			OutPlan.ProjectDirectory,
			TEXT("Content/Localization"),
			OutPlan.TargetName);
		OutPlan.ContentDirectory = FPaths::ConvertRelativePathToFull(OutPlan.ContentDirectory);
		FPaths::NormalizeDirectoryName(OutPlan.ContentDirectory);
		OutPlan.LogDirectory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Monolith/LocalizationJobs"));
		OutPlan.LogDirectory = FPaths::ConvertRelativePathToFull(OutPlan.LogDirectory);
		FPaths::NormalizeDirectoryName(OutPlan.LogDirectory);

		TArray<FString> ExistingFiles;
		if (IFileManager::Get().DirectoryExists(*OutPlan.ContentDirectory))
		{
			IFileManager::Get().FindFilesRecursive(
				ExistingFiles,
				*OutPlan.ContentDirectory,
				TEXT("*"),
				true,
				false,
				false);
		}
		ExistingFiles.Sort();
		for (FString FilePath : ExistingFiles)
		{
			FPaths::NormalizeFilename(FilePath);
			if (!IsExistingLocalizationPipelineOutput(FilePath, OutPlan.Operations))
			{
				continue;
			}
			OutPlan.ExistingOutputFiles.Add(FilePath);
			if (IFileManager::Get().IsReadOnly(*FilePath))
			{
				OutPlan.ReadOnlyOutputFiles.Add(FilePath);
			}
		}
		return true;
	}

	bool MakePathRelativeToProject(FString& InOutPath, const FString& ProjectDirectory)
	{
		FString ProjectDirectoryWithSlash = ProjectDirectory;
		FPaths::NormalizeDirectoryName(ProjectDirectoryWithSlash);
		ProjectDirectoryWithSlash += TEXT("/");
		return FPaths::MakePathRelativeTo(InOutPath, *ProjectDirectoryWithSlash);
	}

	bool CaptureLocalizationArtifactHashes(
		const FString& ContentDirectory,
		const FString& ProjectDirectory,
		TMap<FString, FString>& OutHashes,
		FString& OutError)
	{
		OutHashes.Reset();
		TArray<FString> Files;
		if (IFileManager::Get().DirectoryExists(*ContentDirectory))
		{
			IFileManager::Get().FindFilesRecursive(
				Files,
				*ContentDirectory,
				TEXT("*"),
				true,
				false,
				false);
		}
		Files.Sort();

		for (FString FilePath : Files)
		{
			FPaths::NormalizeFilename(FilePath);
			FString RelativePath = FilePath;
			if (!MakePathRelativeToProject(RelativePath, ProjectDirectory))
			{
				RelativePath = FilePath;
			}
			FPaths::NormalizeFilename(RelativePath);
			const FMD5Hash FileHash = FMD5Hash::HashFile(*FilePath);
			if (!FileHash.IsValid())
			{
				OutError = FString::Printf(
					TEXT("Failed to hash localization artifact '%s'"),
					*FilePath);
				return false;
			}
			OutHashes.Add(RelativePath, BytesToHex(FileHash.GetBytes(), 16));
		}
		return true;
	}

	void AppendLocalizationProcessOutput(
		void* ReadPipe,
		const FString& OutputLogPath,
		FString& InOutTail,
		FString& InOutLogError)
	{
		const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
		if (Chunk.IsEmpty())
		{
			return;
		}

		InOutTail += Chunk;
		constexpr int32 MaxTailCharacters = 32768;
		if (InOutTail.Len() > MaxTailCharacters)
		{
			InOutTail.RightInline(MaxTailCharacters);
		}

		if (InOutLogError.IsEmpty() &&
			!FFileHelper::SaveStringToFile(
				Chunk,
				*OutputLogPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(),
				FILEWRITE_Append))
		{
			InOutLogError = FString::Printf(TEXT("Failed to append process output log '%s'"), *OutputLogPath);
		}
	}

	FLocalizationConfigRunResult RunLocalizationConfig(
		const FLocalizationPipelinePlan& Plan,
		const FString& JobId,
		const FString& Operation,
		const FString& ConfigPath)
	{
		FLocalizationConfigRunResult Result;
		Result.Operation = Operation;
		Result.ConfigPath = ConfigPath;

		const FString JobLogDirectory = FPaths::Combine(Plan.LogDirectory, JobId);
		if (!IFileManager::Get().MakeDirectory(*JobLogDirectory, true))
		{
			Result.Error = FString::Printf(
				TEXT("Failed to create localization job log directory '%s'"),
				*JobLogDirectory);
			return Result;
		}
		Result.LogPath = FPaths::Combine(
			JobLogDirectory,
			FString::Printf(TEXT("%s.log"), *Operation));
		IFileManager::Get().Delete(*Result.LogPath, false, true, true);

		FString RelativeConfigPath = ConfigPath;
		if (!MakePathRelativeToProject(RelativeConfigPath, Plan.ProjectDirectory))
		{
			Result.Error = FString::Printf(
				TEXT("Failed to make config path project-relative: %s"),
				*ConfigPath);
			return Result;
		}
		FPaths::NormalizeFilename(RelativeConfigPath);

		const FString AdditionalArguments = FString::Printf(
			TEXT("-config=\"%s\" -Unattended -nop4 -stdout -FullStdOutLogOutput -UTF8Output"),
			*RelativeConfigPath);
		const FString QuotedProjectFile = FString::Printf(TEXT("\"%s\""), *Plan.ProjectFilePath);
		Result.ProcessArguments = CommandletHelpers::BuildCommandletProcessArguments(
			TEXT("GatherText"),
			*QuotedProjectFile,
			*AdditionalArguments);

		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
		{
			Result.Error = TEXT("Failed to create localization commandlet output pipe");
			return Result;
		}

		FProcHandle ProcessHandle = FPlatformProcess::CreateProc(
			*Plan.CommandletExecutable,
			*Result.ProcessArguments,
			false,
			true,
			true,
			nullptr,
			0,
			*Plan.ProjectDirectory,
			WritePipe);
		if (!ProcessHandle.IsValid())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			Result.Error = FString::Printf(
				TEXT("Failed to launch localization %s commandlet using '%s'"),
				*Operation,
				*Plan.CommandletExecutable);
			return Result;
		}

		Result.bLaunched = true;
		const double StartSeconds = FPlatformTime::Seconds();
		FString LogError;
		while (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			AppendLocalizationProcessOutput(ReadPipe, Result.LogPath, Result.OutputTail, LogError);

			if (FMonolithAsyncJobRegistry::Get().IsCancelRequested(JobId))
			{
				Result.bCancelled = true;
				FPlatformProcess::TerminateProc(ProcessHandle, true);
				break;
			}
			if ((FPlatformTime::Seconds() - StartSeconds) > static_cast<double>(Plan.TimeoutSeconds))
			{
				Result.bTimedOut = true;
				FPlatformProcess::TerminateProc(ProcessHandle, true);
				break;
			}
			FPlatformProcess::Sleep(0.05f);
		}

		FPlatformProcess::WaitForProc(ProcessHandle);
		AppendLocalizationProcessOutput(ReadPipe, Result.LogPath, Result.OutputTail, LogError);
		Result.DurationSeconds = FPlatformTime::Seconds() - StartSeconds;

		if (!Result.bCancelled && !Result.bTimedOut &&
			!FPlatformProcess::GetProcReturnCode(ProcessHandle, &Result.ExitCode))
		{
			Result.Error = TEXT("Localization commandlet exited without a readable return code");
		}
		else if (Result.bCancelled)
		{
			Result.Error = FString::Printf(TEXT("Localization %s operation was cancelled"), *Operation);
		}
		else if (Result.bTimedOut)
		{
			Result.Error = FString::Printf(
				TEXT("Localization %s operation exceeded timeout_seconds=%d"),
				*Operation,
				Plan.TimeoutSeconds);
		}
		else if (Result.ExitCode != 0)
		{
			Result.Error = FString::Printf(
				TEXT("Localization %s commandlet exited with code %d"),
				*Operation,
				Result.ExitCode);
		}

		if (!LogError.IsEmpty())
		{
			if (Result.Error.IsEmpty())
			{
				Result.Error = LogError;
			}
			else
			{
				Result.Error += TEXT("; ");
				Result.Error += LogError;
			}
		}

		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		FPlatformProcess::CloseProc(ProcessHandle);
		return Result;
	}

	TSharedPtr<FJsonObject> LocalizationConfigRunResultToJson(
		const FLocalizationConfigRunResult& RunResult)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("operation"), RunResult.Operation);
		Result->SetStringField(TEXT("config_path"), RunResult.ConfigPath);
		Result->SetStringField(TEXT("log_path"), RunResult.LogPath);
		Result->SetBoolField(TEXT("launched"), RunResult.bLaunched);
		Result->SetBoolField(TEXT("cancelled"), RunResult.bCancelled);
		Result->SetBoolField(TEXT("timed_out"), RunResult.bTimedOut);
		Result->SetNumberField(TEXT("exit_code"), RunResult.ExitCode);
		Result->SetNumberField(TEXT("duration_seconds"), RunResult.DurationSeconds);
		Result->SetStringField(TEXT("output_tail"), RunResult.OutputTail);
		if (!RunResult.Error.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), RunResult.Error);
		}
		return Result;
	}

	void ExecuteLocalizationPipelineAsync(
		FLocalizationPipelinePlan Plan,
		const FString JobId)
	{
		ON_SCOPE_EXIT
		{
			FScopeLock Lock(&GLocalizationPipelineStateLock);
			if (GActiveLocalizationPipelineJobId == JobId)
			{
				GActiveLocalizationPipelineJobId.Reset();
			}
		};

		FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
		JobRegistry.UpdateProgress(
			JobId,
			0.0,
			TEXT("preflight_complete"),
			TEXT("Localization target configs and writable outputs passed preflight."));

		TMap<FString, FString> BeforeHashes;
		FString HashError;
		if (!CaptureLocalizationArtifactHashes(
				Plan.ContentDirectory,
				Plan.ProjectDirectory,
				BeforeHashes,
				HashError))
		{
			JobRegistry.FailJob(JobId, HashError);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> StepResults;
		const int32 OperationCount = Plan.Operations.Num();
		for (int32 OperationIndex = 0; OperationIndex < OperationCount; ++OperationIndex)
		{
			if (JobRegistry.IsCancelRequested(JobId))
			{
				JobRegistry.CancelJob(JobId, TEXT("Cancellation acknowledged before the next localization operation."));
				return;
			}

			const FString& Operation = Plan.Operations[OperationIndex];
			const double StartPercent = 5.0 + (85.0 * static_cast<double>(OperationIndex) / OperationCount);
			JobRegistry.UpdateProgress(
				JobId,
				StartPercent,
				Operation,
				FString::Printf(TEXT("Running localization %s commandlet."), *Operation));

			FLocalizationConfigRunResult RunResult = RunLocalizationConfig(
				Plan,
				JobId,
				Operation,
				Plan.ConfigPaths[OperationIndex]);
			StepResults.Add(MakeShared<FJsonValueObject>(LocalizationConfigRunResultToJson(RunResult)));
			if (RunResult.bCancelled)
			{
				JobRegistry.CancelJob(JobId, RunResult.Error);
				return;
			}
			if (!RunResult.Error.IsEmpty())
			{
				const FString FailureTail = RunResult.OutputTail.Right(1024);
				JobRegistry.FailJob(
					JobId,
					FString::Printf(
						TEXT("%s; log=%s; output_tail=%s"),
						*RunResult.Error,
						*RunResult.LogPath,
						*FailureTail));
				return;
			}
		}

		JobRegistry.UpdateProgress(
			JobId,
			95.0,
			TEXT("artifact_audit"),
			TEXT("Hashing localization target outputs after gather/compile."));

		TMap<FString, FString> AfterHashes;
		if (!CaptureLocalizationArtifactHashes(
				Plan.ContentDirectory,
				Plan.ProjectDirectory,
				AfterHashes,
				HashError))
		{
			JobRegistry.FailJob(JobId, HashError);
			return;
		}
		TArray<FString> CreatedFiles;
		TArray<FString> UpdatedFiles;
		TArray<FString> DeletedFiles;
		for (const TPair<FString, FString>& Pair : AfterHashes)
		{
			const FString* BeforeHash = BeforeHashes.Find(Pair.Key);
			if (!BeforeHash)
			{
				CreatedFiles.Add(Pair.Key);
			}
			else if (*BeforeHash != Pair.Value)
			{
				UpdatedFiles.Add(Pair.Key);
			}
		}
		for (const TPair<FString, FString>& Pair : BeforeHashes)
		{
			if (!AfterHashes.Contains(Pair.Key))
			{
				DeletedFiles.Add(Pair.Key);
			}
		}
		CreatedFiles.Sort();
		UpdatedFiles.Sort();
		DeletedFiles.Sort();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("completed"));
		Result->SetStringField(TEXT("target"), Plan.TargetName);
		Result->SetArrayField(TEXT("operations"), StringsToJson(Plan.Operations));
		Result->SetArrayField(TEXT("steps"), StepResults);
		Result->SetStringField(TEXT("content_directory"), Plan.ContentDirectory);
		Result->SetStringField(TEXT("log_directory"), FPaths::Combine(Plan.LogDirectory, JobId));
		Result->SetBoolField(
			TEXT("changed"),
			!CreatedFiles.IsEmpty() || !UpdatedFiles.IsEmpty() || !DeletedFiles.IsEmpty());
		Result->SetBoolField(TEXT("source_control_enabled_for_child"), false);
		Result->SetNumberField(TEXT("before_file_count"), BeforeHashes.Num());
		Result->SetNumberField(TEXT("after_file_count"), AfterHashes.Num());
		Result->SetArrayField(TEXT("created_files"), StringsToJson(CreatedFiles));
		Result->SetArrayField(TEXT("updated_files"), StringsToJson(UpdatedFiles));
		Result->SetArrayField(TEXT("deleted_files"), StringsToJson(DeletedFiles));
		JobRegistry.CompleteJob(JobId, Result);
	}
}

void FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		GLocalizationPipelineShuttingDown = false;
		GLocalizationTargetConfigurationActive = false;
	}

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

	Registry.RegisterAction(TEXT("localization"), TEXT("set_target_text_search_directories"),
		TEXT("Set one project localization target's Gather Text From Source search directories through "
			 "the runtime-loaded Localization Dashboard model, persist DefaultEditor.ini, and patch only "
			 "the matching GatherTextFromSource rows in the existing gather config. Entries must use "
			 "explicit %LOCENGINEROOT% or %LOCPROJECTROOT% tokens. "
			 "Requires source_control_policy=require_checked_out plus an exact positive target_changelist; "
			 "dry_run ForceUpdates structured readiness without mutation and confirm revalidates before writing."),
		FMonolithActionHandler::CreateStatic(
			&FMonolithLocalizationActions::SetTargetTextSearchDirectories),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("target"), TEXT("string"), TEXT("Project localization target name"))
			.Required(
				TEXT("search_directories"),
				TEXT("array"),
				TEXT("One to 64 existing source directories using %LOCENGINEROOT% or %LOCPROJECTROOT% prefixes"))
			.Required(
				TEXT("source_control_policy"),
				TEXT("string"),
				TEXT("Must be require_checked_out; prepare every changed file in the intended numbered changelist before execution"))
			.Enum(
				TEXT("source_control_policy"),
				{TEXT("require_checked_out")})
			.Required(
				TEXT("target_changelist"),
				TEXT("integer"),
				TEXT("Exact positive numbered changelist that must already own every file the action would write; default is forbidden"))
			.Range(
				TEXT("target_changelist"),
				1.0,
				static_cast<double>(MAX_int32))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("ForceUpdate source-control state and preview model/config changes plus structured blockers without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true to persist the target model and targeted gather-config patch"), TEXT("false"))
			.Build(),
		FString(),
		LocalizationTargetConfigurationExecutionPolicy());

	Registry.RegisterAction(TEXT("localization"), TEXT("run_target_pipeline"),
		TEXT("Asynchronously gather and/or compile one project localization target from its canonical "
			 "Config/Localization/<Target>_{Gather,Compile}.ini files. The child commandlet never enables "
			 "source control; existing generated outputs must already be writable. Requires dry_run=true "
			 "for preflight or confirm=true to start."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::RunTargetPipeline),
		FParamSchemaBuilder()
			.Required(TEXT("target"), TEXT("string"), TEXT("Localization target name; ASCII letters, digits, underscores, and hyphens only"))
			.Optional(TEXT("operations"), TEXT("array"), TEXT("Ordered subset of gather and compile"), TEXT("[\"gather\",\"compile\"]"))
			.Optional(TEXT("timeout_seconds"), TEXT("integer"), TEXT("Per-operation timeout in seconds (30-3600)"), TEXT("900"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate configs and report output checkout requirements without launching a child process"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true to launch the asynchronous pipeline"), TEXT("false"))
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

	Registry.SetActionAnnotations(
		TEXT("localization"), TEXT("set_target_text_search_directories"),
		false, true, false, TEXT("Set localization target source directories"));
	Registry.SetActionSearchMetadata(
		TEXT("localization"), TEXT("set_target_text_search_directories"),
		{
			TEXT("configure localization target"),
			TEXT("set gather text source directories"),
			TEXT("edit localization dashboard target"),
			TEXT("change localization gather paths")
		},
		{TEXT("configure_target"), TEXT("set_gather_paths")},
		{TEXT("limit EngineOverrides gathering to one engine source directory")});
	Registry.SetActionPlanningMetadata(
		TEXT("localization"), TEXT("set_target_text_search_directories"),
		TEXT("unreal-localization"),
		{
			TEXT("Every search directory uses an explicit localization root token and exists"),
			TEXT("Run dry_run first and open every reported file in target_changelist under source_control_policy=require_checked_out before confirm"),
			TEXT("No localization.run_target_pipeline job is active")
		},
		{
			TEXT("DefaultEditor.ini persists the target model"),
			TEXT("the existing GatherTextFromSource config keeps its non-directory text/line terminators and matches exact directory readback"),
			TEXT("source_control_prepare and source_control_after prove exact numbered-changelist ownership with per-file actual_changelist")
		},
		{TEXT("source_control.get_status"), TEXT("localization.run_target_pipeline")});

	Registry.SetActionAnnotations(
		TEXT("localization"), TEXT("run_target_pipeline"),
		false, true, false, TEXT("Run localization target pipeline"));
	Registry.SetActionSearchMetadata(
		TEXT("localization"), TEXT("run_target_pipeline"),
		{
			TEXT("gather localization target"),
			TEXT("compile locres"),
			TEXT("refresh localization resource"),
			TEXT("run gather text commandlet"),
			TEXT("regenerate localization target")
		},
		{TEXT("gather_target"), TEXT("compile_target"), TEXT("refresh_locres")},
		{TEXT("gather and compile the EngineOverrides localization target")});
	Registry.SetActionPlanningMetadata(
		TEXT("localization"), TEXT("run_target_pipeline"),
		TEXT("unreal-localization"),
		{
			TEXT("Config/Localization/<Target>_<Operation>.ini exists and keeps SourcePath/DestinationPath under Content/Localization/<Target>"),
			TEXT("Run dry_run first and check out every reported read_only_output_file before confirm")
		},
		{
			TEXT("asynchronous job_id with monolith.get_job polling"),
			TEXT("per-operation logs and a created/updated/deleted artifact audit")
		},
		{TEXT("monolith.get_job"), TEXT("monolith.cancel_job"), TEXT("source_control.get_status")});
}

void FMonolithLocalizationActions::ShutdownActions()
{
	FString ActiveJobId;
	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		GLocalizationPipelineShuttingDown = true;
		ActiveJobId = GActiveLocalizationPipelineJobId;
	}

	if (!ActiveJobId.IsEmpty())
	{
		FMonolithAsyncJobRegistry::Get().RequestCancel(ActiveJobId);
	}
	if (GLocalizationPipelineFuture.IsValid())
	{
		// The worker acknowledges cancellation, terminates only its owned child process, and then
		// releases the module's static state. Joining here prevents code from executing after unload.
		GLocalizationPipelineFuture.Wait();
	}
}

FMonolithActionResult FMonolithLocalizationActions::SetTargetTextSearchDirectories(
	const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString TargetName;
	if (!ReadRequiredStringParam(Params, TEXT("target"), TargetName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	TargetName = TargetName.TrimStartAndEnd();
	if (!IsValidLocalizationTargetName(TargetName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FGatherTextSearchDirectory> DesiredDirectories;
	TArray<FString> DesiredDirectoryStrings;
	if (!ReadLocalizationGatherDirectories(
			Params,
			DesiredDirectories,
			DesiredDirectoryStrings,
			Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	int32 TargetChangelist = 0;
	if (!ReadLocalizationSourceControlContract(
			Params,
			TargetChangelist,
			Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		if (GLocalizationPipelineShuttingDown)
		{
			return FMonolithActionResult::Error(
				TEXT("MonolithConfig is shutting down; localization target configuration is unavailable"));
		}
		if (!GActiveLocalizationPipelineJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("active_job_id"), GActiveLocalizationPipelineJobId);
			ErrorData->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));
			FMonolithActionResult Result = FMonolithActionResult::Error(
				TEXT("Cannot configure a localization target while localization.run_target_pipeline is active"));
			Result.WithErrorData(ErrorData).WithRelatedAction(TEXT("monolith.get_job"));
			return Result;
		}
		if (GLocalizationTargetConfigurationActive)
		{
			return FMonolithActionResult::Error(
				TEXT("Another localization target configuration mutation is already active"));
		}
	}

	const FString SettingsFile = GetLocalizationSettingsFile();
	TArray<FString> AvailableTargets;
	ULocalizationTarget* Target = nullptr;
	if (!FindProjectLocalizationTarget(
			TargetName,
			SettingsFile,
			Target,
			AvailableTargets,
			Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (!Target)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("target"), TargetName);
		ErrorData->SetArrayField(TEXT("available_project_targets"), StringsToJson(AvailableTargets));
		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Project localization target '%s' was not found in the Localization Dashboard model"),
				*TargetName));
		Result.WithErrorData(ErrorData);
		return Result;
	}

	FLocalizationTargetConfigPreview Preview;
	if (!BuildLocalizationTargetConfigPreview(
			Target,
			DesiredDirectories,
			TargetChangelist,
			Preview,
			Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TArray<FString> ExistingDirectoryStrings =
		LocalizationGatherDirectoriesToStrings(
			Target->Settings.GatherFromTextFiles.SearchDirectories);
	const bool bWouldChange =
		Preview.bSettingsChanged || !Preview.ChangedConfigFiles.IsEmpty();

	TSharedPtr<FJsonObject> Preflight = MakeShared<FJsonObject>();
	Preflight->SetStringField(
		TEXT("action"),
		TEXT("localization.set_target_text_search_directories"));
	Preflight->SetStringField(TEXT("target"), Target->Settings.Name);
	Preflight->SetStringField(TEXT("target_set"), TEXT("project"));
	Preflight->SetArrayField(
		TEXT("search_directories_before"),
		StringsToJson(ExistingDirectoryStrings));
	Preflight->SetArrayField(
		TEXT("search_directories_after"),
		StringsToJson(DesiredDirectoryStrings));
	Preflight->SetBoolField(TEXT("settings_would_change"), Preview.bSettingsChanged);
	Preflight->SetStringField(TEXT("settings_file"), Preview.SettingsFile);
	Preflight->SetArrayField(
		TEXT("config_files_would_change"),
		StringsToJson(Preview.ChangedConfigFiles));
	Preflight->SetArrayField(
		TEXT("missing_files"),
		StringsToJson(Preview.MissingFiles));
	Preflight->SetArrayField(
		TEXT("read_only_files"),
		StringsToJson(Preview.ReadOnlyFiles));
	Preflight->SetNumberField(TEXT("missing_file_count"), Preview.MissingFiles.Num());
	Preflight->SetNumberField(TEXT("read_only_file_count"), Preview.ReadOnlyFiles.Num());
	Preflight->SetBoolField(TEXT("would_change"), bWouldChange);
	Preflight->SetStringField(
		TEXT("source_control_policy"),
		TEXT("require_checked_out"));
	Preflight->SetNumberField(
		TEXT("expected_target_changelist"),
		TargetChangelist);
	Preflight->SetObjectField(
		TEXT("source_control_before"),
		LocalizationSourceControlAuditToJson(Preview));
	Preflight->SetBoolField(
		TEXT("source_control_ready"),
		Preview.bSourceControlReady);
	Preflight->SetNumberField(
		TEXT("source_control_blocker_count"),
		Preview.SourceControlBlockers.Num());
	Preflight->SetArrayField(
		TEXT("source_control_blockers"),
		StringsToJson(Preview.SourceControlBlockers));
	Preflight->SetArrayField(
		TEXT("source_control_files"),
		LocalizationSourceControlFilesToJson(
			Preview.SourceControlFiles));
	Preflight->SetBoolField(
		TEXT("ready"),
		Preview.MissingFiles.IsEmpty() &&
			Preview.ReadOnlyFiles.IsEmpty() &&
			Preview.bSourceControlReady);
	Preflight->SetBoolField(TEXT("changed"), false);
	Preflight->SetBoolField(TEXT("targeted_gather_config_patch"), true);
	Preflight->SetBoolField(TEXT("non_directory_config_text_preserved"), true);
	const FString SourceControlPrepareStatus = !bWouldChange
		? TEXT("not_required_no_change")
		: (Preview.bSourceControlReady
			? TEXT("validated_exact_numbered_changelist")
			: TEXT("failed"));
	const TSharedPtr<FJsonObject> SourceControlPrepare =
		BuildLocalizationHandlerOwnedSourceControlPrepare(
			Preview,
			SourceControlPrepareStatus);
	Preflight->SetObjectField(
		TEXT("source_control_prepare"),
		SourceControlPrepare);

	if (Options.bDryRun)
	{
		Preflight->SetStringField(TEXT("status"), TEXT("planned"));
		return FMonolithActionResult::Success(Preflight);
	}

	if (!Preview.MissingFiles.IsEmpty())
	{
		Preflight->SetStringField(TEXT("status"), TEXT("blocked"));
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("preflight"), Preflight);
		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
					TEXT("Localization target '%s' has %d missing settings/gather-config file(s); refusing to create unowned source-control files"),
				*Target->Settings.Name,
				Preview.MissingFiles.Num()));
		Result.WithErrorData(ErrorData);
		return Result;
	}

	if (!Preview.ReadOnlyFiles.IsEmpty())
	{
		Preflight->SetStringField(TEXT("status"), TEXT("blocked"));
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("preflight"), Preflight);
		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Localization target '%s' has %d read-only settings/config file(s); check them out before confirm"),
				*Target->Settings.Name,
				Preview.ReadOnlyFiles.Num()));
		Result.WithErrorData(ErrorData)
			.WithRelatedAction(TEXT("source_control.get_status"));
		return Result;
	}

	if (bWouldChange && !Preview.bSourceControlReady)
	{
		Preflight->SetStringField(TEXT("status"), TEXT("blocked"));
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("preflight"), Preflight);
		ErrorData->SetObjectField(
			TEXT("source_control_prepare"),
			SourceControlPrepare);
		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Localization target '%s' source-control preflight has %d blocker(s); every write file must be a current, non-conflicted existing edit owned by this client in exact numbered changelist %d"),
				*Target->Settings.Name,
				Preview.SourceControlBlockers.Num(),
				TargetChangelist));
		Result.WithErrorData(ErrorData)
			.WithRelatedAction(TEXT("source_control.get_status"));
		return Result;
	}

	if (!bWouldChange)
	{
		Preflight->SetStringField(TEXT("status"), TEXT("unchanged"));
		return FMonolithActionResult::Success(Preflight);
	}

	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		if (GLocalizationPipelineShuttingDown)
		{
			return FMonolithActionResult::Error(
				TEXT("MonolithConfig is shutting down; localization target configuration is unavailable"));
		}
		if (!GActiveLocalizationPipelineJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("active_job_id"), GActiveLocalizationPipelineJobId);
			ErrorData->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));
			FMonolithActionResult Result = FMonolithActionResult::Error(
				TEXT("A localization target pipeline started after preflight; retry configuration after it completes"));
			Result.WithErrorData(ErrorData).WithRelatedAction(TEXT("monolith.get_job"));
			return Result;
		}
		if (GLocalizationTargetConfigurationActive)
		{
			return FMonolithActionResult::Error(
				TEXT("Another localization target configuration mutation started after preflight"));
		}
		GLocalizationTargetConfigurationActive = true;
	}
	ON_SCOPE_EXIT
	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		GLocalizationTargetConfigurationActive = false;
	};

	TArray<FString> SnapshotPaths = Preview.ChangedConfigFiles;
	if (Preview.bSettingsChanged)
	{
		SnapshotPaths.AddUnique(Preview.SettingsFile);
	}
	SnapshotPaths.Sort();

	FLocalizationTargetConfigPreview FreshPreWriteSourceControl;
	AuditLocalizationSourceControlOwnership(
		SnapshotPaths,
		TargetChangelist,
		FreshPreWriteSourceControl);
	const TSharedPtr<FJsonObject> FreshPreWriteSourceControlJson =
		LocalizationSourceControlAuditToJson(
			FreshPreWriteSourceControl);
	Preflight->SetObjectField(
		TEXT("source_control_pre_write"),
		FreshPreWriteSourceControlJson);
	const TSharedPtr<FJsonObject> FreshSourceControlPrepare =
		BuildLocalizationHandlerOwnedSourceControlPrepare(
			FreshPreWriteSourceControl,
			FreshPreWriteSourceControl.bSourceControlReady
				? TEXT("validated_exact_numbered_changelist")
				: TEXT("failed"));
	Preflight->SetObjectField(
		TEXT("source_control_prepare"),
		FreshSourceControlPrepare);
	if (!FreshPreWriteSourceControl.bSourceControlReady)
	{
		Preflight->SetStringField(TEXT("status"), TEXT("blocked"));
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("preflight"), Preflight);
		ErrorData->SetObjectField(
			TEXT("source_control_prepare"),
			FreshSourceControlPrepare);
		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Localization target '%s' source-control ownership changed before mutation; fresh ForceUpdate reported %d blocker(s), so no snapshot or write was attempted"),
				*Target->Settings.Name,
				FreshPreWriteSourceControl.SourceControlBlockers.Num()));
		Result.WithErrorData(ErrorData)
			.WithRelatedAction(TEXT("source_control.get_status"));
		return Result;
	}

	TArray<FLocalizationFileSnapshot> Snapshots;
	if (!CaptureLocalizationFileSnapshots(SnapshotPaths, Snapshots, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TArray<FGatherTextSearchDirectory> PreviousDirectories =
		Target->Settings.GatherFromTextFiles.SearchDirectories;
	FString MutationError;
	TSharedPtr<FJsonObject> SourceControlAfter;
	if (Preview.bSettingsChanged)
	{
		Target->Modify();
		Target->Settings.GatherFromTextFiles.SearchDirectories = DesiredDirectories;
		Target->PostEditChange();
		if (!VerifyPersistedLocalizationTargetDirectories(
				Preview.SettingsFile,
				Target->Settings.Name,
				DesiredDirectories,
				MutationError))
		{
			// Failure is handled by the common rollback below.
		}
	}

	if (MutationError.IsEmpty() &&
		!WriteLocalizationTargetConfigs(Preview, MutationError))
	{
		// Failure is handled by the common rollback below.
	}
	if (MutationError.IsEmpty() &&
		!VerifyLocalizationTargetConfigContents(Preview, MutationError))
	{
		// Failure is handled by the common rollback below.
	}
	if (MutationError.IsEmpty())
	{
		FLocalizationTargetConfigPreview PostMutationSourceControl;
		AuditLocalizationSourceControlOwnership(
			SnapshotPaths,
			TargetChangelist,
			PostMutationSourceControl);
		SourceControlAfter =
			LocalizationSourceControlAuditToJson(
				PostMutationSourceControl);
		if (!PostMutationSourceControl.bSourceControlReady)
		{
			MutationError = FString::Printf(
				TEXT("Source-control ownership changed during mutation; %d blocker(s) were reported after ForceUpdate"),
				PostMutationSourceControl.SourceControlBlockers.Num());
		}
	}

	if (!MutationError.IsEmpty())
	{
		if (Preview.bSettingsChanged)
		{
			Target->Settings.GatherFromTextFiles.SearchDirectories = PreviousDirectories;
			Target->PostEditChange();
		}

		FString RollbackError;
		const bool bRollbackSucceeded =
			RestoreLocalizationFileSnapshots(Snapshots, RollbackError);
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("preflight"), Preflight);
		ErrorData->SetStringField(TEXT("mutation_error"), MutationError);
		ErrorData->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
		if (SourceControlAfter.IsValid())
		{
			ErrorData->SetObjectField(
				TEXT("source_control_after"),
				SourceControlAfter);
		}
		if (!RollbackError.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("rollback_error"), RollbackError);
		}

		FMonolithActionResult Result = FMonolithActionResult::Error(
			bRollbackSucceeded
				? TEXT("Localization target configuration failed; model and files were rolled back")
				: TEXT("Localization target configuration failed and rollback was incomplete"));
		Result.WithErrorData(ErrorData);
		return Result;
	}

	Preflight->SetStringField(TEXT("status"), TEXT("completed"));
	Preflight->SetBoolField(TEXT("changed"), true);
	Preflight->SetBoolField(TEXT("settings_changed"), Preview.bSettingsChanged);
	Preflight->SetArrayField(
		TEXT("config_files_changed"),
		StringsToJson(Preview.ChangedConfigFiles));
	Preflight->SetBoolField(TEXT("settings_readback_verified"), true);
	Preflight->SetBoolField(TEXT("config_readback_verified"), true);
	Preflight->SetBoolField(TEXT("source_control_readback_verified"), true);
	Preflight->SetObjectField(
		TEXT("source_control_after"),
		SourceControlAfter);
	return FMonolithActionResult::Success(Preflight);
}

FMonolithActionResult FMonolithLocalizationActions::RunTargetPipeline(const TSharedPtr<FJsonObject>& Params)
{
	FLocalizationMutationOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		if (GLocalizationPipelineShuttingDown)
		{
			return FMonolithActionResult::Error(
				TEXT("MonolithConfig is shutting down; localization pipeline launch is unavailable"));
		}
	}

	FLocalizationPipelinePlan Plan;
	if (!BuildLocalizationPipelinePlan(Params, Plan, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Preflight = MakeShared<FJsonObject>();
	Preflight->SetStringField(TEXT("action"), TEXT("localization.run_target_pipeline"));
	Preflight->SetStringField(TEXT("status"), TEXT("planned"));
	Preflight->SetStringField(TEXT("target"), Plan.TargetName);
	Preflight->SetArrayField(TEXT("operations"), StringsToJson(Plan.Operations));
	Preflight->SetArrayField(TEXT("config_paths"), StringsToJson(Plan.ConfigPaths));
	Preflight->SetStringField(TEXT("content_directory"), Plan.ContentDirectory);
	Preflight->SetStringField(TEXT("commandlet_executable"), Plan.CommandletExecutable);
	Preflight->SetStringField(TEXT("log_root"), Plan.LogDirectory);
	Preflight->SetNumberField(TEXT("timeout_seconds"), Plan.TimeoutSeconds);
	Preflight->SetArrayField(TEXT("existing_output_files"), StringsToJson(Plan.ExistingOutputFiles));
	Preflight->SetArrayField(TEXT("read_only_output_files"), StringsToJson(Plan.ReadOnlyOutputFiles));
	Preflight->SetNumberField(TEXT("existing_output_count"), Plan.ExistingOutputFiles.Num());
	Preflight->SetNumberField(TEXT("read_only_output_count"), Plan.ReadOnlyOutputFiles.Num());
	Preflight->SetBoolField(TEXT("ready"), Plan.ReadOnlyOutputFiles.IsEmpty());
	Preflight->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Preflight->SetBoolField(TEXT("changed"), false);
	Preflight->SetBoolField(TEXT("source_control_enabled_for_child"), false);

	if (Options.bDryRun)
	{
		return FMonolithActionResult::Success(Preflight);
	}

	if (!Plan.ReadOnlyOutputFiles.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("target"), Plan.TargetName);
		ErrorData->SetArrayField(TEXT("read_only_output_files"), StringsToJson(Plan.ReadOnlyOutputFiles));
		ErrorData->SetNumberField(TEXT("read_only_output_count"), Plan.ReadOnlyOutputFiles.Num());
		ErrorData->SetStringField(
			TEXT("required_action"),
			TEXT("Check out the listed generated files in the intended changelist, then rerun with confirm=true."));

		FMonolithActionResult Result = FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Localization target '%s' has %d read-only generated output file(s); refusing to launch a child commandlet"),
				*Plan.TargetName,
				Plan.ReadOnlyOutputFiles.Num()));
		Result.WithErrorData(ErrorData).WithRelatedAction(TEXT("source_control.checkout"));
		return Result;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bEnableAsyncJobs)
	{
		return FMonolithActionResult::Error(
			TEXT("Localization target pipeline requires Monolith async jobs; enable bEnableAsyncJobs and retry"));
	}

	FString JobId;
	{
		FScopeLock Lock(&GLocalizationPipelineStateLock);
		if (GLocalizationPipelineShuttingDown)
		{
			return FMonolithActionResult::Error(
				TEXT("MonolithConfig is shutting down; localization pipeline launch is unavailable"));
		}
		if (!GActiveLocalizationPipelineJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("active_job_id"), GActiveLocalizationPipelineJobId);
			ErrorData->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));
			FMonolithActionResult Result = FMonolithActionResult::Error(
				TEXT("Another localization target pipeline is already active"));
			Result.WithErrorData(ErrorData).WithRelatedAction(TEXT("monolith.get_job"));
			return Result;
		}
		if (GLocalizationTargetConfigurationActive)
		{
			return FMonolithActionResult::Error(
				TEXT("A localization target configuration mutation is active; retry the pipeline after it completes"));
		}

		FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
		JobId = JobRegistry.SubmitJob(
			TEXT("localization"),
			TEXT("run_target_pipeline"),
			/*bCancellable=*/true,
			/*bSupportsProgress=*/true,
			TEXT("monolith.get_job"),
			TEXT("monolith.cancel_job"));
		JobRegistry.UpdateProgress(
			JobId,
			0.0,
			TEXT("queued"),
			TEXT("Localization target pipeline queued on the worker thread."));
		GActiveLocalizationPipelineJobId = JobId;
		GLocalizationPipelineFuture = Async(
			EAsyncExecution::ThreadPool,
			[Plan = MoveTemp(Plan), JobId]() mutable
			{
				ExecuteLocalizationPipelineAsync(MoveTemp(Plan), JobId);
			});
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), TEXT("localization.run_target_pipeline"));
	Result->SetStringField(TEXT("status"), TEXT("started"));
	Result->SetStringField(TEXT("target"), Preflight->GetStringField(TEXT("target")));
	Result->SetArrayField(TEXT("operations"), Preflight->GetArrayField(TEXT("operations")));
	Result->SetStringField(TEXT("job_id"), JobId);
	Result->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));
	Result->SetStringField(TEXT("cancel_action"), TEXT("monolith.cancel_job"));
	Result->SetBoolField(TEXT("supports_progress"), true);
	Result->SetBoolField(TEXT("cancellable"), true);
	Result->SetBoolField(TEXT("source_control_enabled_for_child"), false);
	Result->SetStringField(TEXT("log_directory"), FPaths::Combine(Preflight->GetStringField(TEXT("log_root")), JobId));
	Result->SetBoolField(TEXT("changed"), false);
	return FMonolithActionResult::Success(Result);
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
	MonolithStringTableCompat::SetSourceString(MutableTable, TextKey, SourceString);
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
			MonolithStringTableCompat::SetSourceString(MutableTable, TextKey, Row.SourceString, PreservedDevNotes.Find(Row.Key));
#else
			MonolithStringTableCompat::SetSourceString(MutableTable, TextKey, Row.SourceString);
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
				MonolithStringTableCompat::SetSourceString(
					RollbackTable,
					RollbackKey,
					RollbackRow.SourceString,
					RollbackDevNotes.Find(RollbackRow.Key));
#else
				MonolithStringTableCompat::SetSourceString(
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
