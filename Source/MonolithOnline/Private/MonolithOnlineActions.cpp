#include "MonolithOnlineActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/AssetManager.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace MonolithOnline
{
	static constexpr int32 ErrInvalidParams = -32602;

	struct FConfigFieldSpec
	{
		FString Section;
		FString Key;
		bool bSensitive = false;
		bool bExposeValue = false;
	};

	struct FConfigObservation
	{
		FString Source;
		FString FilePath;
		FString Section;
		FString Key;
		FString Value;
		bool bPresent = false;
		bool bEffective = false;
		bool bSensitive = false;
		bool bExposeValue = false;
	};

	struct FLobbySchemaInfo
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		TSet<FString> SeenAttributes;
		bool bHasGameLobbySchema = false;
	};

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(Value));
		}
		return Rows;
	}

	static FMonolithActionExecutionPolicy ExplicitReadOnlyPolicy()
	{
		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.bDefaulted = false;
		return Policy;
	}

	static TSharedPtr<FJsonObject> ErrorData(const FString& Field, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("field"), Field);
		Data->SetStringField(TEXT("detail"), Detail);
		return Data;
	}

	static TSharedPtr<FJsonObject> MakeCheck(const FString& Name, bool bOk, const FString& Severity, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("ok"), bOk);
		Check->SetStringField(TEXT("severity"), Severity);
		Check->SetStringField(TEXT("detail"), Detail);
		return Check;
	}

	static void AddCheck(
		TArray<TSharedPtr<FJsonValue>>& Checks,
		bool& bOverallOk,
		const FString& Name,
		bool bOk,
		const FString& Severity,
		const FString& Detail)
	{
		Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(Name, bOk, Severity, Detail)));
		if (!bOk && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			bOverallOk = false;
		}
	}

	static bool ReadOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetStringField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}

	static bool ReadRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		if (!ReadOptionalStringParam(Params, FieldName, OutValue, OutError))
		{
			return false;
		}
		if (OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double NumberValue = 0.0;
		if (!Params->TryGetNumberField(FieldName, NumberValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a number"), FieldName);
			return false;
		}
		InOutValue = static_cast<int32>(NumberValue);
		return true;
	}

	static bool TryReadPrimaryAssetIdProperty(UObject* Object, const FString& PropertyName, FPrimaryAssetId& OutValue)
	{
		OutValue = FPrimaryAssetId();
		if (!Object || !Object->GetClass())
		{
			return false;
		}
		FStructProperty* StructProperty = CastField<FStructProperty>(Object->GetClass()->FindPropertyByName(FName(*PropertyName)));
		if (!StructProperty || StructProperty->Struct != FPrimaryAssetId::StaticStruct())
		{
			return false;
		}
		const void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(Object);
		OutValue = *static_cast<const FPrimaryAssetId*>(ValuePtr);
		return true;
	}

	static TSharedPtr<FJsonObject> PrimaryAssetIdToJson(const FPrimaryAssetId& AssetId)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetBoolField(TEXT("valid"), AssetId.IsValid());
		Row->SetStringField(TEXT("value"), AssetId.ToString());
		Row->SetStringField(TEXT("type"), AssetId.PrimaryAssetType.ToString());
		Row->SetStringField(TEXT("name"), AssetId.PrimaryAssetName.ToString());
		FString ResolvedPath;
		if (AssetId.IsValid() && UAssetManager::IsInitialized())
		{
			ResolvedPath = UAssetManager::Get().GetPrimaryAssetPath(AssetId).ToString();
		}
		Row->SetBoolField(TEXT("asset_manager_resolved"), !ResolvedPath.IsEmpty());
		Row->SetStringField(TEXT("asset_manager_path"), ResolvedPath);
		return Row;
	}

	static bool TryReadBoolProperty(UObject* Object, const FString& PropertyName, bool& OutValue)
	{
		OutValue = false;
		if (!Object || !Object->GetClass())
		{
			return false;
		}
		FBoolProperty* BoolProperty = CastField<FBoolProperty>(Object->GetClass()->FindPropertyByName(FName(*PropertyName)));
		if (!BoolProperty)
		{
			return false;
		}
		const void* ValuePtr = BoolProperty->ContainerPtrToValuePtr<void>(Object);
		OutValue = BoolProperty->GetPropertyValue(ValuePtr);
		return true;
	}

	static bool TryReadIntProperty(UObject* Object, const FString& PropertyName, int64& OutValue)
	{
		OutValue = 0;
		if (!Object || !Object->GetClass())
		{
			return false;
		}
		FNumericProperty* NumericProperty = CastField<FNumericProperty>(Object->GetClass()->FindPropertyByName(FName(*PropertyName)));
		if (!NumericProperty || NumericProperty->IsFloatingPoint())
		{
			return false;
		}
		const void* ValuePtr = NumericProperty->ContainerPtrToValuePtr<void>(Object);
		OutValue = NumericProperty->GetSignedIntPropertyValue(ValuePtr);
		return true;
	}

	static FString NormalizeSessionModeToken(const FString& Exported)
	{
		if (Exported.Contains(TEXT("Offline"), ESearchCase::IgnoreCase))
		{
			return TEXT("Offline");
		}
		if (Exported.Contains(TEXT("LAN"), ESearchCase::IgnoreCase))
		{
			return TEXT("LAN");
		}
		if (Exported.Contains(TEXT("Online"), ESearchCase::IgnoreCase))
		{
			return TEXT("Online");
		}
		return Exported;
	}

	static FString NormalizeObjectPath(FString AssetPath)
	{
		AssetPath.TrimStartAndEndInline();
		AssetPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (AssetPath.IsEmpty() || AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return AssetPath;
		}
		return AssetPath + TEXT(".") + AssetName;
	}

	static FString ToProjectRelativePath(FString Path)
	{
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		FString Relative = Path;
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		ProjectDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (FPaths::MakePathRelativeTo(Relative, *ProjectDir))
		{
			Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
			return Relative;
		}
		return Path;
	}

	static void AddUniqueFile(TArray<FString>& Files, FString Path)
	{
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return;
		}
		Path = FPaths::ConvertRelativePathToFull(Path);
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		Files.AddUnique(Path);
	}

	static TArray<FString> GetConfigFilesToScan()
	{
		TArray<FString> Files;
		AddUniqueFile(Files, GEngineIni);
		AddUniqueFile(Files, GGameIni);
		AddUniqueFile(Files, FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini"));
		AddUniqueFile(Files, FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		AddUniqueFile(Files, FPaths::ProjectConfigDir() / TEXT("Custom/EOS/DefaultEngine.ini"));
		AddUniqueFile(Files, FPaths::ProjectConfigDir() / TEXT("Windows/Custom/EOS/WindowsEngine.ini"));
		AddUniqueFile(Files, FPaths::ProjectConfigDir() / TEXT("Windows/WindowsEngine.ini"));
		return Files;
	}

	static bool ParseIniLine(const FString& RawLine, FString& InOutSection, FString& OutKey, FString& OutValue)
	{
		OutKey.Reset();
		OutValue.Reset();

		FString Line = RawLine;
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
		{
			return false;
		}
		if (Line.StartsWith(TEXT("[")))
		{
			int32 ClosingBracket = INDEX_NONE;
			if (Line.FindChar(TEXT(']'), ClosingBracket) && ClosingBracket > 1)
			{
				InOutSection = Line.Mid(1, ClosingBracket - 1);
				InOutSection.TrimStartAndEndInline();
			}
			return false;
		}

		int32 EqualsIndex = INDEX_NONE;
		if (!Line.FindChar(TEXT('='), EqualsIndex) || EqualsIndex <= 0)
		{
			return false;
		}

		OutKey = Line.Left(EqualsIndex);
		OutKey.TrimStartAndEndInline();
		if (OutKey.StartsWith(TEXT("+")) || OutKey.StartsWith(TEXT("-")) || OutKey.StartsWith(TEXT("!")))
		{
			OutKey.RightChopInline(1);
			OutKey.TrimStartAndEndInline();
		}

		OutValue = Line.Mid(EqualsIndex + 1);
		OutValue.TrimStartAndEndInline();
		return !InOutSection.IsEmpty() && !OutKey.IsEmpty();
	}

	static bool ReadIniValueFromFile(const FString& FilePath, const FString& Section, const FString& Key, FString& OutValue)
	{
		OutValue.Reset();
		TArray<FString> Lines;
		if (!FPaths::FileExists(FilePath) || !FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			return false;
		}

		bool bFound = false;
		FString CurrentSection;
		for (const FString& RawLine : Lines)
		{
			FString ParsedKey;
			FString ParsedValue;
			if (ParseIniLine(RawLine, CurrentSection, ParsedKey, ParsedValue)
				&& CurrentSection.Equals(Section, ESearchCase::IgnoreCase)
				&& ParsedKey.Equals(Key, ESearchCase::IgnoreCase))
			{
				OutValue = ParsedValue;
				bFound = true;
			}
		}
		return bFound;
	}

	static void AddEffectiveObservation(const FConfigFieldSpec& Spec, const FString& ConfigFile, const FString& Source, TArray<FConfigObservation>& OutObservations)
	{
		FString Value;
		const bool bPresent = GConfig && GConfig->GetString(*Spec.Section, *Spec.Key, Value, ConfigFile);

		FConfigObservation Observation;
		Observation.Source = Source;
		Observation.FilePath = ConfigFile;
		Observation.Section = Spec.Section;
		Observation.Key = Spec.Key;
		Observation.Value = Value;
		Observation.bPresent = bPresent;
		Observation.bEffective = true;
		Observation.bSensitive = Spec.bSensitive;
		Observation.bExposeValue = Spec.bExposeValue && !Spec.bSensitive;
		OutObservations.Add(MoveTemp(Observation));
	}

	static void AddFileObservation(const FConfigFieldSpec& Spec, const FString& FilePath, TArray<FConfigObservation>& OutObservations)
	{
		FString Value;
		const bool bPresent = ReadIniValueFromFile(FilePath, Spec.Section, Spec.Key, Value);

		FConfigObservation Observation;
		Observation.Source = ToProjectRelativePath(FilePath);
		Observation.FilePath = FilePath;
		Observation.Section = Spec.Section;
		Observation.Key = Spec.Key;
		Observation.Value = Value;
		Observation.bPresent = bPresent;
		Observation.bEffective = false;
		Observation.bSensitive = Spec.bSensitive;
		Observation.bExposeValue = Spec.bExposeValue && !Spec.bSensitive;
		OutObservations.Add(MoveTemp(Observation));
	}

	static TArray<FConfigObservation> CollectObservations(const TArray<FConfigFieldSpec>& Specs)
	{
		TArray<FConfigObservation> Observations;
		Observations.Reserve(Specs.Num() * 4);

		for (const FConfigFieldSpec& Spec : Specs)
		{
			AddEffectiveObservation(Spec, GEngineIni, TEXT("effective_engine"), Observations);
			AddEffectiveObservation(Spec, GGameIni, TEXT("effective_game"), Observations);
		}

		for (const FString& FilePath : GetConfigFilesToScan())
		{
			for (const FConfigFieldSpec& Spec : Specs)
			{
				AddFileObservation(Spec, FilePath, Observations);
			}
		}

		return Observations;
	}

	static bool HasValue(const TArray<FConfigObservation>& Observations, const FString& Section, const FString& Key, FString* OutValue = nullptr)
	{
		for (const FConfigObservation& Observation : Observations)
		{
			if (Observation.bPresent
				&& !Observation.Value.TrimStartAndEnd().IsEmpty()
				&& Observation.Section.Equals(Section, ESearchCase::IgnoreCase)
				&& Observation.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				if (OutValue)
				{
					*OutValue = Observation.Value.TrimStartAndEnd();
				}
				return true;
			}
		}
		return false;
	}

	static bool HasValueEqual(const TArray<FConfigObservation>& Observations, const FString& Section, const FString& Key, const TArray<FString>& AcceptedValues)
	{
		for (const FConfigObservation& Observation : Observations)
		{
			const FString Value = Observation.Value.TrimStartAndEnd();
			if (!Observation.bPresent
				|| Value.IsEmpty()
				|| !Observation.Section.Equals(Section, ESearchCase::IgnoreCase)
				|| !Observation.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				continue;
			}
			for (const FString& AcceptedValue : AcceptedValues)
			{
				if (Value.Equals(AcceptedValue, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	static bool Is64Hex(const FString& Value)
	{
		if (Value.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Ch : Value)
		{
			if (!FChar::IsHexDigit(Ch))
			{
				return false;
			}
		}
		return true;
	}

	static TSharedPtr<FJsonObject> ObservationToJson(const FConfigObservation& Observation)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source"), Observation.Source);
		Row->SetStringField(TEXT("section"), Observation.Section);
		Row->SetStringField(TEXT("key"), Observation.Key);
		Row->SetBoolField(TEXT("present"), Observation.bPresent);
		Row->SetBoolField(TEXT("empty"), Observation.bPresent && Observation.Value.TrimStartAndEnd().IsEmpty());
		Row->SetBoolField(TEXT("effective"), Observation.bEffective);
		Row->SetBoolField(TEXT("sensitive"), Observation.bSensitive);
		if (!Observation.bPresent)
		{
			Row->SetStringField(TEXT("value_state"), TEXT("absent"));
		}
		else if (Observation.Value.TrimStartAndEnd().IsEmpty())
		{
			Row->SetStringField(TEXT("value_state"), TEXT("empty"));
		}
		else if (Observation.bSensitive)
		{
			Row->SetStringField(TEXT("value_state"), TEXT("present_redacted"));
			Row->SetStringField(TEXT("value"), TEXT("<redacted>"));
		}
		else
		{
			Row->SetStringField(TEXT("value_state"), TEXT("present"));
			if (Observation.bExposeValue)
			{
				Row->SetStringField(TEXT("value"), Observation.Value.TrimStartAndEnd());
			}
		}
		return Row;
	}

	static TSharedPtr<FJsonObject> BuildPluginRow(const FString& PluginName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("plugin"), PluginName);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Row->SetBoolField(TEXT("exists"), Plugin.IsValid());
		Row->SetBoolField(TEXT("enabled"), Plugin.IsValid() && Plugin->IsEnabled());
		return Row;
	}

	static TSharedPtr<FJsonObject> BuildModuleRow(const FString& ModuleName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("module"), ModuleName);
		Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(*ModuleName));
		Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(*ModuleName));
		return Row;
	}

	static TSharedPtr<FJsonObject> BuildClassRow(const FString& ClassPath, bool bAttemptLoad)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_path"), ClassPath);
		UClass* LoadedClass = FindObject<UClass>(nullptr, *ClassPath);
		Row->SetBoolField(TEXT("already_loaded"), LoadedClass != nullptr);
		if (bAttemptLoad && !LoadedClass)
		{
			LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_NoWarn);
		}
		Row->SetBoolField(TEXT("resolved"), LoadedClass != nullptr);
		if (LoadedClass)
		{
			Row->SetStringField(TEXT("resolved_class"), LoadedClass->GetPathName());
		}
		return Row;
	}

	static FString ExportPropertyValue(UObject* Owner, FProperty* Property)
	{
		if (!Owner || !Property)
		{
			return FString();
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Owner);
		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, Owner, PPF_None);
		return Exported;
	}

	static TSharedPtr<FJsonObject> BuildPropertyRow(UObject* Object, const FString& PropertyName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("property"), PropertyName);
		Row->SetBoolField(TEXT("present"), false);
		if (!Object || !Object->GetClass())
		{
			Row->SetStringField(TEXT("value_state"), TEXT("object_unavailable"));
			return Row;
		}

		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			Row->SetStringField(TEXT("value_state"), TEXT("property_missing"));
			return Row;
		}

		Row->SetBoolField(TEXT("present"), true);
		const FString Exported = ExportPropertyValue(Object, Property);
		Row->SetStringField(TEXT("value_state"), Exported.IsEmpty() ? TEXT("empty") : TEXT("present"));
		Row->SetStringField(TEXT("exported_value"), Exported);
		return Row;
	}

	static bool TryReadPositiveIntProperty(UObject* Object, const FString& PropertyName, int64& OutValue)
	{
		OutValue = 0;
		if (!Object || !Object->GetClass())
		{
			return false;
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
		FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
		if (!NumericProperty || NumericProperty->IsFloatingPoint())
		{
			return false;
		}
		const void* ValuePtr = NumericProperty->ContainerPtrToValuePtr<void>(Object);
		OutValue = NumericProperty->GetSignedIntPropertyValue(ValuePtr);
		return true;
	}

	static FString RedactLongCredentialRuns(const FString& Input)
	{
		FString Output;
		Output.Reserve(Input.Len());

		int32 Index = 0;
		while (Index < Input.Len())
		{
			const TCHAR Ch = Input[Index];
			const bool bTokenChar = FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('.') || Ch == TEXT(':') || Ch == TEXT('/');
			if (!bTokenChar)
			{
				Output.AppendChar(Ch);
				++Index;
				continue;
			}

			const int32 TokenStart = Index;
			bool bHasDigit = false;
			bool bHasUpper = false;
			while (Index < Input.Len())
			{
				const TCHAR TokenCh = Input[Index];
				const bool bCurrentTokenChar = FChar::IsAlnum(TokenCh) || TokenCh == TEXT('_') || TokenCh == TEXT('-') || TokenCh == TEXT('.') || TokenCh == TEXT(':') || TokenCh == TEXT('/');
				if (!bCurrentTokenChar)
				{
					break;
				}
				bHasDigit |= FChar::IsDigit(TokenCh);
				bHasUpper |= FChar::IsUpper(TokenCh);
				++Index;
			}

			const FString Token = Input.Mid(TokenStart, Index - TokenStart);
			if (Token.Len() >= 24 && (bHasDigit || bHasUpper))
			{
				Output += TEXT("<redacted>");
			}
			else
			{
				Output += Token;
			}
		}
		return Output;
	}

	static FString RedactLogLine(FString Line)
	{
		const FString Lower = Line.ToLower();
		const bool bCredentialBearing =
			Lower.Contains(TEXT("secret"))
			|| Lower.Contains(TEXT("token"))
			|| Lower.Contains(TEXT("bearer"))
			|| Lower.Contains(TEXT("cookie"))
			|| Lower.Contains(TEXT("authorization"))
			|| Lower.Contains(TEXT("clientid"))
			|| Lower.Contains(TEXT("client_id"))
			|| Lower.Contains(TEXT("device_code"))
			|| Lower.Contains(TEXT("verification_uri_complete"))
			|| Lower.Contains(TEXT("encryptionkey"))
			|| Lower.Contains(TEXT("password"));

		Line = RedactLongCredentialRuns(Line);
		if (bCredentialBearing)
		{
			Line += TEXT(" [credential fields redacted]");
		}
		return Line;
	}

	static FString ClassifyLogLine(const FString& Line)
	{
		if (Line.Contains(TEXT("client_has_no_application"), ESearchCase::IgnoreCase))
		{
			return TEXT("client_has_no_application");
		}
		if (Line.Contains(TEXT("invalid_client"), ESearchCase::IgnoreCase))
		{
			return TEXT("invalid_client");
		}
		if (Line.Contains(TEXT("AccountPortal"), ESearchCase::IgnoreCase))
		{
			return TEXT("account_portal");
		}
		if (Line.Contains(TEXT("EOS_Auth"), ESearchCase::IgnoreCase))
		{
			return TEXT("eos_auth");
		}
		if (Line.Contains(TEXT("EOS_Invalid"), ESearchCase::IgnoreCase))
		{
			return TEXT("eos_invalid");
		}
		if (Line.Contains(TEXT("device"), ESearchCase::IgnoreCase))
		{
			return TEXT("device_auth");
		}
		return TEXT("online");
	}

	static bool IsInterestingEOSLogLine(const FString& Line)
	{
		static const TCHAR* Patterns[] =
		{
			TEXT("AccountPortal"),
			TEXT("client_has_no_application"),
			TEXT("invalid_client"),
			TEXT("EOS_Auth"),
			TEXT("EOS_Invalid"),
			TEXT("errors.com.epicgames"),
			TEXT("verification_uri"),
			TEXT("device code"),
			TEXT("device_code"),
			TEXT("EOS_Connect")
		};

		for (const TCHAR* Pattern : Patterns)
		{
			if (Line.Contains(Pattern, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TArray<FString> ReadSectionLines(const FString& FilePath, const FString& Section)
	{
		TArray<FString> Lines;
		TArray<FString> SectionLines;
		if (!FPaths::FileExists(FilePath) || !FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			return SectionLines;
		}

		FString CurrentSection;
		for (const FString& RawLine : Lines)
		{
			FString Key;
			FString Value;
			if (ParseIniLine(RawLine, CurrentSection, Key, Value)
				&& CurrentSection.Equals(Section, ESearchCase::IgnoreCase))
			{
				SectionLines.Add(Key + TEXT("=") + Value);
			}
		}
		return SectionLines;
	}

	static TArray<FString> RequiredLobbyAttributes()
	{
		return
		{
			TEXT("GAMEMODE"),
			TEXT("MAPNAME"),
			TEXT("MATCHTIMEOUT"),
			TEXT("SESSIONTEMPLATENAME"),
			TEXT("OSSv2")
		};
	}

	static FLobbySchemaInfo CollectLobbySchemaInfo()
	{
		FLobbySchemaInfo Info;
		const TArray<FString> ExpectedAttributes = RequiredLobbyAttributes();

		for (const FString& FilePath : GetConfigFilesToScan())
		{
			for (const FString& Line : ReadSectionLines(FilePath, TEXT("OnlineServices.Lobbies")))
			{
				const FString LowerLine = Line.ToLower();
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("source"), ToProjectRelativePath(FilePath));
				if (LowerLine.StartsWith(TEXT("schemadescriptors=")))
				{
					Row->SetStringField(TEXT("kind"), TEXT("schema_descriptor"));
					Info.bHasGameLobbySchema |= Line.Contains(TEXT("GameLobby"), ESearchCase::IgnoreCase);
				}
				else if (LowerLine.StartsWith(TEXT("schemacategoryattributedescriptors=")))
				{
					Row->SetStringField(TEXT("kind"), TEXT("schema_category_attribute_descriptor"));
				}
				else if (LowerLine.StartsWith(TEXT("schemaattributedescriptors=")))
				{
					Row->SetStringField(TEXT("kind"), TEXT("schema_attribute_descriptor"));
				}
				else
				{
					Row->SetStringField(TEXT("kind"), TEXT("other"));
				}
				Row->SetStringField(TEXT("value_state"), TEXT("present"));
				Info.Rows.Add(MakeShared<FJsonValueObject>(Row));

				for (const FString& Attribute : ExpectedAttributes)
				{
					if (Line.Contains(Attribute, ESearchCase::IgnoreCase))
					{
						Info.SeenAttributes.Add(Attribute);
					}
				}
			}
		}

		return Info;
	}

	static void AddLobbySchemaChecks(
		const FLobbySchemaInfo& Info,
		TArray<TSharedPtr<FJsonValue>>& Checks,
		bool& bOverallOk,
		const FString& Severity)
	{
		AddCheck(Checks, bOverallOk, TEXT("game_lobby_schema_descriptor"),
			Info.bHasGameLobbySchema,
			Severity,
			TEXT("OnlineServices.Lobbies should declare a GameLobby schema descriptor for EOS lobbies."));

		for (const FString& Attribute : RequiredLobbyAttributes())
		{
			AddCheck(Checks, bOverallOk, FString::Printf(TEXT("lobby_attribute_%s"), *Attribute),
				Info.SeenAttributes.Contains(Attribute),
				Severity,
			FString::Printf(TEXT("OnlineServices.Lobbies should expose %s for CommonSession search/filter flow."), *Attribute));
		}
	}

	static UClass* ResolveClass(const FString& ClassPath)
	{
		UClass* LoadedClass = FindObject<UClass>(nullptr, *ClassPath);
		if (!LoadedClass)
		{
			LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_NoWarn);
		}
		return LoadedClass;
	}

	static UEnum* ResolveEnum(const FString& EnumPath)
	{
		UEnum* LoadedEnum = FindObject<UEnum>(nullptr, *EnumPath);
		if (!LoadedEnum)
		{
			LoadedEnum = Cast<UEnum>(StaticLoadObject(UEnum::StaticClass(), nullptr, *EnumPath, nullptr, LOAD_NoWarn));
		}
		return LoadedEnum;
	}

	static bool EnumContainsName(const UEnum* Enum, const FString& ExpectedName)
	{
		if (!Enum)
		{
			return false;
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.Equals(ExpectedName, ESearchCase::IgnoreCase)
				|| Name.EndsWith(TEXT("::") + ExpectedName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildEnumReport(const FString& EnumPath, const TArray<FString>& ExpectedNames, bool* bOutAllExpectedPresent = nullptr)
	{
		UEnum* Enum = ResolveEnum(EnumPath);
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("enum_path"), EnumPath);
		Report->SetBoolField(TEXT("resolved"), Enum != nullptr);
		if (Enum)
		{
			Report->SetStringField(TEXT("resolved_name"), Enum->GetPathName());
		}

		bool bAllExpectedPresent = Enum != nullptr;
		TArray<TSharedPtr<FJsonValue>> ExpectedRows;
		for (const FString& ExpectedName : ExpectedNames)
		{
			const bool bPresent = EnumContainsName(Enum, ExpectedName);
			bAllExpectedPresent &= bPresent;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), ExpectedName);
			Row->SetBoolField(TEXT("present"), bPresent);
			ExpectedRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("expected_values"), ExpectedRows);

		TArray<TSharedPtr<FJsonValue>> ReflectedRows;
		if (Enum)
		{
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
				Row->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
				Row->SetBoolField(TEXT("hidden"), Enum->HasMetaData(TEXT("Hidden"), Index));
				ReflectedRows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Report->SetArrayField(TEXT("reflected_values"), ReflectedRows);

		if (bOutAllExpectedPresent)
		{
			*bOutAllExpectedPresent = bAllExpectedPresent;
		}
		return Report;
	}

	static TSharedPtr<FJsonObject> BuildFunctionRow(const FString& ClassPath, const FString& FunctionName)
	{
		UClass* Class = ResolveClass(ClassPath);
		UFunction* Function = Class ? Class->FindFunctionByName(FName(*FunctionName)) : nullptr;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_path"), ClassPath);
		Row->SetStringField(TEXT("function"), FunctionName);
		Row->SetBoolField(TEXT("class_resolved"), Class != nullptr);
		Row->SetBoolField(TEXT("reflected"), Function != nullptr);
		if (Function)
		{
			Row->SetStringField(TEXT("function_path"), Function->GetPathName());
		}
		return Row;
	}

	static TSharedPtr<FJsonObject> MakeFlowStep(const FString& Name, const FString& Source, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("name"), Name);
		Step->SetStringField(TEXT("source"), Source);
		Step->SetStringField(TEXT("detail"), Detail);
		return Step;
	}

	static TSharedPtr<FJsonObject> MakeMappingRow(
		const FString& Privilege,
		const FString& OSSv1,
		const FString& OSSv2,
		const FString& Description,
		const FString& Note = FString())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("common_user_privilege"), Privilege);
		Row->SetStringField(TEXT("ossv1_privilege"), OSSv1);
		Row->SetStringField(TEXT("ossv2_privilege"), OSSv2);
		Row->SetStringField(TEXT("description"), Description);
		if (!Note.IsEmpty())
		{
			Row->SetStringField(TEXT("note"), Note);
		}
		return Row;
	}
}

void FMonolithOnlineActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("online"), TEXT("get_status"),
		TEXT("Report optional OnlineServices, EOS, CommonUser, and CommonSession plugin/module/reflection availability"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("validate_eos_ossv2_config"),
		TEXT("Validate EOS/OSSv2 configuration shape without printing credential values"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::ValidateEOSOSSv2Config),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("platform_config_name"), TEXT("string"), TEXT("EOSSDK.Platform.<name> section to validate; auto-detected when omitted"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("describe_common_session_flow"),
		TEXT("Describe CommonSession host, quick-play, OSSv1/OSSv2, and optional Lyra UserFacingExperience flow without creating sessions"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::DescribeCommonSessionFlow),
		FParamSchemaBuilder()
			.EnableValidation()
			.OptionalAssetPath(TEXT("user_facing_experience_path"), TEXT("Optional Lyra user-facing experience asset path to project into CommonSession host-request fields"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("validate_common_session_schema"),
		TEXT("Validate OnlineServices lobby schema and optional Lyra UserFacingExperience session fields without mutating assets"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::ValidateCommonSessionSchema),
		FParamSchemaBuilder()
			.EnableValidation()
			.OptionalAssetPath(TEXT("user_facing_experience_path"), TEXT("Optional Lyra user-facing experience asset path to validate against the session schema"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("validate_user_facing_session"),
		TEXT("Validate a Lyra UserFacingExperience as a CommonSession hosting contract without creating sessions or exposing credentials"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::ValidateUserFacingSession),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("user_facing_experience_path"), TEXT("Lyra user-facing experience asset path to validate"))
			.Optional(TEXT("require_online_session"), TEXT("boolean"), TEXT("Require SessionMode to be Online"), TEXT("false"))
			.Optional(TEXT("require_lobbies_for_online"), TEXT("boolean"), TEXT("Require online sessions to enable bUseLobbies"), TEXT("false"))
			.Optional(TEXT("require_lobby_schema"), TEXT("boolean"), TEXT("Require OnlineServices.Lobbies GameLobby schema when lobbies are effective"), TEXT("true"))
			.Optional(TEXT("require_frontend_visible"), TEXT("boolean"), TEXT("Require bShowInFrontEnd to be true"), TEXT("false"))
			.Optional(TEXT("require_resolved_primary_assets"), TEXT("boolean"), TEXT("Treat unresolved MapID/ExperienceID primary asset paths as errors"), TEXT("false"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("validate_common_user_initialization_contract"),
		TEXT("Validate CommonUser/CommonSession availability and Lyra initialization config contracts without logging credentials"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::ValidateCommonUserInitializationContract),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("validate_common_user_privilege_matrix"),
		TEXT("Validate reflected CommonUser privilege enums, login entry points, and OSSv1/OSSv2 privilege mapping contract without logging in"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::ValidateCommonUserPrivilegeMatrix),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("online"), TEXT("diagnose_eos_accountportal_logs"),
		TEXT("Scan local logs for EOS AccountPortal/auth failures with credential redaction"),
		FMonolithActionHandler::CreateStatic(&FMonolithOnlineActions::DiagnoseEOSAccountPortalLogs),
		FParamSchemaBuilder()
			.EnableValidation()
			.OptionalDiskPath(TEXT("log_root"), TEXT("Log directory to scan; defaults to Saved/Logs"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum matching log rows to return"), TEXT("50"))
			.Range(TEXT("max_results"), 1, 500)
			.Optional(TEXT("since_days"), TEXT("integer"), TEXT("Only scan logs modified within this many days; 0 disables time filtering"), TEXT("14"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithOnline::ExplicitReadOnlyPolicy());

	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("get_status"),
		{ TEXT("EOS"), TEXT("OSSv2"), TEXT("CommonUser"), TEXT("CommonSession"), TEXT("OnlineServices") },
		{ TEXT("online status"), TEXT("eos status") },
		{ TEXT("check whether EOS and CommonUser modules are available") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("validate_eos_ossv2_config"),
		{ TEXT("EOS config"), TEXT("OSSv2 config"), TEXT("ClientEncryptionKey"), TEXT("OnlineServices.EOS") },
		{ TEXT("validate eos settings"), TEXT("check ossv2") },
		{ TEXT("validate EOS config without exposing ProductId, ClientId, ClientSecret, or encryption key") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("describe_common_session_flow"),
		{ TEXT("CommonSession"), TEXT("host session"), TEXT("quick play"), TEXT("OSSv2 lobbies"), TEXT("Lyra UserFacingExperience") },
		{ TEXT("describe common session flow"), TEXT("inspect hosting flow") },
		{ TEXT("describe CommonSession host and quick-play branch contracts without creating sessions") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("validate_common_session_schema"),
		{ TEXT("CommonSession"), TEXT("OnlineServices.Lobbies"), TEXT("Lyra UserFacingExperience"), TEXT("GameLobby") },
		{ TEXT("validate lobby schema"), TEXT("validate session schema") },
		{ TEXT("validate that OnlineServices.Lobbies includes expected Lyra CommonSession attributes") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("validate_user_facing_session"),
		{ TEXT("Lyra UserFacingExperience"), TEXT("CommonSession"), TEXT("hosting request"), TEXT("SessionMode"), TEXT("GameLobby") },
		{ TEXT("validate user facing session"), TEXT("validate playlist hosting"), TEXT("check CommonSession host request") },
		{ TEXT("validate a Lyra user-facing experience as a CommonSession hosting contract") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("validate_common_user_initialization_contract"),
		{ TEXT("CommonUser"), TEXT("CommonSession"), TEXT("LyraLocalPlayer"), TEXT("login flow") },
		{ TEXT("validate common user"), TEXT("check login contract") },
		{ TEXT("check CommonUser subsystem and Lyra initialization class configuration") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("validate_common_user_privilege_matrix"),
		{ TEXT("CommonUser"), TEXT("privileges"), TEXT("CanPlayOnline"), TEXT("CanUseCrossPlay"), TEXT("OSSv2 privileges") },
		{ TEXT("validate privilege matrix"), TEXT("check common user privileges") },
		{ TEXT("validate CommonUser privilege enum and OSSv1/OSSv2 mapping contract without logging in") });
	Registry.SetActionSearchMetadata(TEXT("online"), TEXT("diagnose_eos_accountportal_logs"),
		{ TEXT("EOS AccountPortal"), TEXT("client_has_no_application"), TEXT("invalid_client"), TEXT("redacted logs") },
		{ TEXT("diagnose eos login"), TEXT("accountportal logs") },
		{ TEXT("scan Saved/Logs for EOS AccountPortal failures with secret redaction") });

	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("get_status"),
		TEXT("unreal-online"),
		{ TEXT("None") },
		{ TEXT("Plugin, module, and reflected class availability") },
		{ TEXT("online.validate_eos_ossv2_config"), TEXT("online.validate_common_user_initialization_contract") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("validate_eos_ossv2_config"),
		TEXT("unreal-online"),
		{ TEXT("Project config files must be readable; credential values are never returned") },
		{ TEXT("ok flag, checks, config field presence/provenance, ClientEncryptionKey shape status") },
		{ TEXT("online.validate_common_session_schema"), TEXT("online.diagnose_eos_accountportal_logs") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("describe_common_session_flow"),
		TEXT("unreal-online"),
		{ TEXT("CommonSession classes are resolved by reflection; optional UserFacingExperience path can be provided") },
		{ TEXT("CommonSession host/default/quick-play flow, OSSv1/OSSv2 branch rules, advertised attributes, optional host-request projection") },
		{ TEXT("online.validate_common_session_schema"), TEXT("online.validate_user_facing_session") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("validate_common_session_schema"),
		TEXT("unreal-online"),
		{ TEXT("OnlineServices.Lobbies config is scanned; optional UserFacingExperience path can be provided") },
		{ TEXT("Lobby schema descriptor status and optional user-facing experience session field report") },
		{ TEXT("lyra.validate_user_facing_experience") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("validate_user_facing_session"),
		TEXT("unreal-online"),
		{ TEXT("A ULyraUserFacingExperienceDefinition asset path is required; no PIE/session creation or EOS service call is performed") },
		{ TEXT("CommonSession host-request projection, map/experience IDs, lobby schema alignment, and frontend/session flag checks") },
		{ TEXT("lyra.validate_user_facing_experience"), TEXT("online.validate_common_session_schema"), TEXT("online.validate_eos_ossv2_config") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("validate_common_user_initialization_contract"),
		TEXT("unreal-online"),
		{ TEXT("CommonUser/CommonSession modules are optional and resolved by plugin/module/reflection status") },
		{ TEXT("CommonUser/CommonSession class availability and Lyra initialization config checks") },
		{ TEXT("online.validate_eos_ossv2_config") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("validate_common_user_privilege_matrix"),
		TEXT("unreal-online"),
		{ TEXT("CommonUser reflected enums/classes must be loadable; no login or privilege query is performed") },
		{ TEXT("Privilege enum availability, OSSv1/OSSv2 mapping table, initialization entry points, result bucket contract") },
		{ TEXT("online.validate_common_user_initialization_contract"), TEXT("online.describe_common_session_flow") });
	Registry.SetActionPlanningMetadata(TEXT("online"), TEXT("diagnose_eos_accountportal_logs"),
		TEXT("unreal-online"),
		{ TEXT("Local log files must be readable; returned messages are redacted") },
		{ TEXT("Bounded classified EOS auth/account portal log rows") },
		{ TEXT("online.validate_eos_ossv2_config") });
}

FMonolithActionResult FMonolithOnlineActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only_online_diagnostics"));
	Result->SetBoolField(TEXT("namespace_registered"), FMonolithToolRegistry::Get().HasNamespace(TEXT("online")));
	Result->SetStringField(TEXT("sample_utc"), FDateTime::UtcNow().ToIso8601());

	const TArray<FString> Plugins =
	{
		TEXT("OnlineSubsystem"),
		TEXT("OnlineSubsystemEOS"),
		TEXT("OnlineServices"),
		TEXT("OnlineServicesEOS"),
		TEXT("EOSShared"),
		TEXT("CommonUser"),
		TEXT("CommonGame")
	};
	TArray<TSharedPtr<FJsonValue>> PluginRows;
	for (const FString& PluginName : Plugins)
	{
		PluginRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildPluginRow(PluginName)));
	}
	Result->SetArrayField(TEXT("plugins"), PluginRows);

	const TArray<FString> Modules =
	{
		TEXT("OnlineSubsystem"),
		TEXT("OnlineSubsystemEOS"),
		TEXT("OnlineServicesInterface"),
		TEXT("OnlineServicesCommon"),
		TEXT("OnlineServicesEOS"),
		TEXT("EOSShared"),
		TEXT("CommonUser"),
		TEXT("CommonGame")
	};
	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	for (const FString& ModuleName : Modules)
	{
		ModuleRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildModuleRow(ModuleName)));
	}
	Result->SetArrayField(TEXT("modules"), ModuleRows);

	const TArray<FString> ClassPaths =
	{
		TEXT("/Script/CommonUser.CommonUserSubsystem"),
		TEXT("/Script/CommonUser.CommonSessionSubsystem"),
		TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"),
		TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"),
		TEXT("/Script/LyraGame.LyraLocalPlayer"),
		TEXT("/Script/LyraGame.LyraGameInstance")
	};
	TArray<TSharedPtr<FJsonValue>> ClassRows;
	for (const FString& ClassPath : ClassPaths)
	{
		ClassRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildClassRow(ClassPath, false)));
	}
	Result->SetArrayField(TEXT("reflected_classes"), ClassRows);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::ValidateEOSOSSv2Config(const TSharedPtr<FJsonObject>& Params)
{
	FString PlatformConfigName;
	FString Error;
	if (!MonolithOnline::ReadOptionalStringParam(Params, TEXT("platform_config_name"), PlatformConfigName, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("platform_config_name"), Error));
	}

	TArray<MonolithOnline::FConfigFieldSpec> BootstrapSpecs =
	{
		{ TEXT("OnlineServices"), TEXT("DefaultServices"), false, true },
		{ TEXT("OnlineServices"), TEXT("PlatformServices"), false, true },
		{ TEXT("OnlineServices.EOS"), TEXT("PlatformConfigName"), false, true },
		{ TEXT("EOSSDK"), TEXT("DefaultPlatformConfigName"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("PlatformConfigName"), false, true }
	};
	const TArray<MonolithOnline::FConfigObservation> BootstrapObservations = MonolithOnline::CollectObservations(BootstrapSpecs);
	if (PlatformConfigName.IsEmpty())
	{
		MonolithOnline::HasValue(BootstrapObservations, TEXT("OnlineServices.EOS"), TEXT("PlatformConfigName"), &PlatformConfigName)
			|| MonolithOnline::HasValue(BootstrapObservations, TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("PlatformConfigName"), &PlatformConfigName)
			|| MonolithOnline::HasValue(BootstrapObservations, TEXT("EOSSDK"), TEXT("DefaultPlatformConfigName"), &PlatformConfigName);
	}
	if (PlatformConfigName.IsEmpty())
	{
		PlatformConfigName = TEXT("Default");
	}

	const FString PlatformSection = FString::Printf(TEXT("EOSSDK.Platform.%s"), *PlatformConfigName);
	TArray<MonolithOnline::FConfigFieldSpec> Specs =
	{
		{ TEXT("OnlineServices"), TEXT("DefaultServices"), false, true },
		{ TEXT("OnlineServices"), TEXT("PlatformServices"), false, true },
		{ TEXT("OnlineServices.EOS"), TEXT("PlatformConfigName"), false, true },
		{ TEXT("OnlineServices.EOS"), TEXT("bUseEAS"), false, true },
		{ TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), false, true },
		{ TEXT("OnlineSubsystemEOS"), TEXT("bEnabled"), false, true },
		{ TEXT("OnlineSubsystemEOSPlus"), TEXT("bEnabled"), false, true },
		{ TEXT("EOSSDK"), TEXT("DefaultPlatformConfigName"), false, true },
		{ PlatformSection, TEXT("ProductId"), true, false },
		{ PlatformSection, TEXT("SandboxId"), true, false },
		{ PlatformSection, TEXT("DeploymentId"), true, false },
		{ PlatformSection, TEXT("ClientId"), true, false },
		{ PlatformSection, TEXT("ClientSecret"), true, false },
		{ PlatformSection, TEXT("ClientEncryptionKey"), true, false },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("DefaultArtifactName"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("PlatformConfigName"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("bUseNamedPlatformConfig"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("bUseEAS"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("bUseEOSConnect"), false, true },
		{ TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("bUseEOSSessions"), false, true },
		{ TEXT("/Script/SocketSubsystemEOS.NetDriverEOSBase"), TEXT("bIsUsingP2PSockets"), false, true }
	};
	const TArray<MonolithOnline::FConfigObservation> Observations = MonolithOnline::CollectObservations(Specs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("validate_eos_ossv2_config"));
	Result->SetStringField(TEXT("platform_config_name"), PlatformConfigName);
	Result->SetBoolField(TEXT("redaction_applied"), true);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	MonolithOnline::AddCheck(Checks, bOk, TEXT("ossv2_default_services"),
		MonolithOnline::HasValueEqual(Observations, TEXT("OnlineServices"), TEXT("DefaultServices"), { TEXT("Epic"), TEXT("EOS") }),
		TEXT("error"),
		TEXT("OnlineServices.DefaultServices should resolve to Epic/EOS in an EOS custom config layer for OSSv2 hosting."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("online_services_eos_platform_config"),
		MonolithOnline::HasValue(Observations, TEXT("OnlineServices.EOS"), TEXT("PlatformConfigName")),
		TEXT("error"),
		TEXT("OnlineServices.EOS.PlatformConfigName must point at an EOSSDK.Platform.<name> section."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_sdk_product_id"),
		MonolithOnline::HasValue(Observations, PlatformSection, TEXT("ProductId")),
		TEXT("error"),
		TEXT("EOSSDK platform ProductId is required."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_sdk_sandbox_id"),
		MonolithOnline::HasValue(Observations, PlatformSection, TEXT("SandboxId")),
		TEXT("error"),
		TEXT("EOSSDK platform SandboxId is required."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_sdk_deployment_id"),
		MonolithOnline::HasValue(Observations, PlatformSection, TEXT("DeploymentId")),
		TEXT("error"),
		TEXT("EOSSDK platform DeploymentId is required."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_sdk_client_id"),
		MonolithOnline::HasValue(Observations, PlatformSection, TEXT("ClientId")),
		TEXT("error"),
		TEXT("EOSSDK platform ClientId is required."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_sdk_client_secret"),
		MonolithOnline::HasValue(Observations, PlatformSection, TEXT("ClientSecret")),
		TEXT("error"),
		TEXT("EOSSDK platform ClientSecret is required but is never returned by this action."));

	FString ClientEncryptionKey;
	const bool bHasClientEncryptionKey = MonolithOnline::HasValue(Observations, PlatformSection, TEXT("ClientEncryptionKey"), &ClientEncryptionKey);
	MonolithOnline::AddCheck(Checks, bOk, TEXT("client_encryption_key_present"),
		bHasClientEncryptionKey,
		TEXT("error"),
		TEXT("EOSSDK platform ClientEncryptionKey is required for EOS encrypted storage features."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("client_encryption_key_64_hex"),
		bHasClientEncryptionKey && MonolithOnline::Is64Hex(ClientEncryptionKey),
		TEXT("error"),
		TEXT("ClientEncryptionKey must be exactly 64 hexadecimal characters."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_named_platform_config"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("PlatformConfigName")),
		TEXT("warning"),
		TEXT("OnlineSubsystemEOS.EOSSettings PlatformConfigName should match the EOSSDK platform when OSSv1/EOS net drivers are used."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("eos_p2p_netdriver"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/SocketSubsystemEOS.NetDriverEOSBase"), TEXT("bIsUsingP2PSockets")),
		TEXT("warning"),
		TEXT("SocketSubsystemEOS P2P net driver setting was not found; listen-server NAT behavior may depend on the active net driver."));

	TArray<TSharedPtr<FJsonValue>> FieldRows;
	FieldRows.Reserve(Observations.Num());
	for (const MonolithOnline::FConfigObservation& Observation : Observations)
	{
		FieldRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::ObservationToJson(Observation)));
	}

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("fields"), FieldRows);
	Result->SetArrayField(TEXT("config_files_scanned"), MonolithOnline::StringsToJson(MonolithOnline::GetConfigFilesToScan()));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::DescribeCommonSessionFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString UserFacingExperiencePath;
	FString Error;
	if (!MonolithOnline::ReadOptionalStringParam(Params, TEXT("user_facing_experience_path"), UserFacingExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("user_facing_experience_path"), Error));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("describe_common_session_flow"));
	Result->SetBoolField(TEXT("redaction_applied"), true);
	Result->SetStringField(TEXT("mode"), TEXT("read_only_static_contract"));

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> SystemRows;

	TSharedPtr<FJsonObject> CommonUserPlugin = MonolithOnline::BuildPluginRow(TEXT("CommonUser"));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("plugin_CommonUser_enabled"),
		CommonUserPlugin->GetBoolField(TEXT("enabled")),
		TEXT("error"),
		TEXT("CommonUser plugin should be enabled for CommonSession hosting flow."));
	SystemRows.Add(MakeShared<FJsonValueObject>(CommonUserPlugin));

	TSharedPtr<FJsonObject> CommonUserModule = MonolithOnline::BuildModuleRow(TEXT("CommonUser"));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("module_CommonUser_exists"),
		CommonUserModule->GetBoolField(TEXT("exists")),
		TEXT("error"),
		TEXT("CommonUser module should exist."));
	SystemRows.Add(MakeShared<FJsonValueObject>(CommonUserModule));

	const TArray<FString> ClassPaths =
	{
		TEXT("/Script/CommonUser.CommonSessionSubsystem"),
		TEXT("/Script/CommonUser.CommonSession_HostSessionRequest"),
		TEXT("/Script/CommonUser.CommonSession_SearchSessionRequest"),
		TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition")
	};
	for (const FString& ClassPath : ClassPaths)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildClassRow(ClassPath, true);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("class_%s_resolved"), *ClassPath.Replace(TEXT("/Script/"), TEXT(""))),
			Row->GetBoolField(TEXT("resolved")),
			TEXT("error"),
			FString::Printf(TEXT("Reflected class %s should resolve for static session flow diagnostics."), *ClassPath));
		SystemRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("systems"), SystemRows);

	UClass* CommonSessionSubsystemClass = MonolithOnline::ResolveClass(TEXT("/Script/CommonUser.CommonSessionSubsystem"));
	UObject* CommonSessionSubsystemCDO = CommonSessionSubsystemClass ? CommonSessionSubsystemClass->GetDefaultObject() : nullptr;
	TArray<TSharedPtr<FJsonValue>> DefaultRows;
	const TArray<FString> DefaultPropertyNames =
	{
		TEXT("bUseLobbiesDefault"),
		TEXT("bUseLobbiesVoiceChatDefault"),
		TEXT("bUseBeacons")
	};
	for (const FString& PropertyName : DefaultPropertyNames)
	{
		DefaultRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildPropertyRow(CommonSessionSubsystemCDO, PropertyName)));
	}
	TSharedPtr<FJsonObject> HostDefaults = MakeShared<FJsonObject>();
	HostDefaults->SetStringField(TEXT("source"), TEXT("UCommonSessionSubsystem::CreateOnlineHostSessionRequest"));
	HostDefaults->SetStringField(TEXT("OnlineMode"), TEXT("Online"));
	HostDefaults->SetStringField(TEXT("bUseLobbies"), TEXT("bUseLobbiesDefault"));
	HostDefaults->SetStringField(TEXT("bUseLobbiesVoiceChat"), TEXT("bUseLobbiesVoiceChatDefault"));
	HostDefaults->SetStringField(TEXT("bUsePresence"), TEXT("!IsRunningDedicatedServer()"));
	HostDefaults->SetArrayField(TEXT("config_backed_defaults"), DefaultRows);
	Result->SetObjectField(TEXT("host_request_defaults"), HostDefaults);

	TArray<TSharedPtr<FJsonValue>> FunctionRows;
	const TArray<TPair<FString, FString>> ReflectedFunctions =
	{
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("CreateOnlineHostSessionRequest")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("CreateOnlineSearchSessionRequest")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("HostSession")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("QuickPlaySession")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("FindSessions")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("JoinSession")),
		TPair<FString, FString>(TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"), TEXT("CreateHostingRequest"))
	};
	for (const TPair<FString, FString>& FunctionSpec : ReflectedFunctions)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildFunctionRow(FunctionSpec.Key, FunctionSpec.Value);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("function_%s_reflected"), *FunctionSpec.Value),
			Row->GetBoolField(TEXT("reflected")),
			TEXT("error"),
			FString::Printf(TEXT("%s::%s should be reflected."), *FunctionSpec.Key, *FunctionSpec.Value));
		FunctionRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("reflected_functions"), FunctionRows);

	TArray<TSharedPtr<FJsonValue>> FlowSteps;
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("create_request"), TEXT("UCommonSessionSubsystem::CreateOnlineHostSessionRequest"), TEXT("Creates UCommonSession_HostSessionRequest with Online mode, lobby defaults, voice defaults, and presence disabled on dedicated server."))));
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("lyra_project_request"), TEXT("ULyraUserFacingExperienceDefinition::CreateHostingRequest"), TEXT("Projects MapID, ExperienceID, ExtraArgs, MaxPlayerCount, SessionMode, lobby, voice, presence, replay, and advertised mode into UCommonSession_HostSessionRequest."))));
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("host_session"), TEXT("UCommonSessionSubsystem::HostSession"), TEXT("Rejects null request, invalid non-dedicated hosting player, and invalid request; offline mode travels immediately, other modes create online session."))));
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("ossv2_sessions_branch"), TEXT("UCommonSessionSubsystem::CreateOnlineSessionInternalOSSv2"), TEXT("When OnlineMode is not Online+lobbies, creates an OnlineServices session with GameLobby schema, presence flag, max players, and advertised custom settings."))));
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("ossv2_lobbies_branch"), TEXT("UCommonSessionSubsystem::CreateOnlineSessionInternalOSSv2"), TEXT("When OnlineMode is Online and bUseLobbies is true, creates an OnlineServices lobby with GameLobby schema, public advertised join policy, max members, presence, and advertised attributes."))));
	FlowSteps.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("quick_play"), TEXT("UCommonSessionSubsystem::QuickPlaySession"), TEXT("Searches first, joins the best result when one exists, otherwise hosts with the supplied request."))));
	Result->SetArrayField(TEXT("flow_steps"), FlowSteps);

	TArray<TSharedPtr<FJsonValue>> AdvertisedAttributes;
	const TArray<TPair<FString, FString>> AttributeSources =
	{
		TPair<FString, FString>(TEXT("GAMEMODE"), TEXT("ModeNameForAdvertisement")),
		TPair<FString, FString>(TEXT("MAPNAME"), TEXT("GetMapName()")),
		TPair<FString, FString>(TEXT("MATCHTIMEOUT"), TEXT("120.0")),
		TPair<FString, FString>(TEXT("SESSIONTEMPLATENAME"), TEXT("GameSession")),
		TPair<FString, FString>(TEXT("OSSv2"), TEXT("true"))
	};
	for (const TPair<FString, FString>& AttributeSource : AttributeSources)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("attribute"), AttributeSource.Key);
		Row->SetStringField(TEXT("source_value"), AttributeSource.Value);
		Row->SetStringField(TEXT("schema"), TEXT("GameLobby"));
		AdvertisedAttributes.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("advertised_attributes"), AdvertisedAttributes);

	TArray<TSharedPtr<FJsonValue>> BranchRules;
	BranchRules.Add(MakeShared<FJsonValueString>(TEXT("Offline mode does not create online services state; it server-travels after request validation.")));
	BranchRules.Add(MakeShared<FJsonValueString>(TEXT("LAN mode creates online session state but masks lobby, lobby voice, and presence flags off.")));
	BranchRules.Add(MakeShared<FJsonValueString>(TEXT("Online mode with bUseLobbies=false uses OnlineServices sessions; online mode with bUseLobbies=true uses OnlineServices lobbies.")));
	BranchRules.Add(MakeShared<FJsonValueString>(TEXT("Online non-dedicated hosting requires a valid local account id before session or lobby creation.")));
	BranchRules.Add(MakeShared<FJsonValueString>(TEXT("Default CommonSession quick play does not add GAMEMODE or MAPNAME search filters unless a project subclass overrides it.")));
	Result->SetArrayField(TEXT("branch_rules"), BranchRules);

	const MonolithOnline::FLobbySchemaInfo LobbySchema = MonolithOnline::CollectLobbySchemaInfo();
	Result->SetBoolField(TEXT("game_lobby_schema_configured"), LobbySchema.bHasGameLobbySchema);
	Result->SetArrayField(TEXT("detected_lobby_attributes"), MonolithOnline::StringsToJson(LobbySchema.SeenAttributes.Array()));

	if (!UserFacingExperiencePath.IsEmpty())
	{
		TSharedPtr<FJsonObject> ProjectionParams = MakeShared<FJsonObject>();
		ProjectionParams->SetStringField(TEXT("user_facing_experience_path"), UserFacingExperiencePath);
		ProjectionParams->SetBoolField(TEXT("require_lobby_schema"), false);
		FMonolithActionResult Projection = ValidateUserFacingSession(ProjectionParams);
		if (Projection.bSuccess && Projection.Result.IsValid())
		{
			Result->SetObjectField(TEXT("user_facing_session_projection"), Projection.Result);
		}
		else
		{
			TSharedPtr<FJsonObject> ProjectionError = MakeShared<FJsonObject>();
			ProjectionError->SetBoolField(TEXT("success"), false);
			ProjectionError->SetStringField(TEXT("error"), Projection.ErrorMessage);
			ProjectionError->SetNumberField(TEXT("error_code"), Projection.ErrorCode);
			Result->SetObjectField(TEXT("user_facing_session_projection"), ProjectionError);
		}
	}

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::ValidateCommonSessionSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString UserFacingExperiencePath;
	FString Error;
	if (!MonolithOnline::ReadOptionalStringParam(Params, TEXT("user_facing_experience_path"), UserFacingExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("user_facing_experience_path"), Error));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("validate_common_session_schema"));
	Result->SetBoolField(TEXT("redaction_applied"), true);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	const MonolithOnline::FLobbySchemaInfo LobbySchema = MonolithOnline::CollectLobbySchemaInfo();
	MonolithOnline::AddLobbySchemaChecks(LobbySchema, Checks, bOk, TEXT("error"));

	Result->SetArrayField(TEXT("lobby_schema_rows"), LobbySchema.Rows);
	Result->SetArrayField(TEXT("detected_lobby_attributes"), MonolithOnline::StringsToJson(LobbySchema.SeenAttributes.Array()));

	if (!UserFacingExperiencePath.IsEmpty())
	{
		const FString ObjectPath = MonolithOnline::NormalizeObjectPath(UserFacingExperiencePath);
		UObject* UserFacingExperience = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		TSharedPtr<FJsonObject> AssetReport = MakeShared<FJsonObject>();
		AssetReport->SetStringField(TEXT("input_path"), UserFacingExperiencePath);
		AssetReport->SetStringField(TEXT("resolved_path"), ObjectPath);
		AssetReport->SetBoolField(TEXT("loaded"), UserFacingExperience != nullptr);
		if (UserFacingExperience)
		{
			AssetReport->SetStringField(TEXT("class"), UserFacingExperience->GetClass()->GetPathName());
			TArray<TSharedPtr<FJsonValue>> PropertyRows;
			const TArray<FString> PropertyNames =
			{
				TEXT("MapID"),
				TEXT("ExperienceID"),
				TEXT("MaxPlayerCount"),
				TEXT("SessionMode"),
				TEXT("bUseLobbies"),
				TEXT("bUseLobbiesVoiceChat"),
				TEXT("bUsePresence"),
				TEXT("bShowInFrontEnd"),
				TEXT("bIsDefaultExperience")
			};
			for (const FString& PropertyName : PropertyNames)
			{
				PropertyRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildPropertyRow(UserFacingExperience, PropertyName)));
			}
			AssetReport->SetArrayField(TEXT("properties"), PropertyRows);

			int64 MaxPlayerCount = 0;
			const bool bHasMaxPlayerCount = MonolithOnline::TryReadPositiveIntProperty(UserFacingExperience, TEXT("MaxPlayerCount"), MaxPlayerCount);
			MonolithOnline::AddCheck(Checks, bOk, TEXT("user_facing_max_player_count"),
				bHasMaxPlayerCount && MaxPlayerCount > 0,
				TEXT("error"),
				TEXT("UserFacingExperience MaxPlayerCount should be a positive integer."));
			MonolithOnline::AddCheck(Checks, bOk, TEXT("user_facing_map_id_present"),
				UserFacingExperience->GetClass()->FindPropertyByName(TEXT("MapID")) != nullptr
					&& !MonolithOnline::ExportPropertyValue(UserFacingExperience, UserFacingExperience->GetClass()->FindPropertyByName(TEXT("MapID"))).IsEmpty(),
				TEXT("error"),
				TEXT("UserFacingExperience MapID should be populated."));
			MonolithOnline::AddCheck(Checks, bOk, TEXT("user_facing_experience_id_present"),
				UserFacingExperience->GetClass()->FindPropertyByName(TEXT("ExperienceID")) != nullptr
					&& !MonolithOnline::ExportPropertyValue(UserFacingExperience, UserFacingExperience->GetClass()->FindPropertyByName(TEXT("ExperienceID"))).IsEmpty(),
				TEXT("error"),
				TEXT("UserFacingExperience ExperienceID should be populated."));
		}
		else
		{
			MonolithOnline::AddCheck(Checks, bOk, TEXT("user_facing_experience_load"),
				false,
				TEXT("error"),
				TEXT("user_facing_experience_path could not be loaded."));
		}
		Result->SetObjectField(TEXT("user_facing_experience"), AssetReport);
	}

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::ValidateUserFacingSession(const TSharedPtr<FJsonObject>& Params)
{
	FString UserFacingExperiencePath;
	FString Error;
	if (!MonolithOnline::ReadRequiredStringParam(Params, TEXT("user_facing_experience_path"), UserFacingExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("user_facing_experience_path"), Error));
	}

	bool bRequireOnlineSession = false;
	bool bRequireLobbiesForOnline = false;
	bool bRequireLobbySchema = true;
	bool bRequireFrontendVisible = false;
	bool bRequireResolvedPrimaryAssets = false;
	if (!MonolithOnline::ReadBoolParam(Params, TEXT("require_online_session"), bRequireOnlineSession, Error)
		|| !MonolithOnline::ReadBoolParam(Params, TEXT("require_lobbies_for_online"), bRequireLobbiesForOnline, Error)
		|| !MonolithOnline::ReadBoolParam(Params, TEXT("require_lobby_schema"), bRequireLobbySchema, Error)
		|| !MonolithOnline::ReadBoolParam(Params, TEXT("require_frontend_visible"), bRequireFrontendVisible, Error)
		|| !MonolithOnline::ReadBoolParam(Params, TEXT("require_resolved_primary_assets"), bRequireResolvedPrimaryAssets, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("boolean_param"), Error));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("validate_user_facing_session"));
	Result->SetBoolField(TEXT("redaction_applied"), true);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> SystemRows;

	TSharedPtr<FJsonObject> CommonUserPlugin = MonolithOnline::BuildPluginRow(TEXT("CommonUser"));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("plugin_CommonUser_enabled"),
		CommonUserPlugin->GetBoolField(TEXT("enabled")),
		TEXT("error"),
		TEXT("CommonUser plugin should be enabled for CommonSession host requests."));
	SystemRows.Add(MakeShared<FJsonValueObject>(CommonUserPlugin));

	TSharedPtr<FJsonObject> CommonUserModule = MonolithOnline::BuildModuleRow(TEXT("CommonUser"));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("module_CommonUser_exists"),
		CommonUserModule->GetBoolField(TEXT("exists")),
		TEXT("error"),
		TEXT("CommonUser module should exist."));
	SystemRows.Add(MakeShared<FJsonValueObject>(CommonUserModule));

	TSharedPtr<FJsonObject> HostRequestClass = MonolithOnline::BuildClassRow(TEXT("/Script/CommonUser.CommonSession_HostSessionRequest"), true);
	MonolithOnline::AddCheck(Checks, bOk, TEXT("class_CommonSession_HostSessionRequest_resolved"),
		HostRequestClass->GetBoolField(TEXT("resolved")),
		TEXT("error"),
		TEXT("UCommonSession_HostSessionRequest should resolve for Lyra hosting requests."));
	SystemRows.Add(MakeShared<FJsonValueObject>(HostRequestClass));
	Result->SetArrayField(TEXT("systems"), SystemRows);

	const FString ObjectPath = MonolithOnline::NormalizeObjectPath(UserFacingExperiencePath);
	UObject* UserFacingExperience = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
	TSharedPtr<FJsonObject> AssetReport = MakeShared<FJsonObject>();
	AssetReport->SetStringField(TEXT("input_path"), UserFacingExperiencePath);
	AssetReport->SetStringField(TEXT("resolved_path"), ObjectPath);
	AssetReport->SetBoolField(TEXT("loaded"), UserFacingExperience != nullptr);
	MonolithOnline::AddCheck(Checks, bOk, TEXT("user_facing_experience_load"),
		UserFacingExperience != nullptr,
		TEXT("error"),
		UserFacingExperience ? TEXT("Loaded") : TEXT("user_facing_experience_path could not be loaded."));

	if (UserFacingExperience)
	{
		AssetReport->SetStringField(TEXT("class"), UserFacingExperience->GetClass()->GetPathName());
		UClass* ExpectedClass = StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"), nullptr, LOAD_NoWarn);
		MonolithOnline::AddCheck(Checks, bOk, TEXT("class_LyraUserFacingExperienceDefinition_resolved"),
			ExpectedClass != nullptr,
			TEXT("error"),
			TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition should resolve."));
		MonolithOnline::AddCheck(Checks, bOk, TEXT("asset_class_matches_user_facing_experience"),
			ExpectedClass && UserFacingExperience->IsA(ExpectedClass),
			TEXT("error"),
			FString::Printf(TEXT("asset_class=%s"), *UserFacingExperience->GetClass()->GetPathName()));

		TArray<TSharedPtr<FJsonValue>> PropertyRows;
		const TArray<FString> PropertyNames =
		{
			TEXT("MapID"),
			TEXT("ExperienceID"),
			TEXT("ExtraArgs"),
			TEXT("LoadingScreenWidget"),
			TEXT("bShowInFrontEnd"),
			TEXT("bRecordReplay"),
			TEXT("MaxPlayerCount"),
			TEXT("SessionMode"),
			TEXT("bUseLobbies"),
			TEXT("bUseLobbiesVoiceChat"),
			TEXT("bUsePresence")
		};
		for (const FString& PropertyName : PropertyNames)
		{
			PropertyRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildPropertyRow(UserFacingExperience, PropertyName)));
		}
		AssetReport->SetArrayField(TEXT("properties"), PropertyRows);

		FPrimaryAssetId MapId;
		FPrimaryAssetId ExperienceId;
		MonolithOnline::TryReadPrimaryAssetIdProperty(UserFacingExperience, TEXT("MapID"), MapId);
		MonolithOnline::TryReadPrimaryAssetIdProperty(UserFacingExperience, TEXT("ExperienceID"), ExperienceId);
		AssetReport->SetObjectField(TEXT("map_id"), MonolithOnline::PrimaryAssetIdToJson(MapId));
		AssetReport->SetObjectField(TEXT("experience_id"), MonolithOnline::PrimaryAssetIdToJson(ExperienceId));

		MonolithOnline::AddCheck(Checks, bOk, TEXT("map_id_valid"),
			MapId.IsValid(),
			TEXT("error"),
			MapId.ToString());
		MonolithOnline::AddCheck(Checks, bOk, TEXT("map_id_type"),
			!MapId.IsValid() || MapId.PrimaryAssetType.ToString().Equals(TEXT("Map"), ESearchCase::IgnoreCase),
			TEXT("error"),
			MapId.PrimaryAssetType.ToString());
		MonolithOnline::AddCheck(Checks, bOk, TEXT("experience_id_valid"),
			ExperienceId.IsValid(),
			TEXT("error"),
			ExperienceId.ToString());
		MonolithOnline::AddCheck(Checks, bOk, TEXT("experience_id_type"),
			!ExperienceId.IsValid() || ExperienceId.PrimaryAssetType.ToString().Equals(TEXT("LyraExperienceDefinition"), ESearchCase::IgnoreCase),
			TEXT("error"),
			ExperienceId.PrimaryAssetType.ToString());

		const FString MapResolvedPath = MapId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(MapId).ToString()
			: FString();
		const FString ExperienceResolvedPath = ExperienceId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(ExperienceId).ToString()
			: FString();
		MonolithOnline::AddCheck(Checks, bOk, TEXT("map_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !MapResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			MapResolvedPath.IsEmpty() ? TEXT("MapID did not resolve through AssetManager") : MapResolvedPath);
		MonolithOnline::AddCheck(Checks, bOk, TEXT("experience_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !ExperienceResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			ExperienceResolvedPath.IsEmpty() ? TEXT("ExperienceID did not resolve through AssetManager") : ExperienceResolvedPath);

		int64 MaxPlayerCount = 0;
		MonolithOnline::TryReadIntProperty(UserFacingExperience, TEXT("MaxPlayerCount"), MaxPlayerCount);
		MonolithOnline::AddCheck(Checks, bOk, TEXT("max_player_count_positive"),
			MaxPlayerCount > 0,
			TEXT("error"),
			FString::FromInt(static_cast<int32>(MaxPlayerCount)));

		const FString SessionMode = MonolithOnline::NormalizeSessionModeToken(MonolithOnline::ExportPropertyValue(
			UserFacingExperience,
			UserFacingExperience->GetClass()->FindPropertyByName(TEXT("SessionMode"))));
		const bool bOnline = SessionMode.Equals(TEXT("Online"), ESearchCase::IgnoreCase);
		const bool bLan = SessionMode.Equals(TEXT("LAN"), ESearchCase::IgnoreCase);
		const bool bOffline = SessionMode.Equals(TEXT("Offline"), ESearchCase::IgnoreCase);
		MonolithOnline::AddCheck(Checks, bOk, TEXT("session_mode_known"),
			bOnline || bLan || bOffline,
			TEXT("error"),
			SessionMode);
		MonolithOnline::AddCheck(Checks, bOk, TEXT("session_mode_online_required"),
			!bRequireOnlineSession || bOnline,
			TEXT("error"),
			FString::Printf(TEXT("SessionMode=%s"), *SessionMode));

		bool bUseLobbies = false;
		bool bUseLobbiesVoiceChat = false;
		bool bUsePresence = false;
		bool bShowInFrontEnd = false;
		bool bRecordReplay = false;
		MonolithOnline::TryReadBoolProperty(UserFacingExperience, TEXT("bUseLobbies"), bUseLobbies);
		MonolithOnline::TryReadBoolProperty(UserFacingExperience, TEXT("bUseLobbiesVoiceChat"), bUseLobbiesVoiceChat);
		MonolithOnline::TryReadBoolProperty(UserFacingExperience, TEXT("bUsePresence"), bUsePresence);
		MonolithOnline::TryReadBoolProperty(UserFacingExperience, TEXT("bShowInFrontEnd"), bShowInFrontEnd);
		MonolithOnline::TryReadBoolProperty(UserFacingExperience, TEXT("bRecordReplay"), bRecordReplay);

		MonolithOnline::AddCheck(Checks, bOk, TEXT("frontend_visible_required"),
			!bRequireFrontendVisible || bShowInFrontEnd,
			TEXT("error"),
			FString::Printf(TEXT("bShowInFrontEnd=%s"), bShowInFrontEnd ? TEXT("true") : TEXT("false")));
		MonolithOnline::AddCheck(Checks, bOk, TEXT("online_lobbies_required"),
			!bRequireLobbiesForOnline || !bOnline || bUseLobbies,
			TEXT("error"),
			FString::Printf(TEXT("SessionMode=%s bUseLobbies=%s"), *SessionMode, bUseLobbies ? TEXT("true") : TEXT("false")));
		MonolithOnline::AddCheck(Checks, bOk, TEXT("lobby_voice_requires_lobbies"),
			!bUseLobbiesVoiceChat || bUseLobbies,
			TEXT("error"),
			FString::Printf(TEXT("bUseLobbies=%s bUseLobbiesVoiceChat=%s"), bUseLobbies ? TEXT("true") : TEXT("false"), bUseLobbiesVoiceChat ? TEXT("true") : TEXT("false")));
		MonolithOnline::AddCheck(Checks, bOk, TEXT("lan_offline_ignores_online_flags"),
			bOnline || (!bUseLobbies && !bUseLobbiesVoiceChat && !bUsePresence),
			TEXT("warning"),
			TEXT("LAN/offline hosting ignores lobby, voice, and presence flags; clear them for an unambiguous contract."));

		const bool bEffectiveUseLobbies = bOnline && bUseLobbies;
		const bool bEffectiveUseVoice = bEffectiveUseLobbies && bUseLobbiesVoiceChat;
		const bool bEffectiveUsePresence = bOnline && bUsePresence;
		TSharedPtr<FJsonObject> HostRequestProjection = MakeShared<FJsonObject>();
		HostRequestProjection->SetStringField(TEXT("online_mode"), SessionMode);
		HostRequestProjection->SetBoolField(TEXT("bUseLobbies"), bEffectiveUseLobbies);
		HostRequestProjection->SetBoolField(TEXT("bUseLobbiesVoiceChat"), bEffectiveUseVoice);
		HostRequestProjection->SetBoolField(TEXT("bUsePresence"), bEffectiveUsePresence);
		HostRequestProjection->SetObjectField(TEXT("map_id"), MonolithOnline::PrimaryAssetIdToJson(MapId));
		HostRequestProjection->SetNumberField(TEXT("max_player_count"), MaxPlayerCount);
		FString ModeNameForAdvertisement = FPackageName::GetLongPackageAssetName(ObjectPath);
		if (const UPrimaryDataAsset* PrimaryDataAsset = Cast<UPrimaryDataAsset>(UserFacingExperience))
		{
			const FPrimaryAssetId UserFacingId = PrimaryDataAsset->GetPrimaryAssetId();
			if (UserFacingId.IsValid())
			{
				ModeNameForAdvertisement = UserFacingId.PrimaryAssetName.ToString();
			}
			AssetReport->SetStringField(TEXT("primary_asset_id"), UserFacingId.ToString());
		}
		HostRequestProjection->SetStringField(TEXT("mode_name_for_advertisement"), ModeNameForAdvertisement);
		HostRequestProjection->SetStringField(TEXT("experience_extra_arg"), ExperienceId.PrimaryAssetName.ToString());
		HostRequestProjection->SetBoolField(TEXT("would_add_demo_rec_arg"), bRecordReplay);
		Result->SetObjectField(TEXT("host_request_projection"), HostRequestProjection);

		if (bRequireLobbySchema && bEffectiveUseLobbies)
		{
			const MonolithOnline::FLobbySchemaInfo LobbySchema = MonolithOnline::CollectLobbySchemaInfo();
			MonolithOnline::AddLobbySchemaChecks(LobbySchema, Checks, bOk, TEXT("error"));
			Result->SetArrayField(TEXT("lobby_schema_rows"), LobbySchema.Rows);
			Result->SetArrayField(TEXT("detected_lobby_attributes"), MonolithOnline::StringsToJson(LobbySchema.SeenAttributes.Array()));
		}

		if (!bOnline && (bUseLobbies || bUseLobbiesVoiceChat || bUsePresence))
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("UserFacingExperience has lobby/presence flags set on LAN/offline SessionMode; Lyra CreateHostingRequest masks them off.")));
		}
	}

	Result->SetObjectField(TEXT("user_facing_experience"), AssetReport);
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::ValidateCommonUserInitializationContract(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("validate_common_user_initialization_contract"));
	Result->SetBoolField(TEXT("redaction_applied"), true);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;

	const TArray<FString> Plugins = { TEXT("CommonUser"), TEXT("CommonGame") };
	TArray<TSharedPtr<FJsonValue>> PluginRows;
	for (const FString& PluginName : Plugins)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildPluginRow(PluginName);
		const bool bEnabled = Row->GetBoolField(TEXT("enabled"));
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("plugin_%s_enabled"), *PluginName),
			bEnabled,
			TEXT("error"),
			FString::Printf(TEXT("%s plugin should be enabled for Lyra CommonUser/CommonSession flow."), *PluginName));
		PluginRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("plugins"), PluginRows);

	const TArray<FString> Modules = { TEXT("CommonUser"), TEXT("CommonGame") };
	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	for (const FString& ModuleName : Modules)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildModuleRow(ModuleName);
		const bool bExists = Row->GetBoolField(TEXT("exists"));
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("module_%s_exists"), *ModuleName),
			bExists,
			TEXT("error"),
			FString::Printf(TEXT("%s module should exist."), *ModuleName));
		ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("modules"), ModuleRows);

	const TArray<FString> ClassPaths =
	{
		TEXT("/Script/CommonUser.CommonUserSubsystem"),
		TEXT("/Script/CommonUser.CommonSessionSubsystem"),
		TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"),
		TEXT("/Script/LyraGame.LyraLocalPlayer"),
		TEXT("/Script/LyraGame.LyraGameInstance")
	};
	TArray<TSharedPtr<FJsonValue>> ClassRows;
	for (const FString& ClassPath : ClassPaths)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildClassRow(ClassPath, true);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("class_%s_resolved"), *ClassPath.Replace(TEXT("/Script/"), TEXT(""))),
			Row->GetBoolField(TEXT("resolved")),
			TEXT("error"),
			FString::Printf(TEXT("Required reflected class %s should resolve."), *ClassPath));
		ClassRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("reflected_classes"), ClassRows);

	TArray<MonolithOnline::FConfigFieldSpec> Specs =
	{
		{ TEXT("/Script/Engine.Engine"), TEXT("LocalPlayerClassName"), false, true },
		{ TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), false, true },
		{ TEXT("OnlineServices"), TEXT("DefaultServices"), false, true }
	};
	const TArray<MonolithOnline::FConfigObservation> Observations = MonolithOnline::CollectObservations(Specs);
	TArray<TSharedPtr<FJsonValue>> FieldRows;
	for (const MonolithOnline::FConfigObservation& Observation : Observations)
	{
		FieldRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::ObservationToJson(Observation)));
	}
	Result->SetArrayField(TEXT("config_fields"), FieldRows);

	FString LocalPlayerClassName;
	FString GameInstanceClass;
	MonolithOnline::AddCheck(Checks, bOk, TEXT("lyra_local_player_configured"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/Engine.Engine"), TEXT("LocalPlayerClassName"), &LocalPlayerClassName)
			&& LocalPlayerClassName.Contains(TEXT("LyraLocalPlayer"), ESearchCase::IgnoreCase),
		TEXT("error"),
		TEXT("Engine LocalPlayerClassName should point at a Lyra local player class for CommonUser initialization."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("game_instance_configured"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), &GameInstanceClass),
		TEXT("warning"),
		TEXT("GameInstanceClass should be configured so front-end login/session flow initializes consistently."));

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::ValidateCommonUserPrivilegeMatrix(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("validate_common_user_privilege_matrix"));
	Result->SetBoolField(TEXT("redaction_applied"), true);
	Result->SetStringField(TEXT("mode"), TEXT("read_only_static_contract"));

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;

	const TArray<FString> Plugins = { TEXT("CommonUser"), TEXT("CommonGame") };
	TArray<TSharedPtr<FJsonValue>> PluginRows;
	for (const FString& PluginName : Plugins)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildPluginRow(PluginName);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("plugin_%s_enabled"), *PluginName),
			Row->GetBoolField(TEXT("enabled")),
			TEXT("error"),
			FString::Printf(TEXT("%s plugin should be enabled for CommonUser privilege handling."), *PluginName));
		PluginRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("plugins"), PluginRows);

	const TArray<FString> Modules = { TEXT("CommonUser"), TEXT("CommonGame") };
	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	for (const FString& ModuleName : Modules)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildModuleRow(ModuleName);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("module_%s_exists"), *ModuleName),
			Row->GetBoolField(TEXT("exists")),
			TEXT("error"),
			FString::Printf(TEXT("%s module should exist."), *ModuleName));
		ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("modules"), ModuleRows);

	const TArray<FString> ClassPaths =
	{
		TEXT("/Script/CommonUser.CommonUserSubsystem"),
		TEXT("/Script/CommonUser.CommonUserInfo"),
		TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"),
		TEXT("/Script/CommonGame.CommonGameInstance")
	};
	TArray<TSharedPtr<FJsonValue>> ClassRows;
	for (const FString& ClassPath : ClassPaths)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildClassRow(ClassPath, true);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("class_%s_resolved"), *ClassPath.Replace(TEXT("/Script/"), TEXT(""))),
			Row->GetBoolField(TEXT("resolved")),
			TEXT("error"),
			FString::Printf(TEXT("Reflected class %s should resolve."), *ClassPath));
		ClassRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("reflected_classes"), ClassRows);

	const TArray<FString> ExpectedPrivileges =
	{
		TEXT("CanPlay"),
		TEXT("CanPlayOnline"),
		TEXT("CanCommunicateViaTextOnline"),
		TEXT("CanCommunicateViaVoiceOnline"),
		TEXT("CanUseUserGeneratedContent"),
		TEXT("CanUseCrossPlay"),
		TEXT("Invalid_Count")
	};
	const TArray<FString> ExpectedContexts =
	{
		TEXT("Game"),
		TEXT("Default"),
		TEXT("Service"),
		TEXT("ServiceOrDefault"),
		TEXT("Platform"),
		TEXT("PlatformOrDefault"),
		TEXT("Invalid")
	};
	const TArray<FString> ExpectedPrivilegeResults =
	{
		TEXT("Unknown"),
		TEXT("Available"),
		TEXT("UserNotLoggedIn"),
		TEXT("LicenseInvalid"),
		TEXT("VersionOutdated"),
		TEXT("NetworkConnectionUnavailable"),
		TEXT("AgeRestricted"),
		TEXT("AccountTypeRestricted"),
		TEXT("AccountUseRestricted"),
		TEXT("PlatformFailure")
	};
	const TArray<FString> ExpectedAvailability =
	{
		TEXT("Unknown"),
		TEXT("NowAvailable"),
		TEXT("PossiblyAvailable"),
		TEXT("CurrentlyUnavailable"),
		TEXT("AlwaysUnavailable"),
		TEXT("Invalid")
	};
	const TArray<FString> ExpectedInitializationStates =
	{
		TEXT("Unknown"),
		TEXT("DoingInitialLogin"),
		TEXT("DoingNetworkLogin"),
		TEXT("FailedtoLogin"),
		TEXT("LoggedInOnline"),
		TEXT("LoggedInLocalOnly"),
		TEXT("Invalid")
	};

	bool bPrivilegeEnumOk = false;
	bool bContextEnumOk = false;
	bool bResultEnumOk = false;
	bool bAvailabilityEnumOk = false;
	bool bInitializationEnumOk = false;
	TArray<TSharedPtr<FJsonValue>> EnumRows;
	EnumRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildEnumReport(TEXT("/Script/CommonUser.ECommonUserPrivilege"), ExpectedPrivileges, &bPrivilegeEnumOk)));
	EnumRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildEnumReport(TEXT("/Script/CommonUser.ECommonUserOnlineContext"), ExpectedContexts, &bContextEnumOk)));
	EnumRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildEnumReport(TEXT("/Script/CommonUser.ECommonUserPrivilegeResult"), ExpectedPrivilegeResults, &bResultEnumOk)));
	EnumRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildEnumReport(TEXT("/Script/CommonUser.ECommonUserAvailability"), ExpectedAvailability, &bAvailabilityEnumOk)));
	EnumRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::BuildEnumReport(TEXT("/Script/CommonUser.ECommonUserInitializationState"), ExpectedInitializationStates, &bInitializationEnumOk)));
	Result->SetArrayField(TEXT("enums"), EnumRows);
	MonolithOnline::AddCheck(Checks, bOk, TEXT("enum_ECommonUserPrivilege_expected_values"), bPrivilegeEnumOk, TEXT("error"), TEXT("CommonUser privilege enum should expose all expected privilege values."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("enum_ECommonUserOnlineContext_expected_values"), bContextEnumOk, TEXT("error"), TEXT("CommonUser online context enum should expose all expected contexts."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("enum_ECommonUserPrivilegeResult_expected_values"), bResultEnumOk, TEXT("error"), TEXT("CommonUser privilege result enum should expose all expected result buckets."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("enum_ECommonUserAvailability_expected_values"), bAvailabilityEnumOk, TEXT("error"), TEXT("CommonUser availability enum should expose all expected availability buckets."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("enum_ECommonUserInitializationState_expected_values"), bInitializationEnumOk, TEXT("error"), TEXT("CommonUser initialization state enum should expose all expected login-flow states."));

	TArray<TSharedPtr<FJsonValue>> FunctionRows;
	const TArray<TPair<FString, FString>> ReflectedFunctions =
	{
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("TryToInitializeForLocalPlay")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("TryToLoginForOnlinePlay")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("TryToInitializeUser")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("CancelUserInitialization")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("ResetUserState")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"), TEXT("InitializeForLocalPlay")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"), TEXT("LoginForOnlinePlay")),
		TPair<FString, FString>(TEXT("/Script/CommonUser.AsyncAction_CommonUserInitialize"), TEXT("HandleInitializationComplete")),
		TPair<FString, FString>(TEXT("/Script/CommonGame.CommonGameInstance"), TEXT("HandlePrivilegeChanged"))
	};
	for (const TPair<FString, FString>& FunctionSpec : ReflectedFunctions)
	{
		TSharedPtr<FJsonObject> Row = MonolithOnline::BuildFunctionRow(FunctionSpec.Key, FunctionSpec.Value);
		MonolithOnline::AddCheck(Checks, bOk, FString::Printf(TEXT("function_%s_reflected"), *FunctionSpec.Value),
			Row->GetBoolField(TEXT("reflected")),
			TEXT("error"),
			FString::Printf(TEXT("%s::%s should be reflected for Blueprint/CommonGame flow diagnostics."), *FunctionSpec.Key, *FunctionSpec.Value));
		FunctionRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("reflected_functions"), FunctionRows);

	TArray<TSharedPtr<FJsonValue>> PrivilegeRows;
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanPlay"), TEXT("EUserPrivileges::CanPlay"), TEXT("UE::Online::EUserPrivileges::CanPlay"), TEXT("Base permission to play the game."))));
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanPlayOnline"), TEXT("EUserPrivileges::CanPlayOnline"), TEXT("UE::Online::EUserPrivileges::CanPlayOnline"), TEXT("Permission to enter online modes."))));
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanCommunicateViaTextOnline"), TEXT("EUserPrivileges::CanCommunicateOnline"), TEXT("UE::Online::EUserPrivileges::CanCommunicateViaTextOnline"), TEXT("Permission to use text chat."), TEXT("OSSv1 collapses text and voice into CanCommunicateOnline."))));
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanCommunicateViaVoiceOnline"), TEXT("EUserPrivileges::CanCommunicateOnline"), TEXT("UE::Online::EUserPrivileges::CanCommunicateViaVoiceOnline"), TEXT("Permission to use voice chat."), TEXT("OSSv1 collapses text and voice into CanCommunicateOnline."))));
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanUseUserGeneratedContent"), TEXT("EUserPrivileges::CanUseUserGeneratedContent"), TEXT("UE::Online::EUserPrivileges::CanUseUserGeneratedContent"), TEXT("Permission to access user-generated content."))));
	PrivilegeRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeMappingRow(TEXT("CanUseCrossPlay"), TEXT("EUserPrivileges::CanUserCrossPlay"), TEXT("UE::Online::EUserPrivileges::CanCrossPlay"), TEXT("Permission to participate in cross-platform play."))));
	Result->SetArrayField(TEXT("privilege_matrix"), PrivilegeRows);

	TArray<TSharedPtr<FJsonValue>> ResultRows;
	for (const FString& ResultName : ExpectedPrivilegeResults)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("result"), ResultName);
		if (ResultName.Equals(TEXT("Available"), ESearchCase::IgnoreCase))
		{
			Row->SetStringField(TEXT("availability_effect"), TEXT("eligible_for_now_available"));
		}
		else if (ResultName.Equals(TEXT("Unknown"), ESearchCase::IgnoreCase))
		{
			Row->SetStringField(TEXT("availability_effect"), TEXT("requires_query"));
		}
		else if (ResultName.Equals(TEXT("VersionOutdated"), ESearchCase::IgnoreCase)
			|| ResultName.Equals(TEXT("NetworkConnectionUnavailable"), ESearchCase::IgnoreCase))
		{
			Row->SetStringField(TEXT("availability_effect"), TEXT("currently_unavailable"));
		}
		else
		{
			Row->SetStringField(TEXT("availability_effect"), TEXT("always_unavailable_or_platform_failure"));
		}
		ResultRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("result_buckets"), ResultRows);

	TArray<TSharedPtr<FJsonValue>> LoginFlowRows;
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("local_play_initialize"), TEXT("UAsyncAction_CommonUserInitialize::InitializeForLocalPlay"), TEXT("Requests ECommonUserPrivilege::CanPlay, allows optional guest login, and can create a new local player."))));
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("online_play_login"), TEXT("UAsyncAction_CommonUserInitialize::LoginForOnlinePlay"), TEXT("Requests ECommonUserPrivilege::CanPlayOnline for an existing local player and does not create a new local player."))));
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("login_state_machine"), TEXT("UCommonUserSubsystem::ProcessLoginRequest"), TEXT("Runs platform auth transfer, auto-login, login UI, and privilege query states until the requested privilege is available or fails."))));
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("ossv1_privilege_query"), TEXT("UCommonUserSubsystem::QueryUserPrivilegeOSSv1"), TEXT("Uses IOnlineIdentity::GetUserPrivilege after converting ECommonUserPrivilege to OSSv1 EUserPrivileges."))));
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("ossv2_privilege_query"), TEXT("UCommonUserSubsystem::QueryUserPrivilegeOSSv2"), TEXT("Uses OnlineServices IPrivileges::QueryUserPrivilege when available; if no privilege interface exists, CommonUser marks the requested privilege available."))));
	LoginFlowRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::MakeFlowStep(TEXT("common_game_reaction"), TEXT("UCommonGameInstance::HandlePrivilegeChanged"), TEXT("CommonGame handles loss of CanPlay for the primary player as a severe privilege change."))));
	Result->SetArrayField(TEXT("login_flow_steps"), LoginFlowRows);

	TArray<MonolithOnline::FConfigFieldSpec> Specs =
	{
		{ TEXT("/Script/Engine.Engine"), TEXT("LocalPlayerClassName"), false, true },
		{ TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), false, true },
		{ TEXT("OnlineServices"), TEXT("DefaultServices"), false, true },
		{ TEXT("OnlineServices"), TEXT("PlatformServices"), false, true },
		{ TEXT("OnlineServices.EOS"), TEXT("bUseEAS"), false, true }
	};
	const TArray<MonolithOnline::FConfigObservation> Observations = MonolithOnline::CollectObservations(Specs);
	TArray<TSharedPtr<FJsonValue>> FieldRows;
	for (const MonolithOnline::FConfigObservation& Observation : Observations)
	{
		FieldRows.Add(MakeShared<FJsonValueObject>(MonolithOnline::ObservationToJson(Observation)));
	}
	Result->SetArrayField(TEXT("config_fields"), FieldRows);

	FString LocalPlayerClassName;
	FString GameInstanceClass;
	MonolithOnline::AddCheck(Checks, bOk, TEXT("lyra_local_player_configured"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/Engine.Engine"), TEXT("LocalPlayerClassName"), &LocalPlayerClassName)
			&& LocalPlayerClassName.Contains(TEXT("LyraLocalPlayer"), ESearchCase::IgnoreCase),
		TEXT("error"),
		TEXT("Engine LocalPlayerClassName should point at a Lyra local player class for CommonUser initialization."));
	MonolithOnline::AddCheck(Checks, bOk, TEXT("common_game_instance_configured"),
		MonolithOnline::HasValue(Observations, TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), &GameInstanceClass)
			&& GameInstanceClass.Contains(TEXT("GameInstance"), ESearchCase::IgnoreCase),
		TEXT("warning"),
		TEXT("GameInstanceClass should be configured so CommonGame privilege handling can attach to CommonUser delegates."));

	TArray<TSharedPtr<FJsonValue>> Limitations;
	Limitations.Add(MakeShared<FJsonValueString>(TEXT("This action does not run login, show external UI, call EOS, or query live privileges.")));
	Limitations.Add(MakeShared<FJsonValueString>(TEXT("Cross-play availability depends on platform/EOS policy and the live user's account state; this action validates the local CommonUser mapping contract only.")));
	Limitations.Add(MakeShared<FJsonValueString>(TEXT("OSSv1 maps text and voice communication to the same CanCommunicateOnline privilege; OSSv2 keeps them separate.")));
	Result->SetArrayField(TEXT("limitations"), Limitations);

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithOnlineActions::DiagnoseEOSAccountPortalLogs(const TSharedPtr<FJsonObject>& Params)
{
	FString LogRoot;
	FString Error;
	if (!MonolithOnline::ReadOptionalStringParam(Params, TEXT("log_root"), LogRoot, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("log_root"), Error));
	}

	int32 MaxResults = 50;
	int32 SinceDays = 14;
	if (!MonolithOnline::ReadIntParam(Params, TEXT("max_results"), MaxResults, Error)
		|| !MonolithOnline::ReadIntParam(Params, TEXT("since_days"), SinceDays, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("logs"), Error));
	}
	if (MaxResults < 1 || MaxResults > 500)
	{
		Error = TEXT("Param 'max_results' must be between 1 and 500");
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("max_results"), Error));
	}
	if (SinceDays < 0)
	{
		Error = TEXT("Param 'since_days' must be >= 0");
		return FMonolithActionResult::Error(Error, MonolithOnline::ErrInvalidParams)
			.WithErrorData(MonolithOnline::ErrorData(TEXT("since_days"), Error));
	}

	if (LogRoot.IsEmpty())
	{
		LogRoot = FPaths::ProjectSavedDir() / TEXT("Logs");
	}
	if (FPaths::IsRelative(LogRoot))
	{
		LogRoot = FPaths::ProjectDir() / LogRoot;
	}
	LogRoot = FPaths::ConvertRelativePathToFull(LogRoot);
	LogRoot.ReplaceInline(TEXT("\\"), TEXT("/"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("online"));
	Result->SetStringField(TEXT("action"), TEXT("diagnose_eos_accountportal_logs"));
	Result->SetStringField(TEXT("log_root"), MonolithOnline::ToProjectRelativePath(LogRoot));
	Result->SetBoolField(TEXT("redaction_applied"), true);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 FilesScanned = 0;
	int32 LinesScanned = 0;
	bool bTruncated = false;

	if (IFileManager::Get().DirectoryExists(*LogRoot))
	{
		TArray<FString> LogFiles;
		IFileManager::Get().FindFilesRecursive(LogFiles, *LogRoot, TEXT("*.log"), true, false);
		LogFiles.Sort();
		const FDateTime Cutoff = SinceDays > 0 ? FDateTime::UtcNow() - FTimespan::FromDays(SinceDays) : FDateTime::MinValue();

		for (const FString& LogFile : LogFiles)
		{
			const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*LogFile);
			if (SinceDays > 0 && Timestamp != FDateTime::MinValue() && Timestamp < Cutoff)
			{
				continue;
			}

			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *LogFile))
			{
				continue;
			}
			++FilesScanned;
			for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
			{
				++LinesScanned;
				const FString& Line = Lines[LineIndex];
				if (!MonolithOnline::IsInterestingEOSLogLine(Line))
				{
					continue;
				}
				if (Rows.Num() >= MaxResults)
				{
					bTruncated = true;
					break;
				}

				FString Trimmed = Line;
				Trimmed.TrimStartAndEndInline();

				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("file"), MonolithOnline::ToProjectRelativePath(LogFile));
				Row->SetNumberField(TEXT("line"), LineIndex + 1);
				Row->SetStringField(TEXT("category"), MonolithOnline::ClassifyLogLine(Line));
				Row->SetStringField(TEXT("message"), MonolithOnline::RedactLogLine(Trimmed));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			if (bTruncated)
			{
				break;
			}
		}
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("log_root_exists"), IFileManager::Get().DirectoryExists(*LogRoot));
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetNumberField(TEXT("files_scanned"), FilesScanned);
	Result->SetNumberField(TEXT("lines_scanned"), LinesScanned);
	Result->SetNumberField(TEXT("match_count"), Rows.Num());
	Result->SetArrayField(TEXT("matches"), Rows);
	return FMonolithActionResult::Success(Result);
}
