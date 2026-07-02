#include "MonolithConsoleActions.h"

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	static constexpr int32 DefaultListLimit = 100;
	static constexpr int32 MaxListLimit = 5000;
	static constexpr int32 DefaultLogLimit = 200;
	static constexpr int32 MaxLogLimit = 2000;
	static constexpr int32 MaxSequenceSteps = 100;
	static constexpr int32 MaxSettleMs = 5000;
	static constexpr int32 DefaultCaptureWaitMs = 120000;
	static constexpr int32 MaxCaptureWaitMs = 240000;
	static constexpr int32 DefaultWaitForLogMs = 3000;
	static constexpr int32 MaxWaitForLogMs = 30000;
	static constexpr int32 DefaultWaitPollMs = 100;
	static constexpr int32 MinWaitPollMs = 25;
	static constexpr int32 MaxWaitPollMs = 1000;
	static constexpr int32 ConsolePumpSliceMs = 16;

	void WaitForConsoleAsyncWork(const int32 WaitMs)
	{
		if (WaitMs <= 0)
		{
			return;
		}

		if (!IsInGameThread())
		{
			FPlatformProcess::Sleep(static_cast<float>(WaitMs) / 1000.0f);
			return;
		}

		int32 RemainingMs = WaitMs;
		while (RemainingMs > 0)
		{
			const int32 SliceMs = FMath::Min(ConsolePumpSliceMs, RemainingMs);

			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication& Slate = FSlateApplication::Get();
				Slate.PumpMessages();
				Slate.Tick();
			}

			FPlatformProcess::Sleep(0.001f);
			RemainingMs -= SliceMs;
		}
	}

	struct FConsoleExecutionOptions
	{
		bool bDryRun = true;
		bool bRequireKnownObject = false;
		FString WorldTarget = TEXT("auto");
	};

	struct FConsoleExecutionOutcome
	{
		bool bOk = false;
		int32 ErrorCode = -32603;
		FString Error;
		TSharedPtr<FJsonObject> Result;
	};

	struct FConsoleLogExpectation
	{
		FString Pattern;
		FString Category;
		int32 MinCount = 1;
		int32 MaxCount = -1;
	};

	struct FScreenshotFileSnapshot
	{
		FDateTime Timestamp = FDateTime::MinValue();
		int64 Size = -1;
	};

	struct FPendingConsoleCapture
	{
		FString Id;
		FString CaptureCommand;
		FString OutputPath;
		FString ResolvedOutputPath;
		FString CapturedPath;
		FString Status = TEXT("capture_pending");
		FString Warning;
		TMap<FString, FScreenshotFileSnapshot> BeforeFiles;
		FDateTime CreatedUtc = FDateTime::UtcNow();
		FDateTime CompletedUtc = FDateTime::MinValue();
		int32 CaptureWaitMs = 0;
		int32 CaptureWaitedMs = 0;
		int32 CopyResult = COPY_OK;
		bool bCompleted = false;
		bool bPassed = false;
	};

	using FPendingConsoleCapturePtr = TSharedPtr<FPendingConsoleCapture, ESPMode::ThreadSafe>;

	FCriticalSection GPendingConsoleCaptureLock;
	TMap<FString, FPendingConsoleCapturePtr> GPendingConsoleCaptures;

	FString NormalizeObjectType(FString ObjectType)
	{
		ObjectType.TrimStartAndEndInline();
		ObjectType = ObjectType.ToLower();
		if (ObjectType == TEXT("cvar") || ObjectType == TEXT("cvars") || ObjectType == TEXT("variable"))
		{
			return TEXT("variable");
		}
		if (ObjectType == TEXT("cmd") || ObjectType == TEXT("command"))
		{
			return TEXT("command");
		}
		if (ObjectType == TEXT("object") || ObjectType == TEXT("objects") || ObjectType == TEXT("all"))
		{
			return TEXT("all");
		}
		return ObjectType.IsEmpty() ? TEXT("all") : ObjectType;
	}

	bool MatchesObjectType(const FString& Candidate, const FString& Filter)
	{
		return Filter.IsEmpty() || Filter == TEXT("all") || Candidate == Filter;
	}

	int32 ReadLimit(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName = TEXT("limit"), int32 DefaultValue = DefaultListLimit)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		double Raw = 0.0;
		if (!Params->TryGetNumberField(FieldName, Raw))
		{
			return DefaultValue;
		}
		return FMath::Clamp(static_cast<int32>(Raw), 1, MaxListLimit);
	}

	int32 ReadOptionalLimit(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName = TEXT("limit"), int32 DefaultValue = 0)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		double Raw = 0.0;
		if (!Params->TryGetNumberField(FieldName, Raw))
		{
			return DefaultValue;
		}
		const int32 Value = static_cast<int32>(Raw);
		return Value <= 0 ? 0 : FMath::Clamp(Value, 1, MaxListLimit);
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
	{
		bool Value = DefaultValue;
		if (Params.IsValid() && Params->HasField(FieldName))
		{
			Params->TryGetBoolField(FieldName, Value);
		}
		return Value;
	}

	void RunOnGameThreadBlocking(TFunctionRef<void()> Work)
	{
		if (IsInGameThread())
		{
			Work();
			return;
		}

		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true);
		AsyncTask(ENamedThreads::GameThread, [&Work, Done]()
		{
			Work();
			Done->Trigger();
		});
		Done->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Done);
	}

	FString ClassifyConsoleObject(IConsoleObject* Object)
	{
		if (!Object)
		{
			return TEXT("object");
		}
		if (Object->AsVariable())
		{
			return TEXT("variable");
		}
		if (Object->AsCommand())
		{
			return TEXT("command");
		}
		return TEXT("object");
	}

	FString VariableType(IConsoleVariable* Variable)
	{
		if (!Variable)
		{
			return TEXT("");
		}
		if (Variable->IsVariableBool())
		{
			return TEXT("bool");
		}
		if (Variable->IsVariableInt())
		{
			return TEXT("int");
		}
		if (Variable->IsVariableFloat())
		{
			return TEXT("float");
		}
		if (Variable->IsVariableString())
		{
			return TEXT("string");
		}
		return TEXT("unknown");
	}

	FMonolithConsoleObjectRow MakeConsoleObjectRow(
		const FString& Name,
		IConsoleObject* Object,
		bool bIncludeValues,
		bool bIncludeDefaults)
	{
		FMonolithConsoleObjectRow Row;
		Row.Name = Name;
		Row.ObjectType = ClassifyConsoleObject(Object);
		Row.Source = TEXT("live");

		if (!Object)
		{
			return Row;
		}

		Row.Help = Object->GetHelp();
		Row.Flags = static_cast<int32>(Object->GetFlags());
		Row.bIsEnabled = Object->IsEnabled();
		Row.bIsDeprecated = Object->IsDeprecated();

		if (IConsoleVariable* Variable = Object->AsVariable())
		{
			if (bIncludeValues)
			{
				Row.Value = Variable->GetString();
			}
			if (bIncludeDefaults)
			{
				Row.DefaultValue = Variable->GetDefaultValue();
			}
			Row.VariableType = VariableType(Variable);
			Row.SetBy = GetConsoleVariableSetByName(Variable->GetFlags());
			Row.bReadOnly = Variable->TestFlags(ECVF_ReadOnly);
			Row.bCheat = Variable->TestFlags(ECVF_Cheat);
		}
		else
		{
			const uint32 Flags = static_cast<uint32>(Object->GetFlags());
			Row.bReadOnly = (Flags & static_cast<uint32>(ECVF_ReadOnly)) != 0;
			Row.bCheat = (Flags & static_cast<uint32>(ECVF_Cheat)) != 0;
		}

		return Row;
	}

	TSharedPtr<FJsonObject> ConsoleRowToJson(const FMonolithConsoleObjectRow& Row, const FString& CapturedAt = TEXT(""))
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Row.Name);
		Obj->SetStringField(TEXT("object_type"), Row.ObjectType);
		Obj->SetStringField(TEXT("help"), Row.Help);
		Obj->SetNumberField(TEXT("flags"), Row.Flags);
		Obj->SetBoolField(TEXT("is_enabled"), Row.bIsEnabled);
		Obj->SetBoolField(TEXT("is_deprecated"), Row.bIsDeprecated);
		if (!Row.Value.IsEmpty())
		{
			Obj->SetStringField(TEXT("value"), Row.Value);
		}
		if (!Row.DefaultValue.IsEmpty())
		{
			Obj->SetStringField(TEXT("default_value"), Row.DefaultValue);
		}
		if (!Row.VariableType.IsEmpty())
		{
			Obj->SetStringField(TEXT("variable_type"), Row.VariableType);
		}
		if (!Row.SetBy.IsEmpty())
		{
			Obj->SetStringField(TEXT("set_by"), Row.SetBy);
		}
		Obj->SetBoolField(TEXT("read_only"), Row.bReadOnly);
		Obj->SetBoolField(TEXT("cheat"), Row.bCheat);
		Obj->SetStringField(TEXT("source"), Row.Source);
		if (!CapturedAt.IsEmpty())
		{
			Obj->SetStringField(TEXT("captured_at"), CapturedAt);
		}
		return Obj;
	}

	TArray<FMonolithConsoleObjectRow> CollectConsoleObjects(
		const FString& Query,
		const FString& Mode,
		const FString& ObjectType,
		int32 Limit,
		bool bIncludeValues,
		bool bIncludeDefaults,
		int32& OutMatched)
	{
		TArray<FMonolithConsoleObjectRow> Rows;
		OutMatched = 0;
		const FString TypeFilter = NormalizeObjectType(ObjectType);
		const int32 SafeLimit = Limit <= 0 ? MAX_int32 : FMath::Clamp(Limit, 1, MaxListLimit);

		RunOnGameThreadBlocking([&]()
		{
			FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
				[&](const TCHAR* RawName, IConsoleObject* Object)
				{
					const FString Name(RawName ? RawName : TEXT(""));
					const FString CandidateType = ClassifyConsoleObject(Object);
					if (!MatchesObjectType(CandidateType, TypeFilter))
					{
						return;
					}

					++OutMatched;
					if (Rows.Num() < SafeLimit)
					{
						Rows.Add(MakeConsoleObjectRow(Name, Object, bIncludeValues, bIncludeDefaults));
					}
				});

			if (Mode.Equals(TEXT("contains"), ESearchCase::IgnoreCase) && !Query.IsEmpty())
			{
				IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *Query);
			}
			else
			{
				IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(Visitor, *Query);
			}
		});

		Rows.Sort([](const FMonolithConsoleObjectRow& A, const FMonolithConsoleObjectRow& B)
		{
			return A.Name < B.Name;
		});
		return Rows;
	}

	TOptional<FMonolithConsoleObjectRow> FindLiveConsoleObject(
		const FString& Name,
		bool bIncludeValues,
		bool bIncludeDefaults)
	{
		TOptional<FMonolithConsoleObjectRow> Row;
		RunOnGameThreadBlocking([&]()
		{
			if (IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Name))
			{
				Row = MakeConsoleObjectRow(Name, Object, bIncludeValues, bIncludeDefaults);
			}
		});
		return Row;
	}

	FMonolithSourceDatabase* GetSourceDatabase(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is unavailable; console snapshot database access requires a live editor.");
			return nullptr;
		}

		UMonolithSourceSubsystem* SourceSubsystem = GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>();
		if (!SourceSubsystem)
		{
			OutError = TEXT("UMonolithSourceSubsystem is unavailable; enable MonolithSource and run source.trigger_reindex if needed.");
			return nullptr;
		}

		FMonolithSourceDatabase* Database = SourceSubsystem->GetDatabase();
		if (!Database)
		{
			OutError = TEXT("EngineSource DB is not open; run source.trigger_reindex before refreshing or searching console snapshots.");
			return nullptr;
		}
		return Database;
	}

	FMonolithActionResult SourceDatabaseError(const FString& Error)
	{
		return FMonolithActionResult::Error(Error, -32000)
			.WithRelatedAction(TEXT("source.trigger_reindex"))
			.WithRelatedAction(TEXT("console.health"));
	}

	FString FirstConsoleToken(const FString& Command)
	{
		FString Trimmed = Command.TrimStartAndEnd();
		int32 SplitIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
		{
			if (FChar::IsWhitespace(Trimmed[Index]))
			{
				SplitIndex = Index;
				break;
			}
		}
		return SplitIndex == INDEX_NONE ? Trimmed : Trimmed.Left(SplitIndex);
	}

	FString NormalizeWorldTarget(FString TargetWorld)
	{
		TargetWorld.TrimStartAndEndInline();
		TargetWorld = TargetWorld.ToLower();
		if (TargetWorld.IsEmpty() || TargetWorld == TEXT("any"))
		{
			return TEXT("auto");
		}
		if (TargetWorld == TEXT("game"))
		{
			return TEXT("pie");
		}
		return TargetWorld;
	}

	TSharedPtr<FJsonObject> MakeObjectArrayResult(
		const TArray<FMonolithConsoleObjectRow>& Rows,
		const FString& Query,
		const FString& Mode,
		const FString& ObjectType,
		int32 Matched,
		int32 Limit)
	{
		TArray<TSharedPtr<FJsonValue>> JsonRows;
		JsonRows.Reserve(Rows.Num());
		for (const FMonolithConsoleObjectRow& Row : Rows)
		{
			JsonRows.Add(MakeShared<FJsonValueObject>(ConsoleRowToJson(Row)));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("source"), TEXT("live"));
		Root->SetStringField(TEXT("query"), Query);
		Root->SetStringField(TEXT("mode"), Mode);
		Root->SetStringField(TEXT("object_type"), NormalizeObjectType(ObjectType));
		Root->SetNumberField(TEXT("matched_count"), Matched);
		Root->SetNumberField(TEXT("returned_count"), Rows.Num());
		Root->SetNumberField(TEXT("limit"), Limit);
		Root->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
		if (Matched > Rows.Num())
		{
			Root->SetNumberField(TEXT("truncated_remaining"), Matched - Rows.Num());
		}
		Root->SetArrayField(TEXT("objects"), JsonRows);
		return Root;
	}

	UWorld* ResolveConsoleWorld(const FString& WorldTarget, FString& OutWorldType)
	{
		UWorld* TargetWorld = nullptr;
		OutWorldType = TEXT("none");
		if (GEditor)
		{
			if (WorldTarget != TEXT("editor"))
			{
				for (const FWorldContext& Context : GEditor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE && Context.World())
					{
						TargetWorld = Context.World();
						OutWorldType = TEXT("pie");
						break;
					}
				}
			}
			if (!TargetWorld && WorldTarget != TEXT("pie"))
			{
				TargetWorld = GEditor->GetEditorWorldContext().World();
				OutWorldType = TEXT("editor");
			}
		}
		return TargetWorld;
	}

	FString ConsoleVerbosityToString(ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal: return TEXT("fatal");
		case ELogVerbosity::Error: return TEXT("error");
		case ELogVerbosity::Warning: return TEXT("warning");
		case ELogVerbosity::Display: return TEXT("display");
		case ELogVerbosity::Log: return TEXT("log");
		case ELogVerbosity::Verbose: return TEXT("verbose");
		case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
		default: return TEXT("unknown");
		}
	}

	ELogVerbosity::Type ConsoleStringToVerbosity(FString Verbosity)
	{
		Verbosity.TrimStartAndEndInline();
		Verbosity = Verbosity.ToLower();
		if (Verbosity == TEXT("fatal")) { return ELogVerbosity::Fatal; }
		if (Verbosity == TEXT("error")) { return ELogVerbosity::Error; }
		if (Verbosity == TEXT("warning")) { return ELogVerbosity::Warning; }
		if (Verbosity == TEXT("display")) { return ELogVerbosity::Display; }
		if (Verbosity == TEXT("log")) { return ELogVerbosity::Log; }
		if (Verbosity == TEXT("verbose")) { return ELogVerbosity::Verbose; }
		if (Verbosity == TEXT("very_verbose") || Verbosity == TEXT("veryverbose")) { return ELogVerbosity::VeryVerbose; }
		return ELogVerbosity::VeryVerbose;
	}

	TSharedPtr<FJsonObject> ConsoleLogEntryToJson(const FMonolithLogEntry& Entry)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("sequence"), static_cast<double>(Entry.Sequence));
		Obj->SetNumberField(TEXT("timestamp"), Entry.Timestamp);
		Obj->SetStringField(TEXT("category"), Entry.Category.ToString());
		Obj->SetStringField(TEXT("verbosity"), ConsoleVerbosityToString(Entry.Verbosity));
		Obj->SetStringField(TEXT("message"), Entry.Message);
		return Obj;
	}

	bool ReadStringField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, const FString& DefaultValue = TEXT(""))
	{
		OutValue = DefaultValue;
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		return Params->TryGetStringField(FieldName, OutValue);
	}

	int32 ReadClampedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		double Raw = 0.0;
		if (!Params->TryGetNumberField(FieldName, Raw))
		{
			return DefaultValue;
		}
		return FMath::Clamp(static_cast<int32>(Raw), MinValue, MaxValue);
	}

	bool ReadRequiredCursor(const TSharedPtr<FJsonObject>& Params, int64& OutCursor)
	{
		OutCursor = 0;
		double Raw = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("cursor"), Raw))
		{
			return false;
		}
		OutCursor = static_cast<int64>(Raw);
		return true;
	}

	FConsoleExecutionOptions ReadExecutionOptions(const TSharedPtr<FJsonObject>& Params, bool bDefaultDryRun)
	{
		FConsoleExecutionOptions Options;
		Options.bDryRun = ReadBool(Params, TEXT("dry_run"), bDefaultDryRun);
		Options.bRequireKnownObject = ReadBool(Params, TEXT("require_known_object"), true);
		ReadStringField(Params, TEXT("target_world"), Options.WorldTarget, TEXT("auto"));
		Options.WorldTarget = NormalizeWorldTarget(Options.WorldTarget);
		return Options;
	}

	bool ValidateWorldTarget(const FString& WorldTarget, FString& OutError)
	{
		if (WorldTarget == TEXT("auto") || WorldTarget == TEXT("pie") || WorldTarget == TEXT("editor"))
		{
			return true;
		}
		OutError = FString::Printf(TEXT("Invalid target_world '%s'; expected auto, pie, or editor."), *WorldTarget);
		return false;
	}

	FConsoleExecutionOutcome ExecuteConsoleCommandInternal(const FString& Command, const FConsoleExecutionOptions& Options)
	{
		FConsoleExecutionOutcome Outcome;
		RunOnGameThreadBlocking([&]()
		{
			const FString FirstToken = FirstConsoleToken(Command);
			IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(*FirstToken);
			if (Options.bRequireKnownObject && !ConsoleObject)
			{
				Outcome.ErrorCode = FMonolithJsonUtils::ErrInvalidParams;
				Outcome.Error = FString::Printf(TEXT("First console token does not resolve to a registered console object: %s"), *FirstToken);
				return;
			}

			FString WorldType;
			UWorld* TargetWorld = ResolveConsoleWorld(Options.WorldTarget, WorldType);
			if (!TargetWorld && (Options.WorldTarget != TEXT("auto") || !Options.bDryRun))
			{
				Outcome.Error = Options.WorldTarget == TEXT("pie")
					? TEXT("No live PIE/game world found for console.execute target_world=pie.")
					: Options.WorldTarget == TEXT("editor")
						? TEXT("No editor world found for console.execute target_world=editor.")
						: TEXT("No usable world found (no PIE active and no editor world).");
				return;
			}

			bool bExecutedViaPC = false;
			if (!Options.bDryRun)
			{
				if (APlayerController* PC = TargetWorld ? TargetWorld->GetFirstPlayerController() : nullptr)
				{
					PC->ConsoleCommand(Command, /*bWriteToLog=*/true);
					bExecutedViaPC = true;
				}
				else
				{
					if (!GEngine)
					{
						Outcome.Error = TEXT("GEngine is null; console.execute requires engine context.");
						return;
					}
					GEngine->Exec(TargetWorld, *Command);
				}
			}

			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("command"), Command);
			Root->SetStringField(TEXT("first_token"), FirstToken);
			Root->SetBoolField(TEXT("known_object"), ConsoleObject != nullptr);
			if (ConsoleObject)
			{
				Root->SetStringField(TEXT("object_type"), ClassifyConsoleObject(ConsoleObject));
			}
			Root->SetBoolField(TEXT("dry_run"), Options.bDryRun);
			Root->SetStringField(TEXT("target_world"), Options.WorldTarget);
			Root->SetStringField(TEXT("world"), WorldType);
			Root->SetBoolField(TEXT("via_player_controller"), bExecutedViaPC);
			Root->SetStringField(TEXT("status"), Options.bDryRun ? TEXT("validated") : TEXT("executed"));

			Outcome.bOk = true;
			Outcome.Result = Root;
		});
		return Outcome;
	}

	bool EntryMatchesExpectation(const FMonolithLogEntry& Entry, const FConsoleLogExpectation& Expectation)
	{
		if (!Expectation.Category.IsEmpty() && Entry.Category != FName(*Expectation.Category))
		{
			return false;
		}
		return Expectation.Pattern.IsEmpty() || Entry.Message.Contains(Expectation.Pattern, ESearchCase::IgnoreCase);
	}

	bool ParseExpectationObject(const TSharedPtr<FJsonObject>& Obj, FConsoleLogExpectation& OutExpectation, FString& OutError)
	{
		if (!Obj.IsValid() || !Obj->TryGetStringField(TEXT("pattern"), OutExpectation.Pattern) || OutExpectation.Pattern.IsEmpty())
		{
			OutError = TEXT("Log expectation objects require non-empty string field 'pattern'.");
			return false;
		}
		Obj->TryGetStringField(TEXT("category"), OutExpectation.Category);
		double MinValue = static_cast<double>(OutExpectation.MinCount);
		if (Obj->TryGetNumberField(TEXT("min_count"), MinValue))
		{
			OutExpectation.MinCount = FMath::Max(0, static_cast<int32>(MinValue));
		}
		double MaxValue = static_cast<double>(OutExpectation.MaxCount);
		if (Obj->TryGetNumberField(TEXT("max_count"), MaxValue))
		{
			OutExpectation.MaxCount = static_cast<int32>(MaxValue);
		}
		return true;
	}

	bool AddExpectationFromValue(const TSharedPtr<FJsonValue>& Value, TArray<FConsoleLogExpectation>& OutExpectations, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("Log expectation entry is null.");
			return false;
		}

		FString Pattern;
		if (Value->TryGetString(Pattern))
		{
			if (Pattern.IsEmpty())
			{
				OutError = TEXT("Log expectation pattern cannot be empty.");
				return false;
			}
			FConsoleLogExpectation Expectation;
			Expectation.Pattern = Pattern;
			OutExpectations.Add(MoveTemp(Expectation));
			return true;
		}

		TSharedPtr<FJsonObject> Obj = Value->AsObject();
		FConsoleLogExpectation Expectation;
		if (!ParseExpectationObject(Obj, Expectation, OutError))
		{
			return false;
		}
		OutExpectations.Add(MoveTemp(Expectation));
		return true;
	}

	bool ReadExpectations(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* SingleField,
		const TCHAR* ArrayField,
		TArray<FConsoleLogExpectation>& OutExpectations,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		if (Params->HasField(SingleField))
		{
			FString Pattern;
			if (!Params->TryGetStringField(SingleField, Pattern) || Pattern.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s must be a non-empty string."), SingleField);
				return false;
			}
			FConsoleLogExpectation Expectation;
			Expectation.Pattern = Pattern;
			OutExpectations.Add(MoveTemp(Expectation));
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params->TryGetArrayField(ArrayField, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!AddExpectationFromValue(Value, OutExpectations, OutError))
				{
					return false;
				}
			}
		}
		else if (Params->HasField(ArrayField))
		{
			OutError = FString::Printf(TEXT("%s must be an array of strings or objects."), ArrayField);
			return false;
		}
		return true;
	}

	bool ReadCommandArray(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonObject>>& OutSteps, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), Commands) || !Commands || Commands->Num() == 0)
		{
			OutError = TEXT("Missing required non-empty array field: commands.");
			return false;
		}
		if (Commands->Num() > MaxSequenceSteps)
		{
			OutError = FString::Printf(TEXT("commands has %d entries; maximum is %d."), Commands->Num(), MaxSequenceSteps);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Commands)
		{
			FString Command;
			if (Value.IsValid() && Value->TryGetString(Command))
			{
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("command"), Command);
				OutSteps.Add(Step);
				continue;
			}

			TSharedPtr<FJsonObject> StepObj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!StepObj.IsValid())
			{
				OutError = TEXT("commands entries must be strings or objects.");
				return false;
			}
			OutSteps.Add(StepObj);
		}
		return true;
	}

	TArray<FMonolithLogEntry> CollectLogsAfter(
		int64 Cursor,
		const FString& Pattern,
		const FString& Category,
		ELogVerbosity::Type MaxVerbosity,
		int32 Limit)
	{
		TArray<FMonolithLogEntry> Filtered;
		if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
		{
			TArray<FMonolithLogEntry> Entries = LogCapture->GetEntriesAfter(
				Cursor, /*CategoryFilter*/{}, MaxVerbosity, Limit);
			for (const FMonolithLogEntry& Entry : Entries)
			{
				if (!Category.IsEmpty() && Entry.Category != FName(*Category))
				{
					continue;
				}
				if (!Pattern.IsEmpty() && !Entry.Message.Contains(Pattern, ESearchCase::IgnoreCase))
				{
					continue;
				}
				Filtered.Add(Entry);
			}
		}
		return Filtered;
	}

	TSharedPtr<FJsonObject> BuildExpectationReport(
		const TArray<FMonolithLogEntry>& Entries,
		const TArray<FConsoleLogExpectation>& Expected,
		const TArray<FConsoleLogExpectation>& Rejected,
		bool& bOutPassed)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ExpectedJson;
		TArray<TSharedPtr<FJsonValue>> RejectedJson;
		bOutPassed = true;

		auto BuildOne = [&Entries, &bOutPassed](const FConsoleLogExpectation& Expectation, bool bReject) -> TSharedPtr<FJsonObject>
		{
			int32 Count = 0;
			TArray<TSharedPtr<FJsonValue>> Matches;
			for (const FMonolithLogEntry& Entry : Entries)
			{
				if (EntryMatchesExpectation(Entry, Expectation))
				{
					++Count;
					if (Matches.Num() < 10)
					{
						Matches.Add(MakeShared<FJsonValueObject>(ConsoleLogEntryToJson(Entry)));
					}
				}
			}

			const bool bPassed = bReject
				? Count == 0
				: Count >= Expectation.MinCount && (Expectation.MaxCount < 0 || Count <= Expectation.MaxCount);
			if (!bPassed)
			{
				bOutPassed = false;
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("pattern"), Expectation.Pattern);
			if (!Expectation.Category.IsEmpty())
			{
				Obj->SetStringField(TEXT("category"), Expectation.Category);
			}
			Obj->SetNumberField(TEXT("count"), Count);
			Obj->SetNumberField(TEXT("min_count"), bReject ? 0 : Expectation.MinCount);
			if (!bReject && Expectation.MaxCount >= 0)
			{
				Obj->SetNumberField(TEXT("max_count"), Expectation.MaxCount);
			}
			Obj->SetBoolField(TEXT("passed"), bPassed);
			Obj->SetArrayField(TEXT("matches"), Matches);
			return Obj;
		};

		for (const FConsoleLogExpectation& Expectation : Expected)
		{
			ExpectedJson.Add(MakeShared<FJsonValueObject>(BuildOne(Expectation, /*bReject=*/false)));
		}
		for (const FConsoleLogExpectation& Expectation : Rejected)
		{
			RejectedJson.Add(MakeShared<FJsonValueObject>(BuildOne(Expectation, /*bReject=*/true)));
		}

		Root->SetBoolField(TEXT("passed"), bOutPassed);
		Root->SetArrayField(TEXT("expected"), ExpectedJson);
		Root->SetArrayField(TEXT("rejected"), RejectedJson);
		return Root;
	}

	TMap<FString, FScreenshotFileSnapshot> SnapshotPngFiles()
	{
		TArray<FString> Files;
		const FString ScreenshotRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots"));
		IFileManager::Get().FindFilesRecursive(Files, *ScreenshotRoot, TEXT("*.png"), /*Files=*/true, /*Directories=*/false, /*ClearFileNames=*/true);
		TMap<FString, FScreenshotFileSnapshot> Snapshot;
		for (FString& File : Files)
		{
			FPaths::NormalizeFilename(File);
			FScreenshotFileSnapshot Entry;
			Entry.Timestamp = IFileManager::Get().GetTimeStamp(*File);
			Entry.Size = IFileManager::Get().FileSize(*File);
			Snapshot.Add(File, Entry);
		}
		return Snapshot;
	}

	FString FindNewestChangedPng(const TMap<FString, FScreenshotFileSnapshot>& Before)
	{
		TMap<FString, FScreenshotFileSnapshot> After = SnapshotPngFiles();
		FString BestPath;
		FDateTime BestTime = FDateTime::MinValue();
		for (const TPair<FString, FScreenshotFileSnapshot>& Pair : After)
		{
			const FScreenshotFileSnapshot* Previous = Before.Find(Pair.Key);
			if (Previous && Previous->Timestamp == Pair.Value.Timestamp && Previous->Size == Pair.Value.Size)
			{
				continue;
			}
			if (BestPath.IsEmpty() || Pair.Value.Timestamp > BestTime)
			{
				BestPath = Pair.Key;
				BestTime = Pair.Value.Timestamp;
			}
		}
		return BestPath;
	}

	FString ResolveOutputPath(const FString& OutputPath)
	{
		if (OutputPath.IsEmpty())
		{
			return TEXT("");
		}
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(OutputPath) ? FPaths::ProjectDir() / OutputPath : OutputPath);
	}

	void ApplyCaptureOutputPath(const FString& CapturedPath, const FString& ResolvedOutputPath, bool& bOutPassed, FString& OutStatus, int32& OutCopyResult)
	{
		bOutPassed = true;
		OutStatus = TEXT("captured");
		OutCopyResult = COPY_OK;
		if (CapturedPath.IsEmpty() || ResolvedOutputPath.IsEmpty() || ResolvedOutputPath.Equals(CapturedPath, ESearchCase::IgnoreCase))
		{
			return;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResolvedOutputPath), /*Tree=*/true);
		OutCopyResult = IFileManager::Get().Copy(*ResolvedOutputPath, *CapturedPath, /*Replace=*/true, /*EvenIfReadOnly=*/true);
		if (OutCopyResult != COPY_OK)
		{
			bOutPassed = false;
			OutStatus = TEXT("copy_failed");
		}
	}

	TSharedPtr<FJsonObject> PendingCaptureToJson(const FPendingConsoleCapture& Capture)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("capture_id"), Capture.Id);
		Root->SetStringField(TEXT("status"), Capture.Status);
		Root->SetBoolField(TEXT("completed"), Capture.bCompleted);
		Root->SetBoolField(TEXT("passed"), Capture.bPassed);
		Root->SetStringField(TEXT("capture_command"), Capture.CaptureCommand);
		Root->SetNumberField(TEXT("capture_wait_ms"), Capture.CaptureWaitMs);
		Root->SetNumberField(TEXT("capture_waited_ms"), Capture.CaptureWaitedMs);
		Root->SetStringField(TEXT("created_utc"), Capture.CreatedUtc.ToIso8601());
		if (Capture.bCompleted)
		{
			Root->SetStringField(TEXT("completed_utc"), Capture.CompletedUtc.ToIso8601());
		}
		if (!Capture.CapturedPath.IsEmpty())
		{
			Root->SetStringField(TEXT("capture_path"), Capture.CapturedPath);
		}
		if (!Capture.ResolvedOutputPath.IsEmpty())
		{
			Root->SetStringField(TEXT("output_path"), Capture.ResolvedOutputPath);
		}
		if (Capture.CopyResult != COPY_OK)
		{
			Root->SetNumberField(TEXT("copy_result"), Capture.CopyResult);
		}
		if (!Capture.Warning.IsEmpty())
		{
			Root->SetStringField(TEXT("warning"), Capture.Warning);
		}
		return Root;
	}

	void TrimPendingConsoleCapturesLocked()
	{
		const FDateTime Now = FDateTime::UtcNow();
		TArray<FString> RemoveIds;
		for (const TPair<FString, FPendingConsoleCapturePtr>& Pair : GPendingConsoleCaptures)
		{
			const FPendingConsoleCapturePtr& Capture = Pair.Value;
			if (!Capture.IsValid())
			{
				RemoveIds.Add(Pair.Key);
				continue;
			}
			const FTimespan Age = Now - (Capture->bCompleted ? Capture->CompletedUtc : Capture->CreatedUtc);
			if (Capture->bCompleted && Age.GetTotalMinutes() > 10.0)
			{
				RemoveIds.Add(Pair.Key);
			}
		}
		for (const FString& Id : RemoveIds)
		{
			GPendingConsoleCaptures.Remove(Id);
		}
	}

	FPendingConsoleCapturePtr StartPendingConsoleCapture(
		const FString& CaptureCommand,
		const TMap<FString, FScreenshotFileSnapshot>& BeforeFiles,
		const FString& OutputPath,
		const FString& ResolvedOutputPath,
		const int32 CaptureWaitMs)
	{
		FPendingConsoleCapturePtr Capture = MakeShared<FPendingConsoleCapture, ESPMode::ThreadSafe>();
		Capture->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Capture->CaptureCommand = CaptureCommand;
		Capture->OutputPath = OutputPath;
		Capture->ResolvedOutputPath = ResolvedOutputPath;
		Capture->BeforeFiles = BeforeFiles;
		Capture->CaptureWaitMs = CaptureWaitMs;

		{
			FScopeLock Lock(&GPendingConsoleCaptureLock);
			TrimPendingConsoleCapturesLocked();
			GPendingConsoleCaptures.Add(Capture->Id, Capture);
		}

		Async(EAsyncExecution::ThreadPool, [Capture]()
		{
			const double StartSeconds = FPlatformTime::Seconds();
			FString CapturedPath;
			int32 WaitedMs = 0;
			while (WaitedMs <= Capture->CaptureWaitMs)
			{
				CapturedPath = FindNewestChangedPng(Capture->BeforeFiles);
				if (!CapturedPath.IsEmpty())
				{
					break;
				}
				if (WaitedMs == Capture->CaptureWaitMs)
				{
					break;
				}
				const int32 StepMs = FMath::Min(250, Capture->CaptureWaitMs - WaitedMs);
				FPlatformProcess::Sleep(static_cast<float>(StepMs) / 1000.0f);
				WaitedMs = FMath::Min(Capture->CaptureWaitMs, static_cast<int32>((FPlatformTime::Seconds() - StartSeconds) * 1000.0));
			}

			bool bPassed = false;
			FString Status = TEXT("capture_not_found");
			FString Warning;
			int32 CopyResult = COPY_OK;
			if (!CapturedPath.IsEmpty())
			{
				ApplyCaptureOutputPath(CapturedPath, Capture->ResolvedOutputPath, bPassed, Status, CopyResult);
			}
			else
			{
				Warning = TEXT("No new or modified PNG appeared under Saved/Screenshots after the capture command.");
			}

			{
				FScopeLock Lock(&GPendingConsoleCaptureLock);
				Capture->CapturedPath = CapturedPath;
				Capture->Status = Status;
				Capture->Warning = Warning;
				Capture->CaptureWaitedMs = FMath::Clamp(WaitedMs, 0, Capture->CaptureWaitMs);
				Capture->CopyResult = CopyResult;
				Capture->bPassed = bPassed;
				Capture->bCompleted = true;
				Capture->CompletedUtc = FDateTime::UtcNow();
			}
		});

		return Capture;
	}

	bool JsonObjectToString(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		OutJson.Reset();
		if (!Object.IsValid())
		{
			return false;
		}
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	FString ResolveArtifactDirectory(const FString& ArtifactDir)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(ArtifactDir) ? FPaths::ProjectDir() / ArtifactDir : ArtifactDir);
	}

	bool WriteJsonObjectFile(const FString& Path, const TSharedPtr<FJsonObject>& Object, FString& OutError)
	{
		FString Json;
		if (!JsonObjectToString(Object, Json))
		{
			OutError = TEXT("Failed to serialize JSON object.");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write JSON file: %s"), *Path);
			return false;
		}
		return true;
	}

	bool WriteSequenceLogJsonl(const FString& Path, const TArray<TSharedPtr<FJsonValue>>& StepResults, FString& OutError)
	{
		FString Body;
		for (const TSharedPtr<FJsonValue>& StepValue : StepResults)
		{
			const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			if (!Step.IsValid())
			{
				continue;
			}

			const int32 StepIndex = static_cast<int32>(Step->GetIntegerField(TEXT("index")));
			const TArray<TSharedPtr<FJsonValue>>* Logs = nullptr;
			if (!Step->TryGetArrayField(TEXT("logs"), Logs) || !Logs)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& LogValue : *Logs)
			{
				const TSharedPtr<FJsonObject> LogJsonObject = LogValue.IsValid() ? LogValue->AsObject() : nullptr;
				if (!LogJsonObject.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("step_index"), StepIndex);
				Row->SetStringField(TEXT("command"), Step->GetStringField(TEXT("command")));
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : LogJsonObject->Values)
				{
					Row->SetField(Pair.Key, Pair.Value);
				}
				FString Line;
				if (JsonObjectToString(Row, Line))
				{
					Body += Line;
					Body += TEXT("\n");
				}
			}
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(Body, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write JSONL file: %s"), *Path);
			return false;
		}
		return true;
	}

	void ApplyDefaultCategory(TArray<FConsoleLogExpectation>& Expectations, const FString& Category)
	{
		if (Category.IsEmpty())
		{
			return;
		}
		for (FConsoleLogExpectation& Expectation : Expectations)
		{
			if (Expectation.Category.IsEmpty())
			{
				Expectation.Category = Category;
			}
		}
	}

	bool HasRejectedMatch(const TArray<FMonolithLogEntry>& Entries, const TArray<FConsoleLogExpectation>& Rejected)
	{
		for (const FConsoleLogExpectation& Expectation : Rejected)
		{
			for (const FMonolithLogEntry& Entry : Entries)
			{
				if (EntryMatchesExpectation(Entry, Expectation))
				{
					return true;
				}
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> ResolveCommandToJson(const FString& Command, const FString& TargetWorld, bool bIncludeValues, bool bIncludeDefaults)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString Trimmed = Command.TrimStartAndEnd();
		const FString FirstToken = FirstConsoleToken(Trimmed);
		Root->SetStringField(TEXT("command"), Trimmed);
		Root->SetStringField(TEXT("first_token"), FirstToken);
		Root->SetStringField(TEXT("target_world"), TargetWorld);

		TOptional<FMonolithConsoleObjectRow> Row = FindLiveConsoleObject(FirstToken, bIncludeValues, bIncludeDefaults);
		Root->SetBoolField(TEXT("known_object"), Row.IsSet());
		if (Row.IsSet())
		{
			Root->SetStringField(TEXT("object_type"), Row.GetValue().ObjectType);
			Root->SetObjectField(TEXT("object"), ConsoleRowToJson(Row.GetValue()));
		}

		FString WorldType;
		UWorld* World = nullptr;
		RunOnGameThreadBlocking([&]()
		{
			World = ResolveConsoleWorld(TargetWorld, WorldType);
		});
		Root->SetBoolField(TEXT("world_available"), World != nullptr);
		Root->SetStringField(TEXT("world"), WorldType);
		Root->SetBoolField(TEXT("can_execute"), !Trimmed.IsEmpty() && World != nullptr);
		if (Trimmed.IsEmpty())
		{
			Root->SetStringField(TEXT("status"), TEXT("invalid_command"));
			Root->SetStringField(TEXT("summary"), TEXT("Command is empty after trimming."));
		}
		else if (!World)
		{
			Root->SetStringField(TEXT("status"), TEXT("world_unavailable"));
			Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("No usable world found for target_world=%s."), *TargetWorld));
		}
		else
		{
			Root->SetStringField(TEXT("status"), Row.IsSet() ? TEXT("resolved") : TEXT("unknown_object"));
			Root->SetStringField(TEXT("summary"), Row.IsSet()
				? TEXT("First token resolves to a live IConsoleManager object.")
				: TEXT("First token does not resolve to a live IConsoleManager object; execute can still run if require_known_object=false."));
		}
		return Root;
	}

	FString JsonValueToConsoleString(const TSharedPtr<FJsonValue>& Value, bool& bOutOk)
	{
		bOutOk = false;
		if (!Value.IsValid())
		{
			return TEXT("");
		}
		switch (Value->Type)
		{
		case EJson::String:
		{
			FString StringValue;
			bOutOk = Value->TryGetString(StringValue);
			return StringValue;
		}
		case EJson::Number:
		{
			double NumberValue = 0.0;
			bOutOk = Value->TryGetNumber(NumberValue);
			return FString::SanitizeFloat(NumberValue);
		}
		case EJson::Boolean:
		{
			bool BoolValue = false;
			bOutOk = Value->TryGetBool(BoolValue);
			return BoolValue ? TEXT("1") : TEXT("0");
		}
		default:
			return TEXT("");
		}
	}

	struct FScopedCvarRecord
	{
		FString Name;
		FString RequestedValue;
		FString OriginalValue;
		FString RestoredValue;
		EConsoleVariableFlags OriginalSetBy = ECVF_SetByConstructor;
		EConsoleVariableFlags RestoredSetBy = ECVF_SetByConstructor;
		FString OriginalSetByName;
		FString RestoredSetByName;
		bool bValidated = false;
		bool bSet = false;
		bool bRestored = false;
		FString Error;
	};

	bool CaptureConsoleVariableForScope(FScopedCvarRecord& Record)
	{
		bool bOk = false;
		RunOnGameThreadBlocking([&]()
		{
			IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Record.Name);
			IConsoleVariable* Variable = Object ? Object->AsVariable() : nullptr;
			if (!Variable)
			{
				Record.Error = FString::Printf(TEXT("Console object is not a variable or does not exist: %s"), *Record.Name);
				return;
			}
			if (Variable->TestFlags(ECVF_ReadOnly))
			{
				Record.Error = FString::Printf(TEXT("Console variable is read-only and cannot be scoped: %s"), *Record.Name);
				return;
			}
			Record.OriginalValue = Variable->GetString();
			Record.OriginalSetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
			Record.OriginalSetByName = GetConsoleVariableSetByName(Record.OriginalSetBy);
			Record.bValidated = true;
			bOk = true;
		});
		return bOk;
	}

	bool SetConsoleVariableString(FScopedCvarRecord& Record)
	{
		bool bOk = false;
		RunOnGameThreadBlocking([&]()
		{
			IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Record.Name);
			IConsoleVariable* Variable = Object ? Object->AsVariable() : nullptr;
			if (!Variable)
			{
				Record.Error = FString::Printf(TEXT("Console variable disappeared before scoped set: %s"), *Record.Name);
				return;
			}
			Variable->Set(*Record.RequestedValue, ECVF_SetByConsole);
			Record.bSet = true;
			bOk = true;
		});
		return bOk;
	}

	bool RestoreConsoleVariableString(FScopedCvarRecord& Record)
	{
		bool bOk = false;
		RunOnGameThreadBlocking([&]()
		{
			IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Record.Name);
			IConsoleVariable* Variable = Object ? Object->AsVariable() : nullptr;
			if (!Variable)
			{
				Record.Error = FString::Printf(TEXT("Console variable disappeared before restore: %s"), *Record.Name);
				return;
			}
			if (Record.OriginalSetBy != ECVF_SetByConsole)
			{
				Variable->Unset(ECVF_SetByConsole);
			}
			Variable->Set(*Record.OriginalValue, Record.OriginalSetBy);
			Record.RestoredValue = Variable->GetString();
			Record.RestoredSetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
			Record.RestoredSetByName = GetConsoleVariableSetByName(Record.RestoredSetBy);
			Record.bRestored = Record.RestoredValue.Equals(Record.OriginalValue, ESearchCase::CaseSensitive) &&
				Record.RestoredSetBy == Record.OriginalSetBy;
			if (!Record.bRestored)
			{
				Record.Error = FString::Printf(
					TEXT("Console variable restore mismatch for %s: value %s -> %s, set_by %s -> %s"),
					*Record.Name,
					*Record.OriginalValue,
					*Record.RestoredValue,
					*Record.OriginalSetByName,
					*Record.RestoredSetByName);
				return;
			}
			bOk = true;
		});
		return bOk;
	}

	TArray<TSharedPtr<FJsonValue>> ScopedCvarRecordsToJson(const TArray<FScopedCvarRecord>& Records)
	{
		TArray<TSharedPtr<FJsonValue>> CvarsJson;
		CvarsJson.Reserve(Records.Num());
		for (const FScopedCvarRecord& Record : Records)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Record.Name);
			Obj->SetStringField(TEXT("requested_value"), Record.RequestedValue);
			Obj->SetStringField(TEXT("original_value"), Record.OriginalValue);
			Obj->SetStringField(TEXT("restored_value"), Record.RestoredValue);
			Obj->SetStringField(TEXT("original_set_by"), Record.OriginalSetByName);
			Obj->SetStringField(TEXT("restored_set_by"), Record.RestoredSetByName);
			Obj->SetNumberField(TEXT("original_set_by_flags"), static_cast<int32>(Record.OriginalSetBy));
			Obj->SetNumberField(TEXT("restored_set_by_flags"), static_cast<int32>(Record.RestoredSetBy));
			Obj->SetBoolField(TEXT("validated"), Record.bValidated);
			Obj->SetBoolField(TEXT("set"), Record.bSet);
			Obj->SetBoolField(TEXT("restored"), Record.bRestored);
			if (!Record.Error.IsEmpty())
			{
				Obj->SetStringField(TEXT("error"), Record.Error);
			}
			CvarsJson.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return CvarsJson;
	}

	void AddDiagnosis(TArray<TSharedPtr<FJsonValue>>& Causes, const FString& Code, const FString& Detail, const FString& NextAction = TEXT(""))
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("detail"), Detail);
		if (!NextAction.IsEmpty())
		{
			Obj->SetStringField(TEXT("next_action"), NextAction);
		}
		Causes.Add(MakeShared<FJsonValueObject>(Obj));
	}

	void DiagnoseStepObject(const TSharedPtr<FJsonObject>& Step, TArray<TSharedPtr<FJsonValue>>& Causes)
	{
		if (!Step.IsValid())
		{
			return;
		}
		const int32 Index = Step->HasField(TEXT("index")) ? static_cast<int32>(Step->GetIntegerField(TEXT("index"))) : -1;
		const FString Prefix = Index >= 0 ? FString::Printf(TEXT("step %d: "), Index) : FString();
		if (Step->HasField(TEXT("error")))
		{
			const FString Error = Step->GetStringField(TEXT("error"));
			if (Error.Contains(TEXT("registered console object")))
			{
				AddDiagnosis(Causes, TEXT("unknown_console_object"), Prefix + Error, TEXT("console.search_objects or console.resolve_command"));
			}
			else if (Error.Contains(TEXT("No live PIE")))
			{
				AddDiagnosis(Causes, TEXT("pie_world_missing"), Prefix + Error, TEXT("editor.start_pie"));
			}
			else if (Error.Contains(TEXT("No editor world")))
			{
				AddDiagnosis(Causes, TEXT("editor_world_missing"), Prefix + Error, TEXT("monolith_status"));
			}
			else
			{
				AddDiagnosis(Causes, TEXT("execution_error"), Prefix + Error, TEXT("console.resolve_command"));
			}
		}

		const TSharedPtr<FJsonObject>* ExpectationsPtr = nullptr;
		const TSharedPtr<FJsonObject> Expectations = Step->TryGetObjectField(TEXT("expectations"), ExpectationsPtr) && ExpectationsPtr
			? *ExpectationsPtr
			: nullptr;
		if (Expectations.IsValid() && Expectations->HasField(TEXT("passed")) && !Expectations->GetBoolField(TEXT("passed")))
		{
			AddDiagnosis(Causes, TEXT("log_expectation_failed"), Prefix + TEXT("Expected/rejected log patterns did not pass."), TEXT("console.wait_for_log"));
		}

		const TSharedPtr<FJsonObject>* CapturePtr = nullptr;
		const TSharedPtr<FJsonObject> Capture = Step->TryGetObjectField(TEXT("capture"), CapturePtr) && CapturePtr
			? *CapturePtr
			: nullptr;
		if (Capture.IsValid() && Capture->HasField(TEXT("passed")) && !Capture->GetBoolField(TEXT("passed")))
		{
			const FString Status = Capture->HasField(TEXT("status")) ? Capture->GetStringField(TEXT("status")) : TEXT("unknown");
			AddDiagnosis(Causes,
				Status == TEXT("capture_pending") ? TEXT("capture_pending") : TEXT("capture_failed"),
				Prefix + Status,
				Status == TEXT("capture_pending") ? TEXT("console.poll_capture") : TEXT("console.execute_and_capture"));
		}
	}
}

void FMonolithConsoleActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("console"), TEXT("list_live_objects"),
		TEXT("List live IConsoleManager objects from the running editor. Includes variables and commands, with optional prefix/contains filtering."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::ListLiveObjects),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("Prefix or substring to match against console object names. Empty lists every registered object."))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode: prefix or contains."), TEXT("prefix"))
			.Optional(TEXT("object_type"), TEXT("string"), TEXT("Filter: all, variable, or command."), TEXT("all"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum objects to return, clamped to 1..5000."), TEXT("100"))
			.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Include current values for console variables. Default true."), TEXT("true"))
			.Optional(TEXT("include_defaults"), TEXT("boolean"), TEXT("Include default values for console variables. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("refresh_snapshot"),
		TEXT("Refresh EngineSource.db console_objects and console_objects_fts from the live IConsoleManager registry."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::RefreshSnapshot),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("Optional prefix or substring to restrict the captured registry rows. Empty captures every registered object."))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode for query: prefix or contains."), TEXT("prefix"))
			.Optional(TEXT("object_type"), TEXT("string"), TEXT("Filter captured object type: all, variable, or command."), TEXT("all"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum objects to capture. 0 means no explicit cap."), TEXT("0"))
			.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Persist current values for console variables. Default true."), TEXT("true"))
			.Optional(TEXT("include_defaults"), TEXT("boolean"), TEXT("Persist default values for console variables. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("search_objects"),
		TEXT("Search console objects from the latest EngineSource.db snapshot using FTS5."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::SearchObjects),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("FTS query over name, type, help, values, variable type, and set-by source. Empty lists snapshot rows by name."))
			.Optional(TEXT("object_type"), TEXT("string"), TEXT("Filter: all, variable, or command."), TEXT("all"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum objects to return. Compact projection is capped at 500 rows; full/detail projection is capped at 200 rows."), TEXT("100"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset for pagination."), TEXT("0"))
			.Optional(TEXT("cursor"), TEXT("string"), TEXT("Numeric next_cursor returned by a previous page; overrides offset when supplied."))
			.Optional(TEXT("detail"), TEXT("boolean"), TEXT("Include full help/value/default fields. Default false returns compact search rows."), TEXT("false"))
			.Optional(TEXT("projection"), TEXT("string"), TEXT("Result projection: compact omits full help/value/default rows; full returns every snapshotted row field."), TEXT("compact"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("get_object"),
		TEXT("Get one console object by exact name from the snapshot or live IConsoleManager registry."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::GetObject),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Exact console object name, for example r.ShadowQuality."))
			.Optional(TEXT("source"), TEXT("string"), TEXT("snapshot or live."), TEXT("snapshot"))
			.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("When source=live, include current variable value. Default true."), TEXT("true"))
			.Optional(TEXT("include_defaults"), TEXT("boolean"), TEXT("When source=live, include variable default value. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("health"),
		TEXT("Report EngineSource.db console snapshot schema, FTS parity, and last refresh metadata."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::Health),
		FParamSchemaBuilder()
			.Optional(TEXT("include_counts"), TEXT("boolean"), TEXT("Include row/FTS parity counts. Default false."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("resolve_command"),
		TEXT("Resolve a console command string before execution: parse first token, read the live IConsoleManager object, and report target-world availability."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::ResolveCommand),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command string to inspect without executing."))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Execution target to validate: auto, pie, or editor. Default auto."), TEXT("auto"))
			.Optional(TEXT("include_values"), TEXT("boolean"), TEXT("Include current variable value when the first token is a CVar. Default true."), TEXT("true"))
			.Optional(TEXT("include_defaults"), TEXT("boolean"), TEXT("Include default variable value when available. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("execute"),
		TEXT("Execute a live console command. Routes to the first PIE PlayerController when available and falls back to GEngine->Exec on the editor world unless target_world is constrained."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::Execute),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command string, including optional arguments."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate and report routing without executing. Default true."), TEXT("true"))
			.Optional(TEXT("require_known_object"), TEXT("boolean"), TEXT("Require the first command token to resolve in IConsoleManager before execution. Default true."), TEXT("true"))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Execution target: auto, pie, or editor. Use pie to require a live game/PIE world. Default auto."), TEXT("auto"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("get_log_cursor"),
		TEXT("Return the current Monolith log-capture cursor. Pass it to console.search_logs_since or expectation actions to isolate logs emitted after this point."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::GetLogCursor),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("console"), TEXT("search_logs_since"),
		TEXT("Search only Monolith-captured log entries emitted after a console log cursor."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::SearchLogsSince),
		FParamSchemaBuilder()
			.Required(TEXT("cursor"), TEXT("number"), TEXT("Log cursor returned by console.get_log_cursor or a prior console verification action."))
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Case-insensitive substring match against log message. Empty returns all matching category/verbosity rows."))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Optional exact log category filter."))
			.Optional(TEXT("verbosity"), TEXT("string"), TEXT("Maximum verbosity: fatal, error, warning, display, log, verbose, very_verbose. Default very_verbose."), TEXT("very_verbose"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum log entries to return, clamped to 1..2000."), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("wait_for_log"),
		TEXT("Wait until expected post-cursor log patterns appear, or until rejected patterns appear or the timeout expires."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::WaitForLog),
		FParamSchemaBuilder()
			.Required(TEXT("cursor"), TEXT("number"), TEXT("Log cursor returned by console.get_log_cursor or a prior console verification action."))
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Shortcut expected log substring. Equivalent to one expect_log entry."))
			.Optional(TEXT("expect_log"), TEXT("string"), TEXT("Single expected log substring."))
			.Optional(TEXT("expect_logs"), TEXT("array"), TEXT("Expected log substrings or objects {pattern, category?, min_count?, max_count?}."))
			.Optional(TEXT("reject_log"), TEXT("string"), TEXT("Single rejected log substring; any match fails immediately."))
			.Optional(TEXT("reject_logs"), TEXT("array"), TEXT("Rejected log substrings or objects {pattern, category?}."))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("expect or assert_absent. Reject-only waits require assert_absent so absence assertions cannot be confused with completion waits."), TEXT("expect"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Default category filter applied to expectations without their own category."))
			.Optional(TEXT("verbosity"), TEXT("string"), TEXT("Maximum verbosity: fatal, error, warning, display, log, verbose, very_verbose. Default very_verbose."), TEXT("very_verbose"))
			.Optional(TEXT("timeout_ms"), TEXT("integer"), TEXT("Maximum wait time. Default 3000, max 30000."), TEXT("3000"))
			.Optional(TEXT("poll_interval_ms"), TEXT("integer"), TEXT("Polling interval. Default 100, clamped to 25..1000."), TEXT("100"))
			.Optional(TEXT("log_limit"), TEXT("integer"), TEXT("Maximum post-cursor log entries to inspect/return. Default 200, max 2000."), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("execute_and_expect"),
		TEXT("Execute one console command and evaluate expected/rejected log patterns emitted after the command cursor."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::ExecuteAndExpect),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command string, including optional arguments."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate and report routing without executing. Default true."), TEXT("true"))
			.Optional(TEXT("require_known_object"), TEXT("boolean"), TEXT("Require the first command token to resolve in IConsoleManager before execution. Default true."), TEXT("true"))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Execution target: auto, pie, or editor. Default auto."), TEXT("auto"))
			.Optional(TEXT("expect_log"), TEXT("string"), TEXT("Single expected log substring."))
			.Optional(TEXT("expect_logs"), TEXT("array"), TEXT("Expected log substrings or objects {pattern, category?, min_count?, max_count?}."))
			.Optional(TEXT("reject_log"), TEXT("string"), TEXT("Single rejected log substring; any match fails expectations."))
			.Optional(TEXT("reject_logs"), TEXT("array"), TEXT("Rejected log substrings or objects {pattern, category?}; any match fails expectations."))
			.Optional(TEXT("settle_ms"), TEXT("integer"), TEXT("Milliseconds to wait after execution before collecting logs. Default 100, max 5000."), TEXT("100"))
			.Optional(TEXT("log_limit"), TEXT("integer"), TEXT("Maximum post-command log entries to inspect/return. Default 200, max 2000."), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("run_sequence"),
		TEXT("Run a bounded sequence of console commands with per-step log expectations and stable post-step cursors."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::RunSequence),
		FParamSchemaBuilder()
			.Required(TEXT("commands"), TEXT("array"), TEXT("Array of command strings or step objects {command, dry_run?, require_known_object?, target_world?, expect_log?, expect_logs?, reject_log?, reject_logs?, settle_ms?, log_limit?, capture?, capture_command?, capture_wait_ms?, capture_output_path?}."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Default dry-run value for steps. Default true."), TEXT("true"))
			.Optional(TEXT("require_known_object"), TEXT("boolean"), TEXT("Default known-object guard for steps. Default true."), TEXT("true"))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Default execution target: auto, pie, or editor. Default auto."), TEXT("auto"))
			.Optional(TEXT("abort_on_failure"), TEXT("boolean"), TEXT("Stop after the first failed execution or expectation. Default true."), TEXT("true"))
			.Optional(TEXT("settle_ms"), TEXT("integer"), TEXT("Default milliseconds to wait after each command. Default 100, max 5000."), TEXT("100"))
			.Optional(TEXT("log_limit"), TEXT("integer"), TEXT("Maximum per-step post-command log entries to inspect/return. Default 200, max 2000."), TEXT("200"))
			.Optional(TEXT("artifact_dir"), TEXT("string"), TEXT("Optional directory for manifest.json and logs.jsonl. Relative paths resolve under the project root."))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("execute_and_capture"),
		TEXT("Execute a console command, run a screenshot console command such as HighResShot, and report the newly-created or modified PNG path without falling back to editor viewport capture. If the engine must finish the screenshot on a later game tick, returns capture_pending with a capture_id for console.poll_capture."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::ExecuteAndCapture),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command to execute before capture."))
			.Optional(TEXT("capture_command"), TEXT("string"), TEXT("Console screenshot command. Default HighResShot 1920x1080."), TEXT("HighResShot 1920x1080"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Optional path to copy the captured PNG to after the engine writes it. Relative paths resolve under the project root."))
			.Optional(TEXT("require_known_object"), TEXT("boolean"), TEXT("Require the first token of the main command to resolve in IConsoleManager. Default true."), TEXT("true"))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Execution target: auto, pie, or editor. Default auto."), TEXT("auto"))
			.Optional(TEXT("settle_ms"), TEXT("integer"), TEXT("Milliseconds to wait between command and capture. Default 250, max 5000."), TEXT("250"))
			.Optional(TEXT("capture_wait_ms"), TEXT("integer"), TEXT("Milliseconds to wait for the screenshot file after capture. Default 120000, max 240000."), TEXT("120000"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("poll_capture"),
		TEXT("Poll a pending console.execute_and_capture request by capture_id, returning capture_pending, captured, capture_not_found, or copy_failed."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::PollCapture),
		FParamSchemaBuilder()
			.Required(TEXT("capture_id"), TEXT("string"), TEXT("Capture id returned by console.execute_and_capture when status is capture_pending."))
			.Optional(TEXT("consume"), TEXT("boolean"), TEXT("Remove the pending-capture record after it reaches a completed status. Default false."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("diagnose_failure"),
		TEXT("Classify a failed console action result and suggest the next console/editor action to run."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::DiagnoseFailure),
		FParamSchemaBuilder()
			.Required(TEXT("result"), TEXT("object"), TEXT("A result object returned by console.execute_and_expect, console.run_sequence, wait_for_log, execute_and_capture, or poll_capture."))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("set_cvar_scoped"),
		TEXT("Temporarily set one or more live console variables, run a console command sequence, then restore the original values even when the sequence fails."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::SetCvarScoped),
		FParamSchemaBuilder()
			.Required(TEXT("cvars"), TEXT("object"), TEXT("Map of console variable name to temporary string/number/bool value."))
			.Required(TEXT("commands"), TEXT("array"), TEXT("Command strings or step objects to run while the CVar values are applied."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Default dry-run value for command steps. Default false."), TEXT("false"))
			.Optional(TEXT("require_known_object"), TEXT("boolean"), TEXT("Default known-object guard for command steps. Default true."), TEXT("true"))
			.Optional(TEXT("target_world"), TEXT("string"), TEXT("Default execution target: auto, pie, or editor. Default auto."), TEXT("auto"))
			.Optional(TEXT("abort_on_failure"), TEXT("boolean"), TEXT("Stop command sequence after the first failed step. Default true."), TEXT("true"))
			.Optional(TEXT("settle_ms"), TEXT("integer"), TEXT("Default milliseconds to wait after each command. Default 100, max 5000."), TEXT("100"))
			.Optional(TEXT("log_limit"), TEXT("integer"), TEXT("Maximum per-step logs. Default 200, max 2000."), TEXT("200"))
			.Optional(TEXT("artifact_dir"), TEXT("string"), TEXT("Optional sequence artifact directory, forwarded to run_sequence."))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("list_live_objects"),
		{ TEXT("console registry"), TEXT("cvar command list"), TEXT("IConsoleManager"), TEXT("live console objects") },
		{ TEXT("list console"), TEXT("list cvars"), TEXT("console objects") },
		{ TEXT("list live console commands"), TEXT("show r.Shadow console objects") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("search_objects"),
		{ TEXT("search console commands"), TEXT("search cvars"), TEXT("console FTS"), TEXT("command help") },
		{ TEXT("find console"), TEXT("find cvar"), TEXT("console search") },
		{ TEXT("search console objects for shadow quality"), TEXT("find console commands mentioning stat") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("execute"),
		{ TEXT("run console command"), TEXT("exec command"), TEXT("set cvar"), TEXT("stat fps") },
		{ TEXT("exec"), TEXT("console command"), TEXT("cmd") },
		{ TEXT("execute stat unit"), TEXT("run r.ShadowQuality 3") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("resolve_command"),
		{ TEXT("validate console command"), TEXT("known console object"), TEXT("dry run inspect"), TEXT("target world availability") },
		{ TEXT("resolve command"), TEXT("check command"), TEXT("console preflight") },
		{ TEXT("resolve Project.Debug.DumpState before execution"), TEXT("check whether r.ShadowQuality is a known CVar") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("execute_and_expect"),
		{ TEXT("console command verification"), TEXT("expect log"), TEXT("assert console result"), TEXT("runtime smoke") },
		{ TEXT("exec expect"), TEXT("console assert"), TEXT("run and check log") },
		{ TEXT("execute Project.Debug.JumpToCheckpoint and expect a gameplay log"), TEXT("run stat fps and reject warnings") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("run_sequence"),
		{ TEXT("console command sequence"), TEXT("batch console"), TEXT("scenario smoke"), TEXT("runtime verification") },
		{ TEXT("console batch"), TEXT("command sequence"), TEXT("run scenario") },
		{ TEXT("run project gameplay console smoke sequence in PIE"), TEXT("run several cvar commands with expectations") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("search_logs_since"),
		{ TEXT("log cursor"), TEXT("logs since command"), TEXT("post command logs") },
		{ TEXT("logs since"), TEXT("cursor logs"), TEXT("new logs") },
		{ TEXT("search logs since cursor for Project.Debug"), TEXT("return only logs emitted after a command") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("wait_for_log"),
		{ TEXT("wait for log"), TEXT("poll log cursor"), TEXT("async console verification"), TEXT("timeout log expectation") },
		{ TEXT("wait log"), TEXT("poll logs"), TEXT("wait for expected log") },
		{ TEXT("wait up to 3 seconds for Project.Debug.Select log"), TEXT("wait for warning after command") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("execute_and_capture"),
		{ TEXT("console screenshot"), TEXT("HighResShot"), TEXT("capture after command"), TEXT("runtime proof") },
		{ TEXT("exec capture"), TEXT("command screenshot") },
		{ TEXT("execute Project.Debug.ShowState then HighResShot 1920x1080") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("diagnose_failure"),
		{ TEXT("console failure diagnosis"), TEXT("why command failed"), TEXT("expectation failure next action") },
		{ TEXT("diagnose console"), TEXT("explain console failure") },
		{ TEXT("diagnose a failed run_sequence result"), TEXT("classify unknown command or missing PIE") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("console"), TEXT("set_cvar_scoped"),
		{ TEXT("temporary cvar"), TEXT("restore cvar"), TEXT("scoped console variable"), TEXT("safe cvar test") },
		{ TEXT("scoped cvar"), TEXT("set and restore cvar") },
		{ TEXT("set r.ScreenPercentage during a smoke sequence then restore it") });
}

FMonolithActionResult FMonolithConsoleActions::ListLiveObjects(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FString Mode = TEXT("prefix");
	FString ObjectType = TEXT("all");
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("query")) && !Params->TryGetStringField(TEXT("query"), Query))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: query must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("mode")) && !Params->TryGetStringField(TEXT("mode"), Mode))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: mode must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("object_type")) && !Params->TryGetStringField(TEXT("object_type"), ObjectType))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: object_type must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	const int32 Limit = ReadLimit(Params);
	const bool bIncludeValues = ReadBool(Params, TEXT("include_values"), true);
	const bool bIncludeDefaults = ReadBool(Params, TEXT("include_defaults"), true);
	int32 Matched = 0;
	TArray<FMonolithConsoleObjectRow> Rows = CollectConsoleObjects(
		Query,
		Mode,
		ObjectType,
		Limit,
		bIncludeValues,
		bIncludeDefaults,
		Matched);
	return FMonolithActionResult::Success(MakeObjectArrayResult(Rows, Query, Mode, ObjectType, Matched, Limit));
}

FMonolithActionResult FMonolithConsoleActions::RefreshSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FString Mode = TEXT("prefix");
	FString ObjectType = TEXT("all");
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("query")) && !Params->TryGetStringField(TEXT("query"), Query))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: query must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("mode")) && !Params->TryGetStringField(TEXT("mode"), Mode))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: mode must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("object_type")) && !Params->TryGetStringField(TEXT("object_type"), ObjectType))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: object_type must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	const bool bIncludeValues = ReadBool(Params, TEXT("include_values"), true);
	const bool bIncludeDefaults = ReadBool(Params, TEXT("include_defaults"), true);
	const int32 Limit = ReadOptionalLimit(Params);

	int32 Matched = 0;
	TArray<FMonolithConsoleObjectRow> Rows = CollectConsoleObjects(
		Query,
		Mode,
		ObjectType,
		Limit,
		bIncludeValues,
		bIncludeDefaults,
		Matched);

	FString Error;
	FMonolithSourceDatabase* Database = GetSourceDatabase(Error);
	if (!Database)
	{
		return SourceDatabaseError(Error);
	}

	TSharedPtr<FJsonObject> Result = Database->ReplaceConsoleObjectSnapshot(Rows, TEXT("IConsoleManager"));
	Result->SetStringField(TEXT("query"), Query);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetNumberField(TEXT("matched_count"), Matched);
	Result->SetStringField(TEXT("object_type"), NormalizeObjectType(ObjectType));
	Result->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
	if (Matched > Rows.Num())
	{
		Result->SetStringField(TEXT("warning"), TEXT("Snapshot matched more console objects than the requested limit; repeat with limit=0 for a full snapshot."));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithConsoleActions::SearchObjects(const TSharedPtr<FJsonObject>& Params)
{
	constexpr int32 MaxConsoleSearchWindow = 100000;
	FString Query;
	FString ObjectType = TEXT("all");
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("query")) && !Params->TryGetStringField(TEXT("query"), Query))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: query must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("object_type")) && !Params->TryGetStringField(TEXT("object_type"), ObjectType))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: object_type must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	const int32 Limit = ReadLimit(Params);
	int32 Offset = 0;
	if (Params.IsValid() && Params->HasField(TEXT("offset")))
	{
		double RawOffset = 0.0;
		if (!Params->HasTypedField<EJson::Number>(TEXT("offset")) || !Params->TryGetNumberField(TEXT("offset"), RawOffset))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (RawOffset < 0.0 || RawOffset > static_cast<double>(MaxConsoleSearchWindow))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be within 0..100000"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Offset = static_cast<int32>(RawOffset);
	}
	bool bDetail = false;
	if (Params.IsValid() && Params->HasField(TEXT("detail")))
	{
		if (!Params->HasTypedField<EJson::Boolean>(TEXT("detail")) || !Params->TryGetBoolField(TEXT("detail"), bDetail))
		{
			return FMonolithActionResult::Error(TEXT("'detail' parameter must be a bool"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	FString Projection = bDetail ? TEXT("full") : TEXT("compact");
	if (Params.IsValid() && Params->HasField(TEXT("projection")))
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("projection")) || !Params->TryGetStringField(TEXT("projection"), Projection))
		{
			return FMonolithActionResult::Error(TEXT("'projection' parameter must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Projection = Projection.ToLower();
		if (Projection == TEXT("full"))
		{
			bDetail = true;
		}
		else if (Projection == TEXT("compact"))
		{
			bDetail = false;
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("'projection' parameter must be 'compact' or 'full'"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	if (Params.IsValid() && Params->HasField(TEXT("cursor")))
	{
		FString Cursor;
		if (!Params->HasTypedField<EJson::String>(TEXT("cursor")) || !Params->TryGetStringField(TEXT("cursor"), Cursor))
		{
			return FMonolithActionResult::Error(TEXT("'cursor' parameter must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Cursor.TrimStartAndEndInline();
		if (Cursor.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'cursor' parameter must be a non-negative numeric offset cursor"), FMonolithJsonUtils::ErrInvalidParams);
		}
		for (const TCHAR Ch : Cursor)
		{
			if (!FChar::IsDigit(Ch))
			{
				return FMonolithActionResult::Error(TEXT("'cursor' parameter must be a non-negative numeric offset cursor"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
		const int64 ParsedOffset = FCString::Atoi64(*Cursor);
		if (ParsedOffset < 0 || ParsedOffset > MaxConsoleSearchWindow)
		{
			return FMonolithActionResult::Error(TEXT("'cursor' parameter must be within 0..100000"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Offset = static_cast<int32>(ParsedOffset);
	}

	FString Error;
	FMonolithSourceDatabase* Database = GetSourceDatabase(Error);
	if (!Database)
	{
		return SourceDatabaseError(Error);
	}

	return FMonolithActionResult::Success(Database->SearchConsoleObjects(Query, NormalizeObjectType(ObjectType), Limit, bDetail, Offset));
}

FMonolithActionResult FMonolithConsoleActions::GetObject(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: name"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Source = TEXT("snapshot");
	if (Params->HasField(TEXT("source")) && !Params->TryGetStringField(TEXT("source"), Source))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: source must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	if (Source.Equals(TEXT("live"), ESearchCase::IgnoreCase))
	{
		const bool bIncludeValues = ReadBool(Params, TEXT("include_values"), true);
		const bool bIncludeDefaults = ReadBool(Params, TEXT("include_defaults"), true);
		TOptional<FMonolithConsoleObjectRow> Row = FindLiveConsoleObject(Name, bIncludeValues, bIncludeDefaults);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("name"), Name);
		Root->SetStringField(TEXT("source"), TEXT("live"));
		if (Row.IsSet())
		{
			Root->SetStringField(TEXT("status"), TEXT("ok"));
			Root->SetBoolField(TEXT("ok"), true);
			Root->SetObjectField(TEXT("object"), ConsoleRowToJson(Row.GetValue()));
		}
		else
		{
			Root->SetStringField(TEXT("status"), TEXT("not_found"));
			Root->SetBoolField(TEXT("ok"), false);
			Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("Live console object not found: %s"), *Name));
		}
		return FMonolithActionResult::Success(Root);
	}

	FString Error;
	FMonolithSourceDatabase* Database = GetSourceDatabase(Error);
	if (!Database)
	{
		return SourceDatabaseError(Error);
	}
	return FMonolithActionResult::Success(Database->GetConsoleObject(Name));
}

FMonolithActionResult FMonolithConsoleActions::Health(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FMonolithSourceDatabase* Database = GetSourceDatabase(Error);
	if (!Database)
	{
		return SourceDatabaseError(Error);
	}
	return FMonolithActionResult::Success(Database->ComputeConsoleHealth(ReadBool(Params, TEXT("include_counts"), false)));
}

FMonolithActionResult FMonolithConsoleActions::ResolveCommand(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString TargetWorld = TEXT("auto");
	if (!ReadStringField(Params, TEXT("target_world"), TargetWorld, TEXT("auto")))
	{
		return FMonolithActionResult::Error(TEXT("target_world must be a string when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}
	TargetWorld = NormalizeWorldTarget(TargetWorld);
	FString TargetError;
	if (!ValidateWorldTarget(TargetWorld, TargetError))
	{
		return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
	}

	return FMonolithActionResult::Success(ResolveCommandToJson(
		Command,
		TargetWorld,
		ReadBool(Params, TEXT("include_values"), true),
		ReadBool(Params, TEXT("include_defaults"), true)));
}

FMonolithActionResult FMonolithConsoleActions::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FConsoleExecutionOptions Options = ReadExecutionOptions(Params, /*bDefaultDryRun=*/true);
	FString TargetError;
	if (!ValidateWorldTarget(Options.WorldTarget, TargetError))
	{
		return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FConsoleExecutionOutcome Outcome = ExecuteConsoleCommandInternal(Command, Options);
	if (!Outcome.bOk)
	{
		FMonolithActionResult Error = FMonolithActionResult::Error(Outcome.Error, Outcome.ErrorCode);
		if (Outcome.Error.Contains(TEXT("registered console object")))
		{
			Error.WithRelatedAction(TEXT("console.search_objects"));
		}
		return Error;
	}
	return FMonolithActionResult::Success(Outcome.Result);
}

FMonolithActionResult FMonolithConsoleActions::GetLogCursor(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
	{
		Root->SetBoolField(TEXT("log_capture_initialized"), true);
		Root->SetNumberField(TEXT("cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		Root->SetNumberField(TEXT("total_count"), LogCapture->GetTotalCount());
	}
	else
	{
		Root->SetBoolField(TEXT("log_capture_initialized"), false);
		Root->SetNumberField(TEXT("cursor"), 0);
		Root->SetNumberField(TEXT("total_count"), 0);
		Root->SetStringField(TEXT("warning"), TEXT("Monolith editor log capture is not initialized; log expectations cannot be evaluated."));
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::SearchLogsSince(const TSharedPtr<FJsonObject>& Params)
{
	int64 Cursor = 0;
	if (!ReadRequiredCursor(Params, Cursor))
	{
		return FMonolithActionResult::Error(TEXT("Missing required numeric field: cursor"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Pattern;
	FString Category;
	FString Verbosity = TEXT("very_verbose");
	if (!ReadStringField(Params, TEXT("pattern"), Pattern) ||
		!ReadStringField(Params, TEXT("category"), Category) ||
		!ReadStringField(Params, TEXT("verbosity"), Verbosity, TEXT("very_verbose")))
	{
		return FMonolithActionResult::Error(TEXT("pattern, category, and verbosity must be strings when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const int32 Limit = ReadClampedInt(Params, TEXT("limit"), DefaultLogLimit, 1, MaxLogLimit);
	const ELogVerbosity::Type MaxVerbosity = ConsoleStringToVerbosity(Verbosity);
	TArray<FMonolithLogEntry> Entries = CollectLogsAfter(Cursor, Pattern, Category, MaxVerbosity, Limit);

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	EntriesJson.Reserve(Entries.Num());
	for (const FMonolithLogEntry& Entry : Entries)
	{
		EntriesJson.Add(MakeShared<FJsonValueObject>(ConsoleLogEntryToJson(Entry)));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
	if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		Root->SetBoolField(TEXT("log_capture_initialized"), true);
	}
	else
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(Cursor));
		Root->SetBoolField(TEXT("log_capture_initialized"), false);
	}
	Root->SetStringField(TEXT("pattern"), Pattern);
	Root->SetStringField(TEXT("category"), Category);
	Root->SetStringField(TEXT("verbosity"), Verbosity);
	Root->SetNumberField(TEXT("match_count"), Entries.Num());
	Root->SetArrayField(TEXT("entries"), EntriesJson);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::WaitForLog(const TSharedPtr<FJsonObject>& Params)
{
	int64 Cursor = 0;
	if (!ReadRequiredCursor(Params, Cursor))
	{
		return FMonolithActionResult::Error(TEXT("Missing required numeric field: cursor"), FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<FConsoleLogExpectation> Expected;
	TArray<FConsoleLogExpectation> Rejected;
	FString ExpectError;
	if (!ReadExpectations(Params, TEXT("expect_log"), TEXT("expect_logs"), Expected, ExpectError) ||
		!ReadExpectations(Params, TEXT("reject_log"), TEXT("reject_logs"), Rejected, ExpectError))
	{
		return FMonolithActionResult::Error(ExpectError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Pattern;
	FString Category;
	FString Mode = TEXT("expect");
	FString Verbosity = TEXT("very_verbose");
	if (!ReadStringField(Params, TEXT("pattern"), Pattern) ||
		!ReadStringField(Params, TEXT("category"), Category) ||
		!ReadStringField(Params, TEXT("mode"), Mode, TEXT("expect")) ||
		!ReadStringField(Params, TEXT("verbosity"), Verbosity, TEXT("very_verbose")))
	{
		return FMonolithActionResult::Error(TEXT("pattern, category, mode, and verbosity must be strings when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}
	Mode = Mode.TrimStartAndEnd().ToLower();
	if (Mode != TEXT("expect") && Mode != TEXT("assert_absent"))
	{
		return FMonolithActionResult::Error(TEXT("mode must be expect or assert_absent."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!Pattern.IsEmpty())
	{
		FConsoleLogExpectation Expectation;
		Expectation.Pattern = Pattern;
		Expected.Add(MoveTemp(Expectation));
	}
	ApplyDefaultCategory(Expected, Category);
	ApplyDefaultCategory(Rejected, Category);

	if (Expected.Num() == 0 && Rejected.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("wait_for_log requires pattern, expect_log/expect_logs, or reject_log/reject_logs."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Expected.Num() == 0 && Rejected.Num() > 0 && Mode != TEXT("assert_absent"))
	{
		return FMonolithActionResult::Error(TEXT("Reject-only wait_for_log calls require mode=\"assert_absent\"."), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Mode == TEXT("assert_absent") && Expected.Num() > 0)
	{
		return FMonolithActionResult::Error(TEXT("mode=\"assert_absent\" cannot be combined with pattern, expect_log, or expect_logs."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const int32 TimeoutMs = ReadClampedInt(Params, TEXT("timeout_ms"), DefaultWaitForLogMs, 0, MaxWaitForLogMs);
	const int32 PollIntervalMs = ReadClampedInt(Params, TEXT("poll_interval_ms"), DefaultWaitPollMs, MinWaitPollMs, MaxWaitPollMs);
	const int32 LogLimit = ReadClampedInt(Params, TEXT("log_limit"), DefaultLogLimit, 1, MaxLogLimit);
	const ELogVerbosity::Type MaxVerbosity = ConsoleStringToVerbosity(Verbosity);

	bool bExpectationsPassed = false;
	bool bRejectedMatched = false;
	int32 WaitedMs = 0;
	TArray<FMonolithLogEntry> Entries;
	TSharedPtr<FJsonObject> ExpectationReport;
	FString Status = TEXT("timed_out");

	if (!FMonolithEditorActions::GetLogCapture())
	{
		Status = TEXT("log_capture_unavailable");
		ExpectationReport = BuildExpectationReport(Entries, Expected, Rejected, bExpectationsPassed);
	}
	else
	{
		const double StartSeconds = FPlatformTime::Seconds();
		while (true)
		{
			Entries = CollectLogsAfter(Cursor, TEXT(""), TEXT(""), MaxVerbosity, LogLimit);
			bRejectedMatched = HasRejectedMatch(Entries, Rejected);
			ExpectationReport = BuildExpectationReport(Entries, Expected, Rejected, bExpectationsPassed);
			if (bRejectedMatched)
			{
				Status = TEXT("rejected");
				break;
			}
			if (Expected.Num() > 0 && bExpectationsPassed)
			{
				Status = TEXT("matched");
				break;
			}

			const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			if (ElapsedMs >= static_cast<double>(TimeoutMs))
			{
				WaitedMs = static_cast<int32>(ElapsedMs);
				if (Mode == TEXT("assert_absent"))
				{
					Status = TEXT("absent");
				}
				break;
			}

			const int32 RemainingMs = FMath::Max(0, TimeoutMs - static_cast<int32>(ElapsedMs));
			const int32 StepMs = FMath::Min(PollIntervalMs, RemainingMs);
			if (StepMs <= 0)
			{
				break;
			}
			WaitForConsoleAsyncWork(StepMs);
			WaitedMs = static_cast<int32>((FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		}
	}

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	EntriesJson.Reserve(Entries.Num());
	for (const FMonolithLogEntry& Entry : Entries)
	{
		EntriesJson.Add(MakeShared<FJsonValueObject>(ConsoleLogEntryToJson(Entry)));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("passed"), Status == TEXT("matched") || Status == TEXT("absent"));
	Root->SetStringField(TEXT("status"), Status);
	Root->SetBoolField(TEXT("timed_out"), Status == TEXT("timed_out"));
	Root->SetBoolField(TEXT("rejected_matched"), bRejectedMatched);
	Root->SetBoolField(TEXT("observed_full_window"), Status == TEXT("absent"));
	Root->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
	Root->SetNumberField(TEXT("waited_ms"), WaitedMs);
	Root->SetNumberField(TEXT("timeout_ms"), TimeoutMs);
	Root->SetNumberField(TEXT("poll_interval_ms"), PollIntervalMs);
	Root->SetStringField(TEXT("pattern"), Pattern);
	Root->SetStringField(TEXT("mode"), Mode);
	Root->SetStringField(TEXT("verbosity"), Verbosity);
	if (!Category.IsEmpty())
	{
		Root->SetStringField(TEXT("category"), Category);
	}
	if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		Root->SetBoolField(TEXT("log_capture_initialized"), true);
	}
	else
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(Cursor));
		Root->SetBoolField(TEXT("log_capture_initialized"), false);
	}
	Root->SetObjectField(TEXT("expectations"), ExpectationReport);
	Root->SetNumberField(TEXT("log_count"), Entries.Num());
	Root->SetArrayField(TEXT("logs"), EntriesJson);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::ExecuteAndExpect(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FConsoleExecutionOptions Options = ReadExecutionOptions(Params, /*bDefaultDryRun=*/true);
	FString TargetError;
	if (!ValidateWorldTarget(Options.WorldTarget, TargetError))
	{
		return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<FConsoleLogExpectation> Expected;
	TArray<FConsoleLogExpectation> Rejected;
	FString ExpectError;
	if (!ReadExpectations(Params, TEXT("expect_log"), TEXT("expect_logs"), Expected, ExpectError) ||
		!ReadExpectations(Params, TEXT("reject_log"), TEXT("reject_logs"), Rejected, ExpectError))
	{
		return FMonolithActionResult::Error(ExpectError, FMonolithJsonUtils::ErrInvalidParams);
	}

	const int64 Cursor = FMonolithEditorActions::GetLogCapture()
		? FMonolithEditorActions::GetLogCapture()->GetLatestSequence()
		: 0;
	FConsoleExecutionOutcome Outcome = ExecuteConsoleCommandInternal(Command, Options);
	if (!Outcome.bOk)
	{
		return FMonolithActionResult::Error(Outcome.Error, Outcome.ErrorCode);
	}

	const int32 SettleMs = ReadClampedInt(Params, TEXT("settle_ms"), 100, 0, MaxSettleMs);
	if (SettleMs > 0)
	{
		WaitForConsoleAsyncWork(SettleMs);
	}

	const int32 LogLimit = ReadClampedInt(Params, TEXT("log_limit"), DefaultLogLimit, 1, MaxLogLimit);
	TArray<FMonolithLogEntry> Entries = CollectLogsAfter(Cursor, TEXT(""), TEXT(""), ELogVerbosity::VeryVerbose, LogLimit);
	bool bExpectationsPassed = true;
	TSharedPtr<FJsonObject> ExpectationReport = BuildExpectationReport(Entries, Expected, Rejected, bExpectationsPassed);

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	EntriesJson.Reserve(Entries.Num());
	for (const FMonolithLogEntry& Entry : Entries)
	{
		EntriesJson.Add(MakeShared<FJsonValueObject>(ConsoleLogEntryToJson(Entry)));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("passed"), bExpectationsPassed);
	Root->SetObjectField(TEXT("execution"), Outcome.Result);
	Root->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
	if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		Root->SetBoolField(TEXT("log_capture_initialized"), true);
	}
	else
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(Cursor));
		Root->SetBoolField(TEXT("log_capture_initialized"), false);
	}
	Root->SetObjectField(TEXT("expectations"), ExpectationReport);
	Root->SetNumberField(TEXT("log_count"), Entries.Num());
	Root->SetArrayField(TEXT("logs"), EntriesJson);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::RunSequence(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonObject>> Steps;
	FString StepsError;
	if (!ReadCommandArray(Params, Steps, StepsError))
	{
		return FMonolithActionResult::Error(StepsError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FConsoleExecutionOptions Defaults = ReadExecutionOptions(Params, /*bDefaultDryRun=*/true);
	FString TargetError;
	if (!ValidateWorldTarget(Defaults.WorldTarget, TargetError))
	{
		return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bAbortOnFailure = ReadBool(Params, TEXT("abort_on_failure"), true);
	const int32 DefaultSettleMs = ReadClampedInt(Params, TEXT("settle_ms"), 100, 0, MaxSettleMs);
	const int32 SequenceLogLimit = ReadClampedInt(Params, TEXT("log_limit"), DefaultLogLimit, 1, MaxLogLimit);
	FString ArtifactDir;
	const bool bArtifactDirProvided = Params.IsValid() && Params->HasField(TEXT("artifact_dir"));
	if (bArtifactDirProvided && !Params->TryGetStringField(TEXT("artifact_dir"), ArtifactDir))
	{
		return FMonolithActionResult::Error(TEXT("artifact_dir must be a string when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}
	ArtifactDir.TrimStartAndEndInline();
	if (bArtifactDirProvided && ArtifactDir.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("artifact_dir must be non-empty when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<TSharedPtr<FJsonValue>> StepResults;
	StepResults.Reserve(Steps.Num());

	bool bAllPassed = true;
	bool bAborted = false;
	int32 ExecutedCount = 0;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		TSharedPtr<FJsonObject> Step = Steps[Index];
		FString Command;
		if (!Step.IsValid() || !Step->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("commands[%d] is missing non-empty string field 'command'."), Index),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FConsoleExecutionOptions Options = Defaults;
		if (Step->HasField(TEXT("dry_run")))
		{
			Options.bDryRun = ReadBool(Step, TEXT("dry_run"), Options.bDryRun);
		}
		if (Step->HasField(TEXT("require_known_object")))
		{
			Options.bRequireKnownObject = ReadBool(Step, TEXT("require_known_object"), Options.bRequireKnownObject);
		}
		if (Step->HasField(TEXT("target_world")))
		{
			FString StepWorld;
			if (!Step->TryGetStringField(TEXT("target_world"), StepWorld))
			{
				return FMonolithActionResult::Error(TEXT("commands[].target_world must be a string."), FMonolithJsonUtils::ErrInvalidParams);
			}
			Options.WorldTarget = NormalizeWorldTarget(StepWorld);
			if (!ValidateWorldTarget(Options.WorldTarget, TargetError))
			{
				return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		TArray<FConsoleLogExpectation> Expected;
		TArray<FConsoleLogExpectation> Rejected;
		FString ExpectError;
		if (!ReadExpectations(Step, TEXT("expect_log"), TEXT("expect_logs"), Expected, ExpectError) ||
			!ReadExpectations(Step, TEXT("reject_log"), TEXT("reject_logs"), Rejected, ExpectError))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("commands[%d] expectation parse failed: %s"), Index, *ExpectError),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		const int64 Cursor = FMonolithEditorActions::GetLogCapture()
			? FMonolithEditorActions::GetLogCapture()->GetLatestSequence()
			: 0;
		FConsoleExecutionOutcome Outcome = ExecuteConsoleCommandInternal(Command, Options);
		++ExecutedCount;

		const int32 StepSettleMs = ReadClampedInt(Step, TEXT("settle_ms"), DefaultSettleMs, 0, MaxSettleMs);
		if (Outcome.bOk && StepSettleMs > 0)
		{
			WaitForConsoleAsyncWork(StepSettleMs);
		}

		const int32 StepLogLimit = ReadClampedInt(Step, TEXT("log_limit"), SequenceLogLimit, 1, MaxLogLimit);
		TArray<FMonolithLogEntry> Entries = CollectLogsAfter(Cursor, TEXT(""), TEXT(""), ELogVerbosity::VeryVerbose, StepLogLimit);
		bool bExpectationsPassed = true;
		TSharedPtr<FJsonObject> ExpectationReport = BuildExpectationReport(Entries, Expected, Rejected, bExpectationsPassed);

		const bool bCaptureRequested = ReadBool(Step, TEXT("capture"), false);
		bool bCapturePassed = true;
		TSharedPtr<FJsonObject> CaptureReport;
		if (bCaptureRequested)
		{
			bCapturePassed = false;
			CaptureReport = MakeShared<FJsonObject>();
			FString CaptureCommand = TEXT("HighResShot 1920x1080");
			if (!ReadStringField(Step, TEXT("capture_command"), CaptureCommand, CaptureCommand) || CaptureCommand.TrimStartAndEnd().IsEmpty())
			{
				CaptureReport->SetStringField(TEXT("status"), TEXT("invalid_capture_command"));
				CaptureReport->SetStringField(TEXT("error"), TEXT("capture_command must be a non-empty string when provided."));
			}
			else if (!Outcome.bOk)
			{
				CaptureReport->SetStringField(TEXT("status"), TEXT("skipped_execution_failed"));
			}
			else if (Options.bDryRun)
			{
				CaptureReport->SetStringField(TEXT("status"), TEXT("skipped_dry_run"));
			}
			else
			{
				const TMap<FString, FScreenshotFileSnapshot> BeforeFiles = SnapshotPngFiles();
				FConsoleExecutionOptions CaptureOptions = Options;
				CaptureOptions.bDryRun = false;
				CaptureOptions.bRequireKnownObject = false;
				FConsoleExecutionOutcome CaptureOutcome = ExecuteConsoleCommandInternal(CaptureCommand, CaptureOptions);
				CaptureReport->SetStringField(TEXT("capture_command"), CaptureCommand);
				if (CaptureOutcome.bOk)
				{
					CaptureReport->SetObjectField(TEXT("capture_execution"), CaptureOutcome.Result);
					const int32 CaptureWaitMs = ReadClampedInt(Step, TEXT("capture_wait_ms"), DefaultCaptureWaitMs, 0, MaxCaptureWaitMs);
					FString CapturedPath;
					int32 WaitedMs = 0;
					CapturedPath = FindNewestChangedPng(BeforeFiles);
					if (CapturedPath.IsEmpty() && IsInGameThread() && CaptureWaitMs > 0)
					{
						FString CaptureOutputPath;
						Step->TryGetStringField(TEXT("capture_output_path"), CaptureOutputPath);
						FString ResolvedOutputPath = ResolveOutputPath(CaptureOutputPath);
						FPaths::NormalizeFilename(ResolvedOutputPath);
						FPendingConsoleCapturePtr Pending = StartPendingConsoleCapture(CaptureCommand, BeforeFiles, CaptureOutputPath, ResolvedOutputPath, CaptureWaitMs);

						CaptureReport = PendingCaptureToJson(*Pending);
						CaptureReport->SetObjectField(TEXT("capture_execution"), CaptureOutcome.Result);
						CaptureReport->SetStringField(TEXT("capture_poll_action"), TEXT("console.poll_capture"));
						CaptureReport->SetStringField(TEXT("warning"), TEXT("Screenshot completion is deferred to a later game tick; poll console.poll_capture with capture_id."));
					}
					while (CapturedPath.IsEmpty() && !CaptureReport->HasField(TEXT("capture_id")) && WaitedMs <= CaptureWaitMs)
					{
						if (WaitedMs == CaptureWaitMs)
						{
							break;
						}
						const int32 WaitStepMs = FMath::Min(250, CaptureWaitMs - WaitedMs);
						WaitForConsoleAsyncWork(WaitStepMs);
						WaitedMs += WaitStepMs;
					}

					CaptureReport->SetNumberField(TEXT("capture_wait_ms"), CaptureWaitMs);
					CaptureReport->SetNumberField(TEXT("capture_waited_ms"), WaitedMs);
					if (CapturedPath.IsEmpty())
					{
						if (!CaptureReport->HasField(TEXT("capture_id")))
						{
							CaptureReport->SetStringField(TEXT("status"), TEXT("capture_not_found"));
							CaptureReport->SetStringField(TEXT("warning"), TEXT("No new or modified PNG appeared under Saved/Screenshots after the capture command."));
						}
					}
					else
					{
						CaptureReport->SetStringField(TEXT("status"), TEXT("captured"));
						CaptureReport->SetStringField(TEXT("capture_path"), CapturedPath);
						bCapturePassed = true;

						FString CaptureOutputPath;
						if (Step->TryGetStringField(TEXT("capture_output_path"), CaptureOutputPath) && !CaptureOutputPath.IsEmpty())
						{
							FString ResolvedOutputPath = ResolveOutputPath(CaptureOutputPath);
							FPaths::NormalizeFilename(ResolvedOutputPath);
							bool bCopyPassed = true;
							FString CopyStatus;
							int32 CopyResult = COPY_OK;
							ApplyCaptureOutputPath(CapturedPath, ResolvedOutputPath, bCopyPassed, CopyStatus, CopyResult);
							if (!bCopyPassed)
							{
								bCapturePassed = false;
								CaptureReport->SetStringField(TEXT("status"), CopyStatus);
								CaptureReport->SetNumberField(TEXT("copy_result"), CopyResult);
							}
							CaptureReport->SetStringField(TEXT("output_path"), ResolvedOutputPath);
						}
					}
				}
				else
				{
					CaptureReport->SetStringField(TEXT("status"), TEXT("capture_command_failed"));
					CaptureReport->SetStringField(TEXT("error"), CaptureOutcome.Error);
					CaptureReport->SetNumberField(TEXT("error_code"), CaptureOutcome.ErrorCode);
				}
			}
			CaptureReport->SetBoolField(TEXT("passed"), bCapturePassed);
		}

		const bool bStepPassed = Outcome.bOk && bExpectationsPassed && bCapturePassed;
		if (!bStepPassed)
		{
			bAllPassed = false;
		}

		TArray<TSharedPtr<FJsonValue>> LogsJson;
		LogsJson.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogsJson.Add(MakeShared<FJsonValueObject>(ConsoleLogEntryToJson(Entry)));
		}

		TSharedPtr<FJsonObject> StepResult = MakeShared<FJsonObject>();
		StepResult->SetNumberField(TEXT("index"), Index);
		StepResult->SetStringField(TEXT("command"), Command);
		StepResult->SetBoolField(TEXT("passed"), bStepPassed);
		StepResult->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
		if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
		{
			StepResult->SetNumberField(TEXT("next_cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		}
		else
		{
			StepResult->SetNumberField(TEXT("next_cursor"), static_cast<double>(Cursor));
		}
		if (Outcome.bOk)
		{
			StepResult->SetObjectField(TEXT("execution"), Outcome.Result);
		}
		else
		{
			StepResult->SetStringField(TEXT("error"), Outcome.Error);
			StepResult->SetNumberField(TEXT("error_code"), Outcome.ErrorCode);
		}
		StepResult->SetObjectField(TEXT("expectations"), ExpectationReport);
		if (CaptureReport.IsValid())
		{
			StepResult->SetObjectField(TEXT("capture"), CaptureReport);
		}
		StepResult->SetNumberField(TEXT("log_count"), Entries.Num());
		StepResult->SetArrayField(TEXT("logs"), LogsJson);
		StepResults.Add(MakeShared<FJsonValueObject>(StepResult));

		if (!bStepPassed && bAbortOnFailure)
		{
			bAborted = Index + 1 < Steps.Num();
			break;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("passed"), bAllPassed);
	Root->SetBoolField(TEXT("aborted"), bAborted);
	Root->SetBoolField(TEXT("abort_on_failure"), bAbortOnFailure);
	Root->SetNumberField(TEXT("requested_count"), Steps.Num());
	Root->SetNumberField(TEXT("executed_count"), ExecutedCount);
	Root->SetArrayField(TEXT("steps"), StepResults);
	if (FMonolithLogCapture* LogCapture = FMonolithEditorActions::GetLogCapture())
	{
		Root->SetNumberField(TEXT("next_cursor"), static_cast<double>(LogCapture->GetLatestSequence()));
		Root->SetBoolField(TEXT("log_capture_initialized"), true);
	}
	else
	{
		Root->SetNumberField(TEXT("next_cursor"), 0);
		Root->SetBoolField(TEXT("log_capture_initialized"), false);
	}
	if (!ArtifactDir.IsEmpty())
	{
		FString ResolvedArtifactDir = ResolveArtifactDirectory(ArtifactDir);
		FPaths::NormalizeFilename(ResolvedArtifactDir);
		FString ManifestPath = ResolvedArtifactDir / TEXT("manifest.json");
		FString LogsPath = ResolvedArtifactDir / TEXT("logs.jsonl");
		FPaths::NormalizeFilename(ManifestPath);
		FPaths::NormalizeFilename(LogsPath);
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("dir"), ResolvedArtifactDir);
		Artifact->SetStringField(TEXT("manifest_path"), ManifestPath);
		Artifact->SetStringField(TEXT("logs_jsonl_path"), LogsPath);
		Root->SetObjectField(TEXT("artifact"), Artifact);

		FString LogsError;
		const bool bLogsOk = WriteSequenceLogJsonl(LogsPath, StepResults, LogsError);
		if (!bLogsOk)
		{
			Artifact->SetStringField(TEXT("logs_error"), LogsError);
		}

		Artifact->SetBoolField(TEXT("written"), bLogsOk);
		Artifact->SetStringField(TEXT("status"), bLogsOk ? TEXT("written") : TEXT("write_failed"));
		if (!bLogsOk)
		{
			Root->SetBoolField(TEXT("passed"), false);
		}

		FString ManifestError;
		const bool bManifestOk = WriteJsonObjectFile(ManifestPath, Root, ManifestError);
		if (!bManifestOk)
		{
			Artifact->SetBoolField(TEXT("written"), false);
			Artifact->SetStringField(TEXT("status"), TEXT("write_failed"));
			Artifact->SetStringField(TEXT("manifest_error"), ManifestError);
			Root->SetBoolField(TEXT("passed"), false);
		}
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::ExecuteAndCapture(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString CaptureCommand = TEXT("HighResShot 1920x1080");
	if (!ReadStringField(Params, TEXT("capture_command"), CaptureCommand, CaptureCommand) || CaptureCommand.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("capture_command must be a non-empty string when provided."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FConsoleExecutionOptions Options = ReadExecutionOptions(Params, /*bDefaultDryRun=*/true);
	Options.bDryRun = false;
	FString TargetError;
	if (!ValidateWorldTarget(Options.WorldTarget, TargetError))
	{
		return FMonolithActionResult::Error(TargetError, FMonolithJsonUtils::ErrInvalidParams);
	}

	const TMap<FString, FScreenshotFileSnapshot> BeforeFiles = SnapshotPngFiles();
	FConsoleExecutionOutcome CommandOutcome = ExecuteConsoleCommandInternal(Command, Options);
	if (!CommandOutcome.bOk)
	{
		return FMonolithActionResult::Error(CommandOutcome.Error, CommandOutcome.ErrorCode);
	}

	const int32 SettleMs = ReadClampedInt(Params, TEXT("settle_ms"), 250, 0, MaxSettleMs);
	if (SettleMs > 0)
	{
		WaitForConsoleAsyncWork(SettleMs);
	}

	FConsoleExecutionOptions CaptureOptions = Options;
	CaptureOptions.bRequireKnownObject = false;
	FConsoleExecutionOutcome CaptureOutcome = ExecuteConsoleCommandInternal(CaptureCommand, CaptureOptions);
	if (!CaptureOutcome.bOk)
	{
		return FMonolithActionResult::Error(CaptureOutcome.Error, CaptureOutcome.ErrorCode);
	}

	const int32 CaptureWaitMs = ReadClampedInt(Params, TEXT("capture_wait_ms"), DefaultCaptureWaitMs, 0, MaxCaptureWaitMs);
	FString CapturedPath;
	int32 WaitedMs = 0;
	CapturedPath = FindNewestChangedPng(BeforeFiles);
	if (CapturedPath.IsEmpty() && IsInGameThread() && CaptureWaitMs > 0)
	{
		FString OutputPath;
		Params->TryGetStringField(TEXT("output_path"), OutputPath);
		FString ResolvedOutputPath = ResolveOutputPath(OutputPath);
		FPaths::NormalizeFilename(ResolvedOutputPath);

		FPendingConsoleCapturePtr Pending = StartPendingConsoleCapture(CaptureCommand, BeforeFiles, OutputPath, ResolvedOutputPath, CaptureWaitMs);
		TSharedPtr<FJsonObject> Root = PendingCaptureToJson(*Pending);
		Root->SetObjectField(TEXT("execution"), CommandOutcome.Result);
		Root->SetObjectField(TEXT("capture_execution"), CaptureOutcome.Result);
		Root->SetStringField(TEXT("capture_poll_action"), TEXT("console.poll_capture"));
		Root->SetStringField(TEXT("warning"), TEXT("Screenshot completion is deferred to a later game tick; poll console.poll_capture with capture_id."));
		return FMonolithActionResult::Success(Root);
	}

	while (CapturedPath.IsEmpty() && WaitedMs <= CaptureWaitMs)
	{
		if (WaitedMs == CaptureWaitMs)
		{
			break;
		}
		const int32 StepMs = FMath::Min(250, CaptureWaitMs - WaitedMs);
		WaitForConsoleAsyncWork(StepMs);
		WaitedMs += StepMs;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("passed"), !CapturedPath.IsEmpty());
	Root->SetObjectField(TEXT("execution"), CommandOutcome.Result);
	Root->SetObjectField(TEXT("capture_execution"), CaptureOutcome.Result);
	Root->SetStringField(TEXT("capture_command"), CaptureCommand);
	Root->SetNumberField(TEXT("capture_wait_ms"), CaptureWaitMs);
	Root->SetNumberField(TEXT("capture_waited_ms"), WaitedMs);
	if (CapturedPath.IsEmpty())
	{
		Root->SetStringField(TEXT("status"), TEXT("capture_not_found"));
		Root->SetStringField(TEXT("warning"), TEXT("No new or modified PNG appeared under Saved/Screenshots after the capture command."));
		return FMonolithActionResult::Success(Root);
	}

	Root->SetStringField(TEXT("status"), TEXT("captured"));
	Root->SetStringField(TEXT("capture_path"), CapturedPath);

	FString OutputPath;
	if (Params->TryGetStringField(TEXT("output_path"), OutputPath) && !OutputPath.IsEmpty())
	{
		FString ResolvedOutputPath = ResolveOutputPath(OutputPath);
		FPaths::NormalizeFilename(ResolvedOutputPath);
		bool bCopyPassed = true;
		FString CopyStatus;
		int32 CopyResult = COPY_OK;
		ApplyCaptureOutputPath(CapturedPath, ResolvedOutputPath, bCopyPassed, CopyStatus, CopyResult);
		if (!bCopyPassed)
		{
			Root->SetBoolField(TEXT("passed"), false);
			Root->SetStringField(TEXT("status"), CopyStatus);
			Root->SetNumberField(TEXT("copy_result"), static_cast<int32>(CopyResult));
			return FMonolithActionResult::Success(Root);
		}
		Root->SetStringField(TEXT("output_path"), ResolvedOutputPath);
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::PollCapture(const TSharedPtr<FJsonObject>& Params)
{
	FString CaptureId;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("capture_id"), CaptureId) || CaptureId.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: capture_id"), FMonolithJsonUtils::ErrInvalidParams);
	}
	CaptureId.TrimStartAndEndInline();

	const bool bConsume = ReadBool(Params, TEXT("consume"), false);
	TSharedPtr<FJsonObject> Root;
	{
		FScopeLock Lock(&GPendingConsoleCaptureLock);
		TrimPendingConsoleCapturesLocked();
		FPendingConsoleCapturePtr* Found = GPendingConsoleCaptures.Find(CaptureId);
		if (!Found || !Found->IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown pending capture_id: %s"), *CaptureId),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		Root = PendingCaptureToJson(**Found);
		if (bConsume && (*Found)->bCompleted)
		{
			GPendingConsoleCaptures.Remove(CaptureId);
			Root->SetBoolField(TEXT("consumed"), true);
		}
		else
		{
			Root->SetBoolField(TEXT("consumed"), false);
		}
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::DiagnoseFailure(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("result"), ResultPtr) || !ResultPtr || !(*ResultPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required object field: result"), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonObject> Result = *ResultPtr;
	TArray<TSharedPtr<FJsonValue>> Causes;
	if (Result->HasField(TEXT("timed_out")) && Result->GetBoolField(TEXT("timed_out")))
	{
		AddDiagnosis(Causes, TEXT("log_wait_timeout"), TEXT("wait_for_log timed out before all expected patterns matched."), TEXT("increase timeout_ms or verify command emitted the expected log"));
	}
	if (Result->HasField(TEXT("rejected_matched")) && Result->GetBoolField(TEXT("rejected_matched")))
	{
		AddDiagnosis(Causes, TEXT("rejected_log_matched"), TEXT("A rejected log pattern appeared after the cursor."), TEXT("inspect result.logs"));
	}
	if (Result->HasField(TEXT("status")))
	{
		const FString Status = Result->GetStringField(TEXT("status"));
		if (Status == TEXT("capture_pending"))
		{
			AddDiagnosis(Causes, TEXT("capture_pending"), TEXT("Screenshot capture is still pending."), TEXT("console.poll_capture"));
		}
		else if (Status == TEXT("capture_not_found") || Status == TEXT("copy_failed"))
		{
			AddDiagnosis(Causes, TEXT("capture_failed"), FString::Printf(TEXT("Capture status: %s"), *Status), TEXT("console.execute_and_capture"));
		}
	}
	const TSharedPtr<FJsonObject>* ArtifactPtr = nullptr;
	const TSharedPtr<FJsonObject> Artifact = Result->TryGetObjectField(TEXT("artifact"), ArtifactPtr) && ArtifactPtr
		? *ArtifactPtr
		: nullptr;
	if (Artifact.IsValid() && Artifact->HasField(TEXT("status")) && Artifact->GetStringField(TEXT("status")) == TEXT("write_failed"))
	{
		FString Detail = TEXT("run_sequence artifact write failed.");
		if (Artifact->HasField(TEXT("manifest_error")))
		{
			Detail += TEXT(" manifest_error=");
			Detail += Artifact->GetStringField(TEXT("manifest_error"));
		}
		if (Artifact->HasField(TEXT("logs_error")))
		{
			Detail += TEXT(" logs_error=");
			Detail += Artifact->GetStringField(TEXT("logs_error"));
		}
		AddDiagnosis(Causes, TEXT("artifact_write_failed"), Detail, TEXT("check artifact_dir permissions or choose a writable Saved/Monolith path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (Result->TryGetArrayField(TEXT("steps"), Steps) && Steps)
	{
		for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
		{
			DiagnoseStepObject(StepValue.IsValid() ? StepValue->AsObject() : nullptr, Causes);
		}
	}
	else
	{
		DiagnoseStepObject(Result, Causes);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("diagnosed"), true);
	Root->SetBoolField(TEXT("failure_detected"), Causes.Num() > 0);
	Root->SetNumberField(TEXT("cause_count"), Causes.Num());
	Root->SetArrayField(TEXT("causes"), Causes);
	if (Causes.Num() == 0)
	{
		Root->SetStringField(TEXT("summary"), TEXT("No known console failure pattern was detected in the supplied result."));
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithConsoleActions::SetCvarScoped(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* CvarsPtr = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("cvars"), CvarsPtr) || !CvarsPtr || !(*CvarsPtr).IsValid() || (*CvarsPtr)->Values.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required non-empty object field: cvars"), FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<FScopedCvarRecord> Records;
	Records.Reserve((*CvarsPtr)->Values.Num());
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CvarsPtr)->Values)
	{
		bool bValueOk = false;
		FString RequestedValue = JsonValueToConsoleString(Pair.Value, bValueOk);
		if (!bValueOk)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("cvars.%s must be a string, number, or boolean."), *Pair.Key),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FScopedCvarRecord Record;
		Record.Name = Pair.Key;
		Record.RequestedValue = RequestedValue;
		Records.Add(MoveTemp(Record));
	}

	bool bValidationPassed = true;
	FString ValidationError;
	for (FScopedCvarRecord& Record : Records)
	{
		if (!CaptureConsoleVariableForScope(Record))
		{
			bValidationPassed = false;
			if (ValidationError.IsEmpty())
			{
				ValidationError = Record.Error;
			}
		}
	}
	if (!bValidationPassed)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("passed"), false);
		Root->SetBoolField(TEXT("sequence_success"), false);
		Root->SetBoolField(TEXT("sequence_passed"), false);
		Root->SetBoolField(TEXT("restored"), true);
		Root->SetStringField(TEXT("status"), TEXT("validation_failed"));
		Root->SetStringField(TEXT("error"), ValidationError);
		Root->SetArrayField(TEXT("cvars"), ScopedCvarRecordsToJson(Records));
		return FMonolithActionResult::Success(Root);
	}

	bool bAllSet = true;
	FString SetError;
	for (FScopedCvarRecord& Record : Records)
	{
		if (!SetConsoleVariableString(Record))
		{
			bAllSet = false;
			if (SetError.IsEmpty())
			{
				SetError = Record.Error;
			}
			break;
		}
	}

	FMonolithActionResult SequenceResult;
	if (bAllSet)
	{
		SequenceResult = FMonolithConsoleActions::RunSequence(Params);
	}

	bool bAllRestored = true;
	for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
	{
		if (Records[Index].bSet && !RestoreConsoleVariableString(Records[Index]))
		{
			bAllRestored = false;
		}
	}

	bool bSequencePassed = false;
	if (SequenceResult.bSuccess && SequenceResult.Result.IsValid() && SequenceResult.Result->HasField(TEXT("passed")))
	{
		bSequencePassed = SequenceResult.Result->GetBoolField(TEXT("passed"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("passed"), bAllSet && SequenceResult.bSuccess && bSequencePassed && bAllRestored);
	Root->SetBoolField(TEXT("sequence_success"), bAllSet && SequenceResult.bSuccess);
	Root->SetBoolField(TEXT("sequence_passed"), bSequencePassed);
	Root->SetBoolField(TEXT("restored"), bAllRestored);
	Root->SetArrayField(TEXT("cvars"), ScopedCvarRecordsToJson(Records));
	if (SequenceResult.Result.IsValid())
	{
		Root->SetObjectField(TEXT("sequence"), SequenceResult.Result);
	}
	if (!bAllSet)
	{
		Root->SetStringField(TEXT("status"), TEXT("set_failed"));
		Root->SetStringField(TEXT("error"), SetError);
	}
	else if (!SequenceResult.bSuccess)
	{
		Root->SetStringField(TEXT("status"), TEXT("sequence_error"));
		Root->SetStringField(TEXT("error"), SequenceResult.ErrorMessage);
		Root->SetNumberField(TEXT("error_code"), SequenceResult.ErrorCode);
	}
	else
	{
		Root->SetStringField(TEXT("status"), bAllRestored ? TEXT("restored") : TEXT("restore_failed"));
	}
	return FMonolithActionResult::Success(Root);
}
