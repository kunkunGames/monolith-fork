#include "MonolithGameplayMessageActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace MonolithGameplayMessage
{
	static constexpr int32 ErrInvalidParams = -32602;

	struct FTracePatternSpec
	{
		const TCHAR* Code;
		const TCHAR* Token;
		const TCHAR* Role;
		const TCHAR* Meaning;
	};

	static const FTracePatternSpec TracePatterns[] =
	{
		{ TEXT("broadcast_template"), TEXT("BroadcastMessage<"), TEXT("broadcaster"), TEXT("Templated native broadcast call; template argument is the payload candidate.") },
		{ TEXT("broadcast_call"), TEXT("BroadcastMessage("), TEXT("broadcaster"), TEXT("Native or reflected broadcast call; payload type may be implied by the message expression.") },
		{ TEXT("broadcast_blueprint"), TEXT("K2_BroadcastMessage("), TEXT("broadcaster"), TEXT("Blueprint-facing broadcast call site.") },
		{ TEXT("register_listener_template"), TEXT("RegisterListener<"), TEXT("listener"), TEXT("Templated native listener registration; template argument is the payload candidate.") },
		{ TEXT("register_listener_call"), TEXT("RegisterListener("), TEXT("listener"), TEXT("Native listener registration; payload type may be implied by callback signature.") },
		{ TEXT("async_listener"), TEXT("ListenForGameplayMessages("), TEXT("listener"), TEXT("Blueprint async listener registration call site.") }
	};

	struct FChannelTraceRow
	{
		FString Role;
		FString Code;
		FString Channel;
		FString Payload;
		FString MatchType;
		FString File;
		int32 Line = 0;
		FString FunctionContext;
		FString LineText;
	};

	static FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static UObject* LoadAnyObjectPath(const FString& ObjectPathValue)
	{
		if (ObjectPathValue.IsEmpty())
		{
			return nullptr;
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPathValue, nullptr, LOAD_NoWarn);
	}

	static UClass* LoadClassPath(const FString& ClassPath)
	{
		return Cast<UClass>(LoadAnyObjectPath(ClassPath));
	}

	static UScriptStruct* LoadStructPath(const FString& StructPath)
	{
		return Cast<UScriptStruct>(LoadAnyObjectPath(StructPath));
	}

	static UEnum* LoadEnumPath(const FString& EnumPath)
	{
		return Cast<UEnum>(LoadAnyObjectPath(EnumPath));
	}

	static TSharedPtr<FJsonObject> PluginStatus(const TCHAR* PluginName)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), PluginName);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
		if (Plugin.IsValid())
		{
			Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
			Obj->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
			Obj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
			Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> ModuleStatus(const TCHAR* ModuleName)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), ModuleName);
		const FName Name(ModuleName);
		Obj->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
		Obj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(Name));
		return Obj;
	}

	static TSharedPtr<FJsonObject> ClassSummary(const FString& RequestedPath, UClass* Class, UClass* ExpectedBaseClass)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("requested_class_path"), RequestedPath);
		Obj->SetBoolField(TEXT("found"), Class != nullptr);
		Obj->SetStringField(TEXT("class_path"), ObjectPath(Class));
		Obj->SetStringField(TEXT("expected_base_class_path"), ObjectPath(ExpectedBaseClass));
		Obj->SetBoolField(TEXT("child_of_expected_base"), Class && ExpectedBaseClass && Class->IsChildOf(ExpectedBaseClass));
		Obj->SetBoolField(TEXT("abstract"), Class && Class->HasAnyClassFlags(CLASS_Abstract));
		Obj->SetBoolField(TEXT("deprecated"), Class && Class->HasAnyClassFlags(CLASS_Deprecated));
		return Obj;
	}

	static TSharedPtr<FJsonObject> StructSummary(const FString& RequestedPath, UScriptStruct* Struct, UObject* LoadedObject)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("requested_struct_path"), RequestedPath);
		Obj->SetBoolField(TEXT("object_found"), LoadedObject != nullptr);
		Obj->SetStringField(TEXT("object_path"), ObjectPath(LoadedObject));
		Obj->SetStringField(TEXT("object_class_path"), LoadedObject && LoadedObject->GetClass() ? LoadedObject->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("found"), Struct != nullptr);
		Obj->SetStringField(TEXT("struct_path"), ObjectPath(Struct));
		Obj->SetBoolField(TEXT("blueprint_type"), Struct && Struct->HasMetaData(TEXT("BlueprintType")));
		Obj->SetBoolField(TEXT("deprecated"), Struct && Struct->HasMetaData(TEXT("Deprecated")));

		int32 PropertyCount = 0;
		int32 ObjectPropertyCount = 0;
		if (Struct)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				++PropertyCount;
				if (CastField<FObjectPropertyBase>(*It))
				{
					++ObjectPropertyCount;
				}
			}
		}
		Obj->SetNumberField(TEXT("property_count"), PropertyCount);
		Obj->SetNumberField(TEXT("object_property_count"), ObjectPropertyCount);
		return Obj;
	}

	static TSharedPtr<FJsonObject> FunctionSummary(UClass* Class, const TCHAR* FunctionName)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), FunctionName);
		UFunction* Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
		Obj->SetBoolField(TEXT("found"), Function != nullptr);
		Obj->SetStringField(TEXT("function_path"), ObjectPath(Function));
		Obj->SetBoolField(TEXT("blueprint_callable"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		Obj->SetBoolField(TEXT("custom_thunk"), Function && Function->HasMetaData(TEXT("CustomThunk")));
		return Obj;
	}

	static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, bool& bOk, const TCHAR* Name, bool bCheckOk, const TCHAR* Severity, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetBoolField(TEXT("ok"), bCheckOk);
		Obj->SetStringField(TEXT("severity"), Severity);
		Obj->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(Obj));

		if (!bCheckOk && FCString::Stricmp(Severity, TEXT("error")) == 0)
		{
			bOk = false;
		}
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Severity, const TCHAR* Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("severity"), Severity);
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Obj));
	}

	static bool ReadRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}
		if (!Params->TryGetStringField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		if (OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Param '%s' must not be empty"), FieldName);
			return false;
		}
		return true;
	}

	static FString ReadOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue = FString())
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		FString Value;
		if (!Params->TryGetStringField(FieldName, Value))
		{
			return DefaultValue;
		}
		Value.TrimStartAndEndInline();
		return Value;
	}

	static bool ReadOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
	{
		if (!Params.IsValid())
		{
			return DefaultValue;
		}
		bool Value = DefaultValue;
		return Params->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
	}

	static int32 ReadOptionalIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		double Number = static_cast<double>(DefaultValue);
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			return DefaultValue;
		}
		return FMath::Clamp(static_cast<int32>(Number), MinValue, MaxValue);
	}

	static bool ReadOptionalStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues, FString& OutError)
	{
		OutValues.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
				return false;
			}
			Text.TrimStartAndEndInline();
			if (!Text.IsEmpty())
			{
				OutValues.Add(Text);
			}
		}
		return true;
	}

	static FString ResolveSourceRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		if (Root.IsEmpty())
		{
			return FString();
		}
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (FPaths::IsRelative(Root))
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Root);
		}
		else
		{
			Root = FPaths::ConvertRelativePathToFull(Root);
		}
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	static void AddOptionalSourceRoot(const FString& Root, TArray<FString>& Roots)
	{
		const FString ResolvedRoot = ResolveSourceRoot(Root);
		if (!ResolvedRoot.IsEmpty() && FPaths::DirectoryExists(ResolvedRoot))
		{
			Roots.AddUnique(ResolvedRoot);
		}
	}

	static bool IsSkippedSourcePath(const FString& Path, bool bIncludeMonolithSource)
	{
		FString Normalized = Path;
		FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/Intermediate/")) ||
			Normalized.Contains(TEXT("/Binaries/")) ||
			Normalized.Contains(TEXT("/DerivedDataCache/")))
		{
			return true;
		}
		return !bIncludeMonolithSource && Normalized.Contains(TEXT("/Plugins/Monolith/"));
	}

	static TArray<FString> DefaultProjectSourceRoots(bool bIncludeMonolithSource)
	{
		TArray<FString> Roots;
		AddOptionalSourceRoot(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")), Roots);

		TArray<FString> PluginDirs;
		IFileManager::Get().FindFiles(PluginDirs, *FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("*")), false, true);
		for (const FString& PluginDirName : PluginDirs)
		{
			const FString SourceRoot = FPaths::Combine(FPaths::ProjectPluginsDir(), PluginDirName, TEXT("Source"));
			if (!IsSkippedSourcePath(SourceRoot, bIncludeMonolithSource))
			{
				AddOptionalSourceRoot(SourceRoot, Roots);
			}
		}
		return Roots;
	}

	static void AddEngineGameplayMessageSourceRoots(TArray<FString>& Roots)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GameplayMessageRouter"));
		if (Plugin.IsValid())
		{
			AddOptionalSourceRoot(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source")), Roots);
		}
	}

	static bool HasSourceExtension(const FString& File)
	{
		const FString Extension = FPaths::GetExtension(File, false).ToLower();
		return Extension == TEXT("cpp") || Extension == TEXT("h") || Extension == TEXT("hpp") || Extension == TEXT("inl");
	}

	static void CollectSourceFiles(
		const TArray<FString>& Roots,
		bool bIncludeMonolithSource,
		int32 MaxFiles,
		TArray<FString>& OutFiles,
		TArray<TSharedPtr<FJsonValue>>& RootRows)
	{
		OutFiles.Reset();
		for (const FString& Root : Roots)
		{
			TSharedPtr<FJsonObject> RootRow = MakeShared<FJsonObject>();
			RootRow->SetStringField(TEXT("root"), Root);
			RootRow->SetBoolField(TEXT("exists"), FPaths::DirectoryExists(Root));

			int32 RootFileCount = 0;
			if (FPaths::DirectoryExists(Root))
			{
				TArray<FString> Files;
				IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.*"), true, false, false);
				for (const FString& File : Files)
				{
					if (OutFiles.Num() >= MaxFiles)
					{
						break;
					}
					if (!HasSourceExtension(File) || IsSkippedSourcePath(File, bIncludeMonolithSource))
					{
						continue;
					}
					OutFiles.Add(File);
					++RootFileCount;
				}
			}

			RootRow->SetNumberField(TEXT("source_file_count"), RootFileCount);
			RootRows.Add(MakeShared<FJsonValueObject>(RootRow));
			if (OutFiles.Num() >= MaxFiles)
			{
				break;
			}
		}
	}

	static FString TrimForJson(FString Value, int32 MaxChars)
	{
		Value.TrimStartAndEndInline();
		Value.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		return Value.Len() > MaxChars ? Value.Left(MaxChars) + TEXT("...") : Value;
	}

	static bool IsCommentOnlyLine(FString Line)
	{
		Line.TrimStartInline();
		return Line.StartsWith(TEXT("//")) || Line.StartsWith(TEXT("/*")) || Line.StartsWith(TEXT("*"));
	}

	static FString RelativeDisplayPath(const FString& File)
	{
		FString Display = File;
		if (FPaths::MakePathRelativeTo(Display, *FPaths::ProjectDir()))
		{
			FPaths::NormalizeFilename(Display);
			return Display;
		}
		FPaths::NormalizeFilename(Display);
		return Display;
	}

	static FString InferFunctionContext(const TArray<FString>& Lines, int32 LineIndex)
	{
		for (int32 Index = LineIndex; Index >= FMath::Max(0, LineIndex - 50); --Index)
		{
			FString Candidate = Lines[Index];
			Candidate.TrimStartAndEndInline();
			if (Candidate.StartsWith(TEXT("//")) || Candidate.StartsWith(TEXT("*")) || Candidate.StartsWith(TEXT("/*")))
			{
				continue;
			}
			if (Candidate.Contains(TEXT("::")) && Candidate.Contains(TEXT("(")))
			{
				return TrimForJson(Candidate, 240);
			}
		}
		return FString();
	}

	static FString ExtractTemplateArgumentNearToken(const FString& Line, const FString& Token)
	{
		const int32 TokenIndex = Line.Find(Token);
		if (TokenIndex == INDEX_NONE)
		{
			return FString();
		}

		const int32 OpenIndex = Line.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TokenIndex);
		const int32 CloseIndex = Line.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenIndex + 1);
		const int32 ParenIndex = Line.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, TokenIndex);
		if (OpenIndex == INDEX_NONE || CloseIndex == INDEX_NONE || (ParenIndex != INDEX_NONE && OpenIndex > ParenIndex))
		{
			return FString();
		}

		FString Payload = Line.Mid(OpenIndex + 1, CloseIndex - OpenIndex - 1);
		Payload.TrimStartAndEndInline();
		return Payload;
	}

	static TArray<FString> ExtractQuotedGameplayTagCandidates(const FString& Line)
	{
		TArray<FString> Tags;
		int32 SearchIndex = 0;
		while (SearchIndex < Line.Len())
		{
			int32 QuoteIndex = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
			if (QuoteIndex == INDEX_NONE)
			{
				break;
			}
			const int32 EndQuoteIndex = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteIndex + 1);
			if (EndQuoteIndex == INDEX_NONE)
			{
				break;
			}

			FString Candidate = Line.Mid(QuoteIndex + 1, EndQuoteIndex - QuoteIndex - 1);
			Candidate.TrimStartAndEndInline();
			const bool bLooksLikeTag = Candidate.Contains(TEXT("."))
				&& !Candidate.Contains(TEXT("/"))
				&& !Candidate.Contains(TEXT(" "))
				&& !Candidate.Contains(TEXT("("))
				&& !Candidate.Contains(TEXT(")"));
			if (bLooksLikeTag)
			{
				Tags.AddUnique(Candidate);
			}
			SearchIndex = EndQuoteIndex + 1;
		}
		return Tags;
	}

	static TArray<FString> ExtractGameplayTagConstants(const FString& Line)
	{
		TArray<FString> Constants;
		for (const FString& Prefix : { FString(TEXT("TAG_")), FString(TEXT("GameplayTag_")), FString(TEXT("NAME_")) })
		{
			int32 SearchIndex = 0;
			while (true)
			{
				const int32 FoundIndex = Line.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
				if (FoundIndex == INDEX_NONE)
				{
					break;
				}
				int32 EndIndex = FoundIndex;
				while (EndIndex < Line.Len() && (FChar::IsAlnum(Line[EndIndex]) || Line[EndIndex] == TEXT('_')))
				{
					++EndIndex;
				}
				Constants.AddUnique(Line.Mid(FoundIndex, EndIndex - FoundIndex));
				SearchIndex = EndIndex;
			}
		}
		return Constants;
	}

	static FString ExtractMatchTypeFromLine(const FString& Line)
	{
		if (Line.Contains(TEXT("PartialMatch")) || Line.Contains(TEXT("EGameplayMessageMatch::Partial")))
		{
			return TEXT("PartialMatch");
		}
		if (Line.Contains(TEXT("ExactMatch")) || Line.Contains(TEXT("EGameplayMessageMatch::Exact")))
		{
			return TEXT("ExactMatch");
		}
		return FString();
	}

	static FString ExtractStaticStructPayloadCandidate(const FString& Line)
	{
		const int32 StaticStructIndex = Line.Find(TEXT("::StaticStruct"));
		if (StaticStructIndex == INDEX_NONE)
		{
			return FString();
		}

		int32 StartIndex = StaticStructIndex - 1;
		while (StartIndex >= 0)
		{
			const TCHAR Ch = Line[StartIndex];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT(':')))
			{
				break;
			}
			--StartIndex;
		}

		FString Candidate = Line.Mid(StartIndex + 1, StaticStructIndex - StartIndex - 1);
		Candidate.TrimStartAndEndInline();
		return Candidate;
	}

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

	static TSharedPtr<FJsonObject> TraceRowToJson(const FChannelTraceRow& Row, bool bIncludeLineText)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("role"), Row.Role);
		Obj->SetStringField(TEXT("code"), Row.Code);
		Obj->SetStringField(TEXT("channel"), Row.Channel);
		Obj->SetStringField(TEXT("payload_candidate"), Row.Payload);
		Obj->SetStringField(TEXT("match_type"), Row.MatchType);
		Obj->SetStringField(TEXT("file"), Row.File);
		Obj->SetNumberField(TEXT("line"), Row.Line);
		Obj->SetStringField(TEXT("function_context"), Row.FunctionContext);
		if (bIncludeLineText)
		{
			Obj->SetStringField(TEXT("line_text"), Row.LineText);
		}
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> TracePatternRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(UE_ARRAY_COUNT(TracePatterns));
		for (const FTracePatternSpec& Pattern : TracePatterns)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("code"), Pattern.Code);
			Row->SetStringField(TEXT("token"), Pattern.Token);
			Row->SetStringField(TEXT("role"), Pattern.Role);
			Row->SetStringField(TEXT("meaning"), Pattern.Meaning);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	static TArray<TSharedPtr<FJsonValue>> TraceLimitationRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(4);
		for (const TCHAR* Limitation : {
			TEXT("Static source trace only; no PIE session, listener registration, broadcast execution, GameFeature activation, or live subsystem mutation is performed."),
			TEXT("Lexical matches identify candidate broadcaster/listener call sites and nearby tag/payload tokens, but they do not prove runtime reachability or branch coverage."),
			TEXT("Channel and payload extraction is best-effort for single-line C++/Blueprint-facing call sites; multi-line fluent calls may report unresolved candidates."),
			TEXT("Payload compatibility issues are reported only when channel and payload candidates can be inferred from source text.")
		})
		{
			Rows.Add(MakeShared<FJsonValueString>(Limitation));
		}
		return Rows;
	}

	static bool NormalizeMatchType(const FString& RawValue, FString& OutMatchType)
	{
		FString Value = RawValue;
		Value.TrimStartAndEndInline();
		if (Value.IsEmpty())
		{
			OutMatchType = TEXT("ExactMatch");
			return true;
		}
		Value.RemoveFromStart(TEXT("EGameplayMessageMatch::"));
		if (Value.Equals(TEXT("Exact"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("ExactMatch"), ESearchCase::IgnoreCase))
		{
			OutMatchType = TEXT("ExactMatch");
			return true;
		}
		if (Value.Equals(TEXT("Partial"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("PartialMatch"), ESearchCase::IgnoreCase))
		{
			OutMatchType = TEXT("PartialMatch");
			return true;
		}
		OutMatchType.Reset();
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> ListenerContractRows()
	{
		const TCHAR* Rows[] =
		{
			TEXT("UGameplayMessageSubsystem is a UGameInstanceSubsystem; route access through a world or game instance."),
			TEXT("BroadcastMessage and RegisterListener must agree on the exact same UScriptStruct payload type for a channel."),
			TEXT("ExactMatch receives only the exact channel; PartialMatch receives the root channel and child channels."),
			TEXT("Listener handles should be unregistered when the receiver lifetime ends."),
			TEXT("Blueprint async listeners use UAsyncAction_ListenForGameplayMessage plus GetPayload custom thunk.")
		};

		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(UE_ARRAY_COUNT(Rows));
		for (const TCHAR* Row : Rows)
		{
			Values.Add(MakeShared<FJsonValueString>(Row));
		}
		return Values;
	}

	static TSharedPtr<FJsonObject> ValidateMessageStructInternal(
		const FString& StructPath,
		bool bRequireBlueprintType,
		bool bRequireNoObjectReferences,
		TArray<TSharedPtr<FJsonValue>>& Checks,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		bool& bOk)
	{
		UObject* LoadedObject = LoadAnyObjectPath(StructPath);
		UScriptStruct* Struct = Cast<UScriptStruct>(LoadedObject);
		TSharedPtr<FJsonObject> Summary = StructSummary(StructPath, Struct, LoadedObject);

		int32 ObjectPropertyCount = 0;
		if (Struct)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (CastField<FObjectPropertyBase>(*It))
				{
					++ObjectPropertyCount;
				}
			}
		}

		AddCheck(Checks, bOk, TEXT("message_struct_loaded"), LoadedObject != nullptr, TEXT("error"), StructPath);
		AddCheck(Checks, bOk, TEXT("message_struct_is_uscriptstruct"), Struct != nullptr, TEXT("error"), ObjectPath(LoadedObject));
		AddCheck(Checks, bOk, TEXT("message_struct_not_deprecated"), Struct && !Struct->HasMetaData(TEXT("Deprecated")), TEXT("error"), ObjectPath(Struct));
		AddCheck(
			Checks,
			bOk,
			TEXT("message_struct_blueprint_type"),
			!bRequireBlueprintType || (Struct && Struct->HasMetaData(TEXT("BlueprintType"))),
			bRequireBlueprintType ? TEXT("error") : TEXT("info"),
			Struct && Struct->HasMetaData(TEXT("BlueprintType")) ? TEXT("BlueprintType") : TEXT("not marked BlueprintType"));
		AddCheck(
			Checks,
			bOk,
			TEXT("message_struct_no_object_references"),
			!bRequireNoObjectReferences || ObjectPropertyCount == 0,
			bRequireNoObjectReferences ? TEXT("error") : TEXT("info"),
			FString::Printf(TEXT("object_property_count=%d"), ObjectPropertyCount));

		if (!LoadedObject)
		{
			AddIssue(Issues, TEXT("error"), TEXT("message_struct_not_found"), FString::Printf(TEXT("Object '%s' could not be loaded."), *StructPath));
		}
		else if (!Struct)
		{
			AddIssue(Issues, TEXT("error"), TEXT("message_struct_wrong_object_type"), FString::Printf(TEXT("Object '%s' is a %s, not a UScriptStruct."), *LoadedObject->GetPathName(), *LoadedObject->GetClass()->GetPathName()));
		}
		else if (Struct->HasMetaData(TEXT("Deprecated")))
		{
			AddIssue(Issues, TEXT("error"), TEXT("message_struct_deprecated"), FString::Printf(TEXT("Struct '%s' is deprecated."), *Struct->GetPathName()));
		}
		if (bRequireBlueprintType && Struct && !Struct->HasMetaData(TEXT("BlueprintType")))
		{
			AddIssue(Issues, TEXT("error"), TEXT("message_struct_not_blueprint_type"), FString::Printf(TEXT("Struct '%s' is not marked BlueprintType."), *Struct->GetPathName()));
		}
		if (bRequireNoObjectReferences && ObjectPropertyCount > 0)
		{
			AddIssue(Issues, TEXT("error"), TEXT("message_struct_has_object_references"), FString::Printf(TEXT("Struct '%s' has %d object reference properties."), *Struct->GetPathName(), ObjectPropertyCount));
		}

		return Summary;
	}
}

void FMonolithGameplayMessageActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("gameplay_message"), TEXT("get_status"),
		TEXT("Report GameplayMessageRouter plugin/module/class availability without hard-linking GameplayMessageRuntime."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"));

	Registry.RegisterAction(
		TEXT("gameplay_message"), TEXT("describe_listener_contract"),
		TEXT("Describe GameplayMessageRouter listener/broadcast payload and match-type contract."),
		FMonolithActionHandler::CreateStatic(&DescribeListenerContract),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"));

	Registry.RegisterAction(
		TEXT("gameplay_message"), TEXT("validate_message_struct"),
		TEXT("Validate a GameplayMessageRouter payload UScriptStruct without loading or mutating assets."),
		FMonolithActionHandler::CreateStatic(&ValidateMessageStruct),
		FParamSchemaBuilder()
			.Required(TEXT("message_struct"), TEXT("string"), TEXT("Payload UScriptStruct path."))
			.Optional(TEXT("require_blueprint_type"), TEXT("boolean"), TEXT("Require BlueprintType metadata on the payload struct."), TEXT("false"))
			.Optional(TEXT("require_no_object_references"), TEXT("boolean"), TEXT("Treat UObject reference properties as an error for payload structs."), TEXT("false"))
			.Build(),
		TEXT("Validation"));

	Registry.RegisterAction(
		TEXT("gameplay_message"), TEXT("validate_channel_contract"),
		TEXT("Validate a gameplay message channel tag, match type, and optional payload struct contract read-only."),
		FMonolithActionHandler::CreateStatic(&ValidateChannelContract),
		FParamSchemaBuilder()
			.Required(TEXT("channel_tag"), TEXT("string"), TEXT("Gameplay tag channel to broadcast/listen on."))
			.Optional(TEXT("message_struct"), TEXT("string"), TEXT("Optional payload UScriptStruct path to validate with the channel."))
			.Optional(TEXT("match_type"), TEXT("string"), TEXT("ExactMatch or PartialMatch."), TEXT("ExactMatch"))
			.Optional(TEXT("require_registered_tag"), TEXT("boolean"), TEXT("Require the channel tag to exist in the GameplayTags manager."), TEXT("true"))
			.Optional(TEXT("require_blueprint_type"), TEXT("boolean"), TEXT("Require BlueprintType metadata on message_struct when supplied."), TEXT("false"))
			.Build(),
		TEXT("Validation"));

	Registry.RegisterAction(
		TEXT("gameplay_message"), TEXT("trace_channel_usage"),
		TEXT("Trace GameplayMessageRouter broadcaster/listener source call sites and report channel/payload compatibility candidates read-only."),
		FMonolithActionHandler::CreateStatic(&TraceChannelUsage),
		FParamSchemaBuilder()
			.Optional(TEXT("channel_tag"), TEXT("string"), TEXT("Optional channel tag filter. Empty scans all detected channels."))
			.Optional(TEXT("source_root"), TEXT("string"), TEXT("Optional local source directory to scan. Relative paths resolve under the project root."))
			.Optional(TEXT("source_roots"), TEXT("array"), TEXT("Additional local source directories to scan."))
			.Optional(TEXT("include_monolith_source"), TEXT("boolean"), TEXT("Include Plugins/Monolith source in the scan. Default skips Monolith to avoid self-noise."), TEXT("false"))
			.Optional(TEXT("include_engine_gameplay_message_sources"), TEXT("boolean"), TEXT("Also scan the engine GameplayMessageRouter plugin source for the runtime contract."), TEXT("false"))
			.Optional(TEXT("max_files"), TEXT("integer"), TEXT("Maximum source files to scan, clamped 1..10000."), TEXT("2000"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum source matches to report, clamped 1..5000."), TEXT("500"))
			.Optional(TEXT("include_line_text"), TEXT("boolean"), TEXT("Include trimmed source line text in each match."), TEXT("false"))
			.Build(),
		TEXT("Diagnostics"));
}

FMonolithActionResult FMonolithGameplayMessageActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	TArray<TSharedPtr<FJsonValue>> Plugins;
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameplayMessageRouter"))));

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageRuntime"))));
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageNodes"))));

	UClass* SubsystemClass = LoadClassPath(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"));
	UClass* AsyncActionClass = LoadClassPath(TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"));
	UObject* ListenerHandleObject = LoadAnyObjectPath(TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"));
	UEnum* MatchEnum = LoadEnumPath(TEXT("/Script/GameplayMessageRuntime.EGameplayMessageMatch"));

	TArray<TSharedPtr<FJsonValue>> Classes;
	Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"), SubsystemClass, UGameInstanceSubsystem::StaticClass())));
	Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), AsyncActionClass, UObject::StaticClass())));

	TArray<TSharedPtr<FJsonValue>> Structs;
	Structs.Add(MakeShared<FJsonValueObject>(StructSummary(TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"), Cast<UScriptStruct>(ListenerHandleObject), ListenerHandleObject)));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(TEXT("gameplay_message_runtime_available"), SubsystemClass != nullptr);
	Result->SetBoolField(TEXT("async_action_available"), AsyncActionClass != nullptr);
	Result->SetBoolField(TEXT("match_enum_available"), MatchEnum != nullptr);
	Result->SetArrayField(TEXT("plugins"), Plugins);
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("classes"), Classes);
	Result->SetArrayField(TEXT("structs"), Structs);
	Result->SetArrayField(TEXT("listener_contract"), ListenerContractRows());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::DescribeListenerContract(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	UClass* SubsystemClass = LoadClassPath(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"));
	UClass* AsyncActionClass = LoadClassPath(TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"));

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SubsystemClass, TEXT("K2_BroadcastMessage"))));
	Functions.Add(MakeShared<FJsonValueObject>(AsyncActionClass ? FunctionSummary(AsyncActionClass, TEXT("ListenForGameplayMessages")) : FunctionSummary(nullptr, TEXT("ListenForGameplayMessages"))));
	Functions.Add(MakeShared<FJsonValueObject>(AsyncActionClass ? FunctionSummary(AsyncActionClass, TEXT("GetPayload")) : FunctionSummary(nullptr, TEXT("GetPayload"))));

	TArray<TSharedPtr<FJsonValue>> MatchTypes;
	MatchTypes.Add(MakeShared<FJsonValueString>(TEXT("ExactMatch")));
	MatchTypes.Add(MakeShared<FJsonValueString>(TEXT("PartialMatch")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetObjectField(TEXT("subsystem_class"), ClassSummary(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"), SubsystemClass, UGameInstanceSubsystem::StaticClass()));
	Result->SetObjectField(TEXT("async_action_class"), ClassSummary(TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), AsyncActionClass, UObject::StaticClass()));
	Result->SetArrayField(TEXT("functions"), Functions);
	Result->SetArrayField(TEXT("match_types"), MatchTypes);
	Result->SetArrayField(TEXT("listener_contract"), ListenerContractRows());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::ValidateMessageStruct(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	FString StructPath;
	FString Error;
	if (!ReadRequiredStringParam(Params, TEXT("message_struct"), StructPath, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	const bool bRequireBlueprintType = ReadOptionalBoolParam(Params, TEXT("require_blueprint_type"), false);
	const bool bRequireNoObjectReferences = ReadOptionalBoolParam(Params, TEXT("require_no_object_references"), false);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> MessageStruct = ValidateMessageStructInternal(StructPath, bRequireBlueprintType, bRequireNoObjectReferences, Checks, Issues, bOk);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetObjectField(TEXT("message_struct"), MessageStruct);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::ValidateChannelContract(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	FString ChannelTagString;
	FString Error;
	if (!ReadRequiredStringParam(Params, TEXT("channel_tag"), ChannelTagString, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	FString MatchType;
	const FString RequestedMatchType = ReadOptionalStringParam(Params, TEXT("match_type"), TEXT("ExactMatch"));
	if (!NormalizeMatchType(RequestedMatchType, MatchType))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Param 'match_type' must be ExactMatch or PartialMatch, got '%s'"), *RequestedMatchType),
			ErrInvalidParams);
	}

	const bool bRequireRegisteredTag = ReadOptionalBoolParam(Params, TEXT("require_registered_tag"), true);
	const bool bRequireBlueprintType = ReadOptionalBoolParam(Params, TEXT("require_blueprint_type"), false);
	const FString StructPath = ReadOptionalStringParam(Params, TEXT("message_struct"));

	FGameplayTag RequestedTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*ChannelTagString), false);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;

	AddCheck(Checks, bOk, TEXT("channel_tag_non_empty"), !ChannelTagString.IsEmpty(), TEXT("error"), ChannelTagString);
	AddCheck(
		Checks,
		bOk,
		TEXT("channel_tag_registered"),
		RequestedTag.IsValid() || !bRequireRegisteredTag,
		bRequireRegisteredTag ? TEXT("error") : TEXT("warning"),
		RequestedTag.IsValid() ? RequestedTag.ToString() : TEXT("tag is not registered in GameplayTags manager"));
	AddCheck(Checks, bOk, TEXT("match_type_supported"), !MatchType.IsEmpty(), TEXT("error"), MatchType);

	TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
	Channel->SetStringField(TEXT("requested_channel_tag"), ChannelTagString);
	Channel->SetBoolField(TEXT("registered"), RequestedTag.IsValid());
	Channel->SetStringField(TEXT("resolved_channel_tag"), RequestedTag.IsValid() ? RequestedTag.ToString() : FString());
	Channel->SetStringField(TEXT("match_type"), MatchType);
	Channel->SetBoolField(TEXT("require_registered_tag"), bRequireRegisteredTag);

	TSharedPtr<FJsonObject> MessageStruct;
	if (!StructPath.IsEmpty())
	{
		MessageStruct = ValidateMessageStructInternal(StructPath, bRequireBlueprintType, false, Checks, Issues, bOk);
	}

	if (!RequestedTag.IsValid())
	{
		AddIssue(
			Issues,
			bRequireRegisteredTag ? TEXT("error") : TEXT("warning"),
			TEXT("channel_tag_not_registered"),
			FString::Printf(TEXT("Gameplay tag '%s' is not registered."), *ChannelTagString));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetObjectField(TEXT("channel"), Channel);
	if (MessageStruct.IsValid())
	{
		Result->SetObjectField(TEXT("message_struct"), MessageStruct);
	}
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("listener_contract"), ListenerContractRows());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::TraceChannelUsage(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	const bool bIncludeMonolithSource = ReadOptionalBoolParam(Params, TEXT("include_monolith_source"), false);
	const bool bIncludeEngineGameplayMessageSources = ReadOptionalBoolParam(Params, TEXT("include_engine_gameplay_message_sources"), false);
	const bool bIncludeLineText = ReadOptionalBoolParam(Params, TEXT("include_line_text"), false);
	const int32 MaxFiles = ReadOptionalIntParam(Params, TEXT("max_files"), 2000, 1, 10000);
	const int32 MaxResults = ReadOptionalIntParam(Params, TEXT("max_results"), 500, 1, 5000);
	const FString RequestedChannelTag = ReadOptionalStringParam(Params, TEXT("channel_tag"));

	TArray<FString> Roots;
	const FString SingleSourceRoot = ReadOptionalStringParam(Params, TEXT("source_root"));
	if (!SingleSourceRoot.IsEmpty())
	{
		AddOptionalSourceRoot(SingleSourceRoot, Roots);
	}

	FString Error;
	TArray<FString> SourceRoots;
	if (!ReadOptionalStringArrayParam(Params, TEXT("source_roots"), SourceRoots, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}
	for (const FString& SourceRoot : SourceRoots)
	{
		AddOptionalSourceRoot(SourceRoot, Roots);
	}

	if (Roots.Num() == 0)
	{
		Roots = DefaultProjectSourceRoots(bIncludeMonolithSource);
	}
	if (bIncludeEngineGameplayMessageSources)
	{
		AddEngineGameplayMessageSourceRoots(Roots);
	}

	TArray<TSharedPtr<FJsonValue>> RootRows;
	TArray<FString> SourceFiles;
	CollectSourceFiles(Roots, bIncludeMonolithSource, MaxFiles, SourceFiles, RootRows);

	TArray<TSharedPtr<FJsonValue>> Matches;
	TArray<TSharedPtr<FJsonValue>> Broadcasters;
	TArray<TSharedPtr<FJsonValue>> Listeners;
	TMap<FString, TArray<FChannelTraceRow>> RowsByChannel;
	TMap<FString, int32> CountsByRole;
	TMap<FString, int32> CountsByCode;
	int32 FilesWithMatches = 0;
	bool bTruncated = false;

	for (const FString& File : SourceFiles)
	{
		if (Matches.Num() >= MaxResults)
		{
			bTruncated = true;
			break;
		}

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			continue;
		}

		bool bFileMatched = false;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (IsCommentOnlyLine(Line))
			{
				continue;
			}

			for (const FTracePatternSpec& Pattern : TracePatterns)
			{
				if (!Line.Contains(Pattern.Token))
				{
					continue;
				}

				TArray<FString> ChannelCandidates = ExtractQuotedGameplayTagCandidates(Line);
				for (const FString& Constant : ExtractGameplayTagConstants(Line))
				{
					ChannelCandidates.AddUnique(Constant);
				}
				if (ChannelCandidates.Num() == 0)
				{
					ChannelCandidates.Add(TEXT("<unresolved>"));
				}

				FString Payload = ExtractTemplateArgumentNearToken(Line, Pattern.Token);
				if (Payload.IsEmpty())
				{
					Payload = ExtractStaticStructPayloadCandidate(Line);
				}

				FString MatchType = ExtractMatchTypeFromLine(Line);
				if (MatchType.IsEmpty() && FString(Pattern.Role).Equals(TEXT("listener"), ESearchCase::IgnoreCase))
				{
					MatchType = TEXT("ExactMatch(default)");
				}

				for (const FString& ChannelCandidate : ChannelCandidates)
				{
					if (!RequestedChannelTag.IsEmpty()
						&& !ChannelCandidate.Equals(RequestedChannelTag, ESearchCase::IgnoreCase))
					{
						continue;
					}

					FChannelTraceRow TraceRow;
					TraceRow.Role = Pattern.Role;
					TraceRow.Code = Pattern.Code;
					TraceRow.Channel = ChannelCandidate;
					TraceRow.Payload = Payload;
					TraceRow.MatchType = MatchType;
					TraceRow.File = RelativeDisplayPath(File);
					TraceRow.Line = LineIndex + 1;
					TraceRow.FunctionContext = InferFunctionContext(Lines, LineIndex);
					TraceRow.LineText = TrimForJson(Line, 300);

					const TSharedPtr<FJsonObject> Match = TraceRowToJson(TraceRow, bIncludeLineText);
					const TSharedPtr<FJsonValue> MatchValue = MakeShared<FJsonValueObject>(Match);
					Matches.Add(MatchValue);
					if (TraceRow.Role.Equals(TEXT("broadcaster"), ESearchCase::IgnoreCase))
					{
						Broadcasters.Add(MatchValue);
					}
					else if (TraceRow.Role.Equals(TEXT("listener"), ESearchCase::IgnoreCase))
					{
						Listeners.Add(MatchValue);
					}

					RowsByChannel.FindOrAdd(ChannelCandidate).Add(TraceRow);
					CountsByRole.FindOrAdd(TraceRow.Role)++;
					CountsByCode.FindOrAdd(TraceRow.Code)++;
					bFileMatched = true;

					if (Matches.Num() >= MaxResults)
					{
						bTruncated = true;
						break;
					}
				}
				if (Matches.Num() >= MaxResults)
				{
					break;
				}
			}
			if (Matches.Num() >= MaxResults)
			{
				break;
			}
		}

		if (bFileMatched)
		{
			++FilesWithMatches;
		}
	}

	auto MapToJsonRows = [](const TMap<FString, int32>& Counts) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<FString> Keys;
		Counts.GenerateKeyArray(Keys);
		Keys.Sort();

		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Keys.Num());
		for (const FString& Key : Keys)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("key"), Key);
			Row->SetNumberField(TEXT("count"), Counts[Key]);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	};

	TArray<FString> Channels;
	RowsByChannel.GenerateKeyArray(Channels);
	Channels.Sort();

	TArray<TSharedPtr<FJsonValue>> ChannelGraph;
	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 PayloadMismatchCount = 0;
	int32 OrphanBroadcasterCount = 0;
	int32 OrphanListenerCount = 0;
	int32 MatchAmbiguityCount = 0;
	int32 UnresolvedChannelCount = 0;

	for (const FString& Channel : Channels)
	{
		const TArray<FChannelTraceRow>& Rows = RowsByChannel[Channel];
		TArray<FString> Payloads;
		TArray<FString> MatchTypes;
		int32 BroadcasterCount = 0;
		int32 ListenerCount = 0;
		bool bHasPartialMatch = false;
		bool bHasExactMatch = false;

		for (const FChannelTraceRow& Row : Rows)
		{
			if (Row.Role.Equals(TEXT("broadcaster"), ESearchCase::IgnoreCase))
			{
				++BroadcasterCount;
			}
			else if (Row.Role.Equals(TEXT("listener"), ESearchCase::IgnoreCase))
			{
				++ListenerCount;
			}
			if (!Row.Payload.IsEmpty())
			{
				Payloads.AddUnique(Row.Payload);
			}
			if (!Row.MatchType.IsEmpty())
			{
				MatchTypes.AddUnique(Row.MatchType);
				bHasPartialMatch |= Row.MatchType.Contains(TEXT("PartialMatch"));
				bHasExactMatch |= Row.MatchType.Contains(TEXT("ExactMatch"));
			}
		}
		Payloads.Sort();
		MatchTypes.Sort();

		const bool bPayloadMismatch = BroadcasterCount > 0 && ListenerCount > 0 && Payloads.Num() > 1;
		const bool bOrphanBroadcaster = BroadcasterCount > 0 && ListenerCount == 0;
		const bool bOrphanListener = ListenerCount > 0 && BroadcasterCount == 0;
		const bool bMatchAmbiguity = bHasPartialMatch && bHasExactMatch;
		const bool bUnresolvedChannel = Channel.Equals(TEXT("<unresolved>"), ESearchCase::IgnoreCase);

		PayloadMismatchCount += bPayloadMismatch ? 1 : 0;
		OrphanBroadcasterCount += bOrphanBroadcaster ? 1 : 0;
		OrphanListenerCount += bOrphanListener ? 1 : 0;
		MatchAmbiguityCount += bMatchAmbiguity ? 1 : 0;
		UnresolvedChannelCount += bUnresolvedChannel ? 1 : 0;

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("channel"), Channel);
		Row->SetNumberField(TEXT("broadcaster_count"), BroadcasterCount);
		Row->SetNumberField(TEXT("listener_count"), ListenerCount);
		Row->SetArrayField(TEXT("payload_candidates"), StringsToJson(Payloads));
		Row->SetArrayField(TEXT("match_types"), StringsToJson(MatchTypes));
		Row->SetBoolField(TEXT("payload_mismatch_candidate"), bPayloadMismatch);
		Row->SetBoolField(TEXT("orphan_broadcaster_candidate"), bOrphanBroadcaster);
		Row->SetBoolField(TEXT("orphan_listener_candidate"), bOrphanListener);
		Row->SetBoolField(TEXT("match_type_ambiguity_candidate"), bMatchAmbiguity);
		Row->SetBoolField(TEXT("unresolved_channel"), bUnresolvedChannel);
		ChannelGraph.Add(MakeShared<FJsonValueObject>(Row));

		auto AddChannelIssue = [&Issues, &Channel](const TCHAR* Severity, const TCHAR* Code, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("channel"), Channel);
			Issue->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		};

		if (bPayloadMismatch)
		{
			AddChannelIssue(TEXT("warning"), TEXT("payload_mismatch_candidate"), FString::Printf(TEXT("Channel '%s' has multiple inferred payload candidates."), *Channel));
		}
		if (bOrphanBroadcaster)
		{
			AddChannelIssue(TEXT("warning"), TEXT("orphan_broadcaster_candidate"), FString::Printf(TEXT("Channel '%s' has broadcaster call sites but no listener call sites in the scanned source roots."), *Channel));
		}
		if (bOrphanListener)
		{
			AddChannelIssue(TEXT("warning"), TEXT("orphan_listener_candidate"), FString::Printf(TEXT("Channel '%s' has listener call sites but no broadcaster call sites in the scanned source roots."), *Channel));
		}
		if (bMatchAmbiguity)
		{
			AddChannelIssue(TEXT("info"), TEXT("match_type_ambiguity_candidate"), FString::Printf(TEXT("Channel '%s' mixes ExactMatch and PartialMatch candidates; verify parent/child channel routing intent."), *Channel));
		}
		if (bUnresolvedChannel)
		{
			AddChannelIssue(TEXT("info"), TEXT("unresolved_channel_candidate"), TEXT("One or more call sites did not expose a channel tag or constant on the matched source line."));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bOk = true;
	AddCheck(Checks, bOk, TEXT("source_roots_present"), RootRows.Num() > 0, TEXT("error"), FString::Printf(TEXT("roots_checked=%d"), RootRows.Num()));
	AddCheck(Checks, bOk, TEXT("source_files_scanned"), SourceFiles.Num() > 0, TEXT("error"), FString::Printf(TEXT("files_scanned=%d"), SourceFiles.Num()));
	AddCheck(Checks, bOk, TEXT("trace_matches_collected"), Matches.Num() > 0, TEXT("warning"), FString::Printf(TEXT("match_count=%d"), Matches.Num()));

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_files"), MaxFiles);
	Limits->SetNumberField(TEXT("max_results"), MaxResults);
	Limits->SetBoolField(TEXT("truncated"), bTruncated);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("channel_count"), Channels.Num());
	Summary->SetNumberField(TEXT("broadcaster_count"), Broadcasters.Num());
	Summary->SetNumberField(TEXT("listener_count"), Listeners.Num());
	Summary->SetNumberField(TEXT("payload_mismatch_candidate_count"), PayloadMismatchCount);
	Summary->SetNumberField(TEXT("orphan_broadcaster_candidate_count"), OrphanBroadcasterCount);
	Summary->SetNumberField(TEXT("orphan_listener_candidate_count"), OrphanListenerCount);
	Summary->SetNumberField(TEXT("match_type_ambiguity_candidate_count"), MatchAmbiguityCount);
	Summary->SetNumberField(TEXT("unresolved_channel_count"), UnresolvedChannelCount);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetStringField(TEXT("action"), TEXT("trace_channel_usage"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("analysis_mode"), TEXT("static_source_trace"));
	Result->SetStringField(TEXT("runtime_execution"), TEXT("not_performed"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetStringField(TEXT("requested_channel_tag"), RequestedChannelTag);
	Result->SetBoolField(TEXT("include_monolith_source"), bIncludeMonolithSource);
	Result->SetBoolField(TEXT("include_engine_gameplay_message_sources"), bIncludeEngineGameplayMessageSources);
	Result->SetNumberField(TEXT("roots_checked"), RootRows.Num());
	Result->SetNumberField(TEXT("files_scanned"), SourceFiles.Num());
	Result->SetNumberField(TEXT("files_with_matches"), FilesWithMatches);
	Result->SetNumberField(TEXT("match_count"), Matches.Num());
	Result->SetObjectField(TEXT("limits"), Limits);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetArrayField(TEXT("source_roots"), RootRows);
	Result->SetArrayField(TEXT("patterns"), TracePatternRows());
	Result->SetArrayField(TEXT("counts_by_code"), MapToJsonRows(CountsByCode));
	Result->SetArrayField(TEXT("counts_by_role"), MapToJsonRows(CountsByRole));
	Result->SetArrayField(TEXT("channel_graph"), ChannelGraph);
	Result->SetArrayField(TEXT("broadcasters"), Broadcasters);
	Result->SetArrayField(TEXT("listeners"), Listeners);
	Result->SetArrayField(TEXT("matches"), Matches);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("limitations"), TraceLimitationRows());
	Result->SetStringField(TEXT("trace_contract"), TEXT("Lexical GameplayMessageRouter source trace only: reports candidate broadcaster/listener channel and payload relationships, but does not prove runtime reachability, listener lifetime, or branch coverage."));
	return FMonolithActionResult::Success(Result);
}
