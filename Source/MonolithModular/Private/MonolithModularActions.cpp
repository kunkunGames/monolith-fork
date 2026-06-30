#include "MonolithModularActions.h"

#include "MonolithParamSchema.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithModular
{
	static constexpr int32 ErrInvalidParams = -32602;

	struct FKnownReceiverSpec
	{
		const TCHAR* Name;
		const TCHAR* ClassPath;
		const TCHAR* ExpectedAddReceiverPhase;
		const TCHAR* ExpectedReadyEventPhase;
		const TCHAR* ExpectedRemoveReceiverPhase;
	};

	struct FTracePatternSpec
	{
		const TCHAR* Code;
		const TCHAR* Token;
		const TCHAR* Role;
		const TCHAR* Meaning;
	};

	static const FKnownReceiverSpec KnownReceiverSpecs[] =
	{
		{ TEXT("ModularPawn"), TEXT("/Script/ModularGameplayActors.ModularPawn"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") },
		{ TEXT("ModularCharacter"), TEXT("/Script/ModularGameplayActors.ModularCharacter"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") },
		{ TEXT("ModularPlayerController"), TEXT("/Script/ModularGameplayActors.ModularPlayerController"), TEXT("PreInitializeComponents"), TEXT("ReceivedPlayer"), TEXT("EndPlay") },
		{ TEXT("ModularPlayerState"), TEXT("/Script/ModularGameplayActors.ModularPlayerState"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") },
		{ TEXT("ModularGameStateBase"), TEXT("/Script/ModularGameplayActors.ModularGameStateBase"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") },
		{ TEXT("ModularGameState"), TEXT("/Script/ModularGameplayActors.ModularGameState"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") },
		{ TEXT("ModularGameModeBase"), TEXT("/Script/ModularGameplayActors.ModularGameModeBase"), TEXT("not_proven_no_receiver_calls_found"), TEXT("not_proven_no_ready_event_found"), TEXT("not_proven_no_remove_receiver_found") },
		{ TEXT("ModularGameMode"), TEXT("/Script/ModularGameplayActors.ModularGameMode"), TEXT("not_proven_no_receiver_calls_found"), TEXT("not_proven_no_ready_event_found"), TEXT("not_proven_no_remove_receiver_found") },
		{ TEXT("ModularAIController"), TEXT("/Script/ModularGameplayActors.ModularAIController"), TEXT("PreInitializeComponents"), TEXT("BeginPlay"), TEXT("EndPlay") }
	};

	static const FTracePatternSpec TracePatterns[] =
	{
		{ TEXT("add_receiver_static"), TEXT("AddGameFrameworkComponentReceiver"), TEXT("receiver_registration"), TEXT("Actor opts into GameFrameworkComponentManager receiver lifecycle through the static helper.") },
		{ TEXT("add_receiver_instance"), TEXT("AddReceiver("), TEXT("receiver_registration"), TEXT("Actor opts into GameFrameworkComponentManager receiver lifecycle through the subsystem instance.") },
		{ TEXT("remove_receiver_static"), TEXT("RemoveGameFrameworkComponentReceiver"), TEXT("receiver_removal"), TEXT("Actor opts out of GameFrameworkComponentManager receiver lifecycle through the static helper.") },
		{ TEXT("remove_receiver_instance"), TEXT("RemoveReceiver("), TEXT("receiver_removal"), TEXT("Actor opts out of GameFrameworkComponentManager receiver lifecycle through the subsystem instance.") },
		{ TEXT("send_extension_static"), TEXT("SendGameFrameworkComponentExtensionEvent"), TEXT("event_send"), TEXT("Code sends a GameFrameworkComponentManager extension event through the static helper.") },
		{ TEXT("send_extension_instance"), TEXT("SendExtensionEvent("), TEXT("event_send"), TEXT("Code sends a GameFrameworkComponentManager extension event through the subsystem instance.") },
		{ TEXT("add_extension_handler"), TEXT("AddExtensionHandler"), TEXT("handler_registration"), TEXT("Code registers an extension handler for receiver events.") },
		{ TEXT("add_component_request"), TEXT("AddComponentRequest"), TEXT("component_request"), TEXT("Code requests components for matching receiver actors.") },
		{ TEXT("event_receiver_added"), TEXT("NAME_ReceiverAdded"), TEXT("event_name"), TEXT("Canonical receiver-added event reference.") },
		{ TEXT("event_receiver_removed"), TEXT("NAME_ReceiverRemoved"), TEXT("event_name"), TEXT("Canonical receiver-removed event reference.") },
		{ TEXT("event_extension_added"), TEXT("NAME_ExtensionAdded"), TEXT("event_name"), TEXT("Canonical extension-added event reference.") },
		{ TEXT("event_extension_removed"), TEXT("NAME_ExtensionRemoved"), TEXT("event_name"), TEXT("Canonical extension-removed event reference.") },
		{ TEXT("event_game_actor_ready"), TEXT("NAME_GameActorReady"), TEXT("event_name"), TEXT("Canonical actor-ready event reference.") },
		{ TEXT("event_bind_inputs_now"), TEXT("NAME_BindInputsNow"), TEXT("project_event_name"), TEXT("Lyra input-binding extension event used by input binding and input context GameFeature actions.") },
		{ TEXT("event_lyra_ability_ready"), TEXT("NAME_LyraAbilityReady"), TEXT("project_event_name"), TEXT("Lyra player-state ability-ready extension event used by AddAbilities GameFeature actions.") }
	};

	static UClass* LoadClassPath(const FString& ClassPath, UClass* /*ExpectedBaseClass*/ = UObject::StaticClass())
	{
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		return LoadObject<UClass>(nullptr, *ClassPath);
	}

	static FString ClassPath(const UClass* Class)
	{
		return Class ? Class->GetPathName() : FString();
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

	static FMonolithActionExecutionPolicy ExplicitReadOnlyPolicy()
	{
		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.bDefaulted = false;
		return Policy;
	}

	static TSharedPtr<FJsonObject> ClassSummary(const FString& RequestedPath, UClass* Class, UClass* ExpectedBaseClass)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("requested_class_path"), RequestedPath);
		Obj->SetBoolField(TEXT("found"), Class != nullptr);
		Obj->SetStringField(TEXT("class_path"), ClassPath(Class));
		Obj->SetStringField(TEXT("expected_base_class_path"), ClassPath(ExpectedBaseClass));
		Obj->SetBoolField(TEXT("child_of_expected_base"), Class && ExpectedBaseClass && Class->IsChildOf(ExpectedBaseClass));
		Obj->SetBoolField(TEXT("abstract"), Class && Class->HasAnyClassFlags(CLASS_Abstract));
		Obj->SetBoolField(TEXT("deprecated"), Class && Class->HasAnyClassFlags(CLASS_Deprecated));
		Obj->SetBoolField(TEXT("native"), Class && Class->HasAnyClassFlags(CLASS_Native));
		return Obj;
	}

	static TSharedPtr<FJsonObject> KnownReceiverSummary(const FKnownReceiverSpec& Spec)
	{
		UClass* ReceiverClass = LoadClassPath(Spec.ClassPath, AActor::StaticClass());
		const bool bSourceProvenReceiver = !FString(Spec.ExpectedAddReceiverPhase).StartsWith(TEXT("not_proven"));
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Spec.Name);
		Obj->SetStringField(TEXT("class_path"), Spec.ClassPath);
		Obj->SetBoolField(TEXT("available"), ReceiverClass != nullptr);
		Obj->SetBoolField(TEXT("source_proven_receiver"), bSourceProvenReceiver);
		Obj->SetStringField(TEXT("resolved_class_path"), ClassPath(ReceiverClass));
		Obj->SetStringField(TEXT("expected_add_receiver_phase"), Spec.ExpectedAddReceiverPhase);
		Obj->SetStringField(TEXT("expected_ready_event_phase"), Spec.ExpectedReadyEventPhase);
		Obj->SetStringField(TEXT("expected_remove_receiver_phase"), Spec.ExpectedRemoveReceiverPhase);
		Obj->SetStringField(TEXT("receiver_lifecycle_status"), bSourceProvenReceiver ? TEXT("source_proven_receiver") : TEXT("not_proven_by_source_trace"));
		return Obj;
	}

	static bool TryMatchKnownReceiver(UClass* ActorClass, FString& OutMatchedClassPath, FString& OutMatchedName)
	{
		OutMatchedClassPath.Reset();
		OutMatchedName.Reset();
		if (!ActorClass)
		{
			return false;
		}

		for (const FKnownReceiverSpec& Spec : KnownReceiverSpecs)
		{
			UClass* ReceiverClass = LoadClassPath(Spec.ClassPath, AActor::StaticClass());
			const bool bSourceProvenReceiver = !FString(Spec.ExpectedAddReceiverPhase).StartsWith(TEXT("not_proven"));
			if (bSourceProvenReceiver && ReceiverClass && ActorClass->IsChildOf(ReceiverClass))
			{
				OutMatchedClassPath = ReceiverClass->GetPathName();
				OutMatchedName = Spec.Name;
				return true;
			}
		}

		return false;
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Severity, const TCHAR* Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("severity"), Severity);
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Obj));
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

	static TArray<TSharedPtr<FJsonValue>> CanonicalEventNames()
	{
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const TCHAR* EventName : {
			TEXT("ReceiverAdded"),
			TEXT("ReceiverRemoved"),
			TEXT("ExtensionAdded"),
			TEXT("ExtensionRemoved"),
			TEXT("GameActorReady")
		})
		{
			Events.Add(MakeShared<FJsonValueString>(EventName));
		}
		return Events;
	}

	static TArray<TSharedPtr<FJsonValue>> KnownReceiverSummaries()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FKnownReceiverSpec& Spec : KnownReceiverSpecs)
		{
			Rows.Add(MakeShared<FJsonValueObject>(KnownReceiverSummary(Spec)));
		}
		return Rows;
	}

	static bool ReadRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required string parameter '%s'."), FieldName);
			return false;
		}
		return true;
	}

	static FString ReadOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue = FString())
	{
		if (!Params.IsValid())
		{
			return DefaultValue;
		}
		FString Value;
		return Params->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
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
		if (!Params.IsValid())
		{
			return DefaultValue;
		}
		int32 Value = DefaultValue;
		return Params->TryGetNumberField(FieldName, Value) ? FMath::Clamp(Value, MinValue, MaxValue) : DefaultValue;
	}

	static bool ReadOptionalStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues, FString& OutError)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings."), FieldName);
				return false;
			}
			StringValue.TrimStartAndEndInline();
			if (!StringValue.IsEmpty())
			{
				OutValues.AddUnique(StringValue);
			}
		}
		return true;
	}

	static void AddOptionalSourceRoot(const FString& Root, TArray<FString>& Roots)
	{
		FString NormalizedRoot = Root;
		NormalizedRoot.TrimStartAndEndInline();
		if (NormalizedRoot.IsEmpty())
		{
			return;
		}
		if (FPaths::IsRelative(NormalizedRoot))
		{
			NormalizedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), NormalizedRoot);
		}
		NormalizedRoot = FPaths::ConvertRelativePathToFull(NormalizedRoot);
		FPaths::NormalizeDirectoryName(NormalizedRoot);
		if (FPaths::DirectoryExists(NormalizedRoot))
		{
			Roots.AddUnique(NormalizedRoot);
		}
	}

	static bool IsSkippedSourcePath(const FString& Path, bool bIncludeMonolithSource)
	{
		FString Normalized = Path;
		FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/Intermediate/")) ||
			Normalized.Contains(TEXT("/Binaries/")) ||
			Normalized.Contains(TEXT("/Saved/")) ||
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

	static void AddEngineModularSourceRoots(TArray<FString>& Roots)
	{
		for (const TCHAR* PluginName : { TEXT("ModularGameplay"), TEXT("ModularGameplayActors") })
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			if (Plugin.IsValid())
			{
				AddOptionalSourceRoot(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source")), Roots);
			}
		}
	}

	static bool HasSourceExtension(const FString& File)
	{
		const FString Extension = FPaths::GetExtension(File, false).ToLower();
		return Extension == TEXT("cpp") || Extension == TEXT("h") || Extension == TEXT("hpp") || Extension == TEXT("inl");
	}

	static void CollectSourceFiles(const TArray<FString>& Roots, bool bIncludeMonolithSource, int32 MaxFiles, TArray<FString>& OutFiles, TArray<TSharedPtr<FJsonValue>>& RootRows)
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

	static TArray<TSharedPtr<FJsonValue>> ExtractNameConstants(const FString& Line)
	{
		TSet<FString> Names;
		int32 SearchIndex = 0;
		while (true)
		{
			const int32 FoundIndex = Line.Find(TEXT("NAME_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
			if (FoundIndex == INDEX_NONE)
			{
				break;
			}
			int32 EndIndex = FoundIndex;
			while (EndIndex < Line.Len() && (FChar::IsAlnum(Line[EndIndex]) || Line[EndIndex] == TEXT('_')))
			{
				++EndIndex;
			}
			Names.Add(Line.Mid(FoundIndex, EndIndex - FoundIndex));
			SearchIndex = EndIndex;
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FString& Name : Names)
		{
			Rows.Add(MakeShared<FJsonValueString>(Name));
		}
		return Rows;
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

	static TArray<TSharedPtr<FJsonValue>> LimitationRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TCHAR* Limitation : {
			TEXT("Static source trace only; no PIE session, actor spawning, GameFeature activation, or live manager mutation is performed."),
			TEXT("Lexical matches identify candidate call sites and event symbols, but do not prove branch coverage or runtime reachability."),
			TEXT("Comment-only lines and generated/intermediate/binary paths are skipped by default; block comments embedded after code are not parsed as an AST."),
			TEXT("Private UGameFrameworkComponentManager maps are not inspected as runtime truth.")
		})
		{
			Rows.Add(MakeShared<FJsonValueString>(Limitation));
		}
		return Rows;
	}

	static TArray<TSharedPtr<FJsonValue>> TracePatternRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
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
}

void FMonolithModularActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("modular"), TEXT("get_status"),
		TEXT("Report ModularGameplay and ModularGameplayActors plugin/module/class availability without hard-linking optional runtime plugins."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"),
		MonolithModular::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("modular"), TEXT("describe_extension_receiver_lifecycle"),
		TEXT("Describe known ModularGameplayActors receiver lifecycle phases and optionally classify an actor class against that receiver contract."),
		FMonolithActionHandler::CreateStatic(&DescribeExtensionReceiverLifecycle),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_class"), TEXT("string"), TEXT("Actor class path to classify against known ModularGameplayActors receiver classes."))
			.Build(),
		TEXT("Diagnostics"),
		MonolithModular::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("modular"), TEXT("validate_add_component_targets"),
		TEXT("Validate GameFrameworkComponentManager AddComponentRequest actor/component target classes read-only; reports whether receiver lifecycle readiness is known or unproven."),
		FMonolithActionHandler::CreateStatic(&ValidateAddComponentTargets),
		FParamSchemaBuilder()
			.Required(TEXT("actor_class"), TEXT("string"), TEXT("Receiver actor class path."))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("Actor component class path to add."))
			.Optional(TEXT("require_modular_receiver"), TEXT("boolean"), TEXT("Treat classes that are not known ModularGameplayActors receivers as errors."), TEXT("true"))
			.Build(),
		TEXT("Validation"),
		MonolithModular::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("modular"), TEXT("trace_game_framework_extension_events"),
		TEXT("Trace GameFrameworkComponentManager receiver, component request, handler, and extension-event call sites in project source text read-only."),
		FMonolithActionHandler::CreateStatic(&TraceGameFrameworkExtensionEvents),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_class"), TEXT("string"), TEXT("Optional actor class path to classify alongside source trace results."))
			.Optional(TEXT("source_root"), TEXT("string"), TEXT("Optional local source directory to scan. Relative paths resolve under the project root."))
			.Optional(TEXT("source_roots"), TEXT("array"), TEXT("Additional local source directories to scan."))
			.Optional(TEXT("include_engine_modular_sources"), TEXT("boolean"), TEXT("Also scan ModularGameplay and ModularGameplayActors plugin source roots for the engine contract."), TEXT("false"))
			.Optional(TEXT("include_monolith_source"), TEXT("boolean"), TEXT("Include Plugins/Monolith source in the scan. Default skips Monolith to avoid self-noise."), TEXT("false"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum match rows to return, clamped 1..1000."), TEXT("200"))
			.Optional(TEXT("max_files"), TEXT("integer"), TEXT("Maximum source files to scan, clamped 1..10000."), TEXT("2000"))
			.Optional(TEXT("include_line_text"), TEXT("boolean"), TEXT("Include trimmed source line text in each match."), TEXT("true"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithModular::ExplicitReadOnlyPolicy());
}

FMonolithActionResult FMonolithModularActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithModular;

	TArray<TSharedPtr<FJsonValue>> Plugins;
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("ModularGameplay"))));
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("ModularGameplayActors"))));

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("ModularGameplay"))));
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("ModularGameplayActors"))));

	UClass* ComponentManagerClass = LoadClassPath(TEXT("/Script/ModularGameplay.GameFrameworkComponentManager"));
	TArray<TSharedPtr<FJsonValue>> Classes;
	Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(TEXT("/Script/ModularGameplay.GameFrameworkComponentManager"), ComponentManagerClass, UObject::StaticClass())));
	for (const FKnownReceiverSpec& Spec : KnownReceiverSpecs)
	{
		UClass* ActorClass = LoadClassPath(Spec.ClassPath, AActor::StaticClass());
		Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(Spec.ClassPath, ActorClass, AActor::StaticClass())));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("modular"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(TEXT("modular_gameplay_available"), ComponentManagerClass != nullptr);
	Result->SetBoolField(TEXT("modular_gameplay_actors_available"), LoadClassPath(TEXT("/Script/ModularGameplayActors.ModularPawn"), AActor::StaticClass()) != nullptr);
	Result->SetArrayField(TEXT("plugins"), Plugins);
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("classes"), Classes);
	Result->SetArrayField(TEXT("canonical_extension_events"), CanonicalEventNames());
	Result->SetArrayField(TEXT("known_receiver_lifecycle"), KnownReceiverSummaries());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModularActions::DescribeExtensionReceiverLifecycle(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithModular;

	const FString ActorClassPath = ReadOptionalStringParam(Params, TEXT("actor_class"));
	UClass* ActorClass = ActorClassPath.IsEmpty() ? nullptr : LoadClassPath(ActorClassPath, AActor::StaticClass());
	FString MatchedClassPath;
	FString MatchedName;
	const bool bKnownReceiver = TryMatchKnownReceiver(ActorClass, MatchedClassPath, MatchedName);

	TSharedPtr<FJsonObject> Classification = MakeShared<FJsonObject>();
	Classification->SetBoolField(TEXT("actor_class_supplied"), !ActorClassPath.IsEmpty());
	Classification->SetObjectField(TEXT("actor_class"), ClassSummary(ActorClassPath, ActorClass, AActor::StaticClass()));
	Classification->SetBoolField(TEXT("known_modular_receiver"), bKnownReceiver);
	Classification->SetStringField(TEXT("matched_receiver_name"), MatchedName);
	Classification->SetStringField(TEXT("matched_receiver_class_path"), MatchedClassPath);
	Classification->SetStringField(
		TEXT("receiver_lifecycle_status"),
		ActorClassPath.IsEmpty()
			? TEXT("not_requested")
			: (bKnownReceiver ? TEXT("known_modular_receiver") : TEXT("not_proven_by_reflection")));
	if (!ActorClassPath.IsEmpty() && !bKnownReceiver)
	{
		Classification->SetStringField(
			TEXT("caveat"),
			TEXT("A non-ModularGameplayActors class can still be a valid receiver if its native code calls AddGameFrameworkComponentReceiver/RemoveGameFrameworkComponentReceiver and sends ready events; this read-only action does not prove arbitrary function bodies."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("modular"));
	Result->SetArrayField(TEXT("canonical_extension_events"), CanonicalEventNames());
	Result->SetArrayField(TEXT("known_receiver_lifecycle"), KnownReceiverSummaries());
	Result->SetObjectField(TEXT("classification"), Classification);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModularActions::ValidateAddComponentTargets(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithModular;

	FString ActorClassPath;
	FString ComponentClassPath;
	FString Error;
	if (!ReadRequiredStringParam(Params, TEXT("actor_class"), ActorClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}
	if (!ReadRequiredStringParam(Params, TEXT("component_class"), ComponentClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	const bool bRequireModularReceiver = ReadOptionalBoolParam(Params, TEXT("require_modular_receiver"), true);
	UClass* ActorClass = LoadClassPath(ActorClassPath, AActor::StaticClass());
	UClass* ComponentClass = LoadClassPath(ComponentClassPath, UActorComponent::StaticClass());

	FString MatchedClassPath;
	FString MatchedName;
	const bool bKnownReceiver = TryMatchKnownReceiver(ActorClass, MatchedClassPath, MatchedName);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;

	AddCheck(Checks, bOk, TEXT("actor_class_loaded"), ActorClass != nullptr, TEXT("error"), ActorClassPath);
	AddCheck(Checks, bOk, TEXT("actor_class_is_actor"), ActorClass && ActorClass->IsChildOf(AActor::StaticClass()), TEXT("error"), ClassPath(ActorClass));
	AddCheck(Checks, bOk, TEXT("actor_class_concrete"), ActorClass && !ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated), TEXT("error"), ClassPath(ActorClass));
	AddCheck(Checks, bOk, TEXT("component_class_loaded"), ComponentClass != nullptr, TEXT("error"), ComponentClassPath);
	AddCheck(Checks, bOk, TEXT("component_class_is_actor_component"), ComponentClass && ComponentClass->IsChildOf(UActorComponent::StaticClass()), TEXT("error"), ClassPath(ComponentClass));
	AddCheck(Checks, bOk, TEXT("component_class_concrete"), ComponentClass && !ComponentClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated), TEXT("error"), ClassPath(ComponentClass));
	AddCheck(
		Checks,
		bOk,
		TEXT("receiver_lifecycle_known"),
		bKnownReceiver || !bRequireModularReceiver,
		bRequireModularReceiver ? TEXT("error") : TEXT("warning"),
		bKnownReceiver ? MatchedClassPath : TEXT("receiver lifecycle is not proven by reflection"));

	if (!ActorClass)
	{
		AddIssue(Issues, TEXT("error"), TEXT("actor_class_not_found"), FString::Printf(TEXT("Actor class '%s' could not be loaded."), *ActorClassPath));
	}
	else if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		AddIssue(Issues, TEXT("error"), TEXT("actor_class_wrong_parent"), FString::Printf(TEXT("Class '%s' is not an AActor subclass."), *ActorClass->GetPathName()));
	}
	else if (ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		AddIssue(Issues, TEXT("error"), TEXT("actor_class_not_instantiable"), FString::Printf(TEXT("Actor class '%s' is abstract or deprecated."), *ActorClass->GetPathName()));
	}

	if (!ComponentClass)
	{
		AddIssue(Issues, TEXT("error"), TEXT("component_class_not_found"), FString::Printf(TEXT("Component class '%s' could not be loaded."), *ComponentClassPath));
	}
	else if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		AddIssue(Issues, TEXT("error"), TEXT("component_class_wrong_parent"), FString::Printf(TEXT("Class '%s' is not a UActorComponent subclass."), *ComponentClass->GetPathName()));
	}
	else if (ComponentClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		AddIssue(Issues, TEXT("error"), TEXT("component_class_not_instantiable"), FString::Printf(TEXT("Component class '%s' is abstract or deprecated."), *ComponentClass->GetPathName()));
	}

	if (!bKnownReceiver)
	{
		AddIssue(
			Issues,
			bRequireModularReceiver ? TEXT("error") : TEXT("warning"),
			TEXT("receiver_lifecycle_not_proven"),
			FString::Printf(
				TEXT("Actor class '%s' is not a known ModularGameplayActors receiver class. AddComponentRequest can only affect live actors that call AddGameFrameworkComponentReceiver and remove it correctly."),
				ActorClass ? *ActorClass->GetPathName() : *ActorClassPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("modular"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("require_modular_receiver"), bRequireModularReceiver);
	Result->SetObjectField(TEXT("actor_class"), ClassSummary(ActorClassPath, ActorClass, AActor::StaticClass()));
	Result->SetObjectField(TEXT("component_class"), ClassSummary(ComponentClassPath, ComponentClass, UActorComponent::StaticClass()));
	Result->SetBoolField(TEXT("known_modular_receiver"), bKnownReceiver);
	Result->SetStringField(TEXT("matched_receiver_name"), MatchedName);
	Result->SetStringField(TEXT("matched_receiver_class_path"), MatchedClassPath);
	Result->SetStringField(TEXT("receiver_lifecycle_status"), bKnownReceiver ? TEXT("known_modular_receiver") : TEXT("not_proven_by_reflection"));
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModularActions::TraceGameFrameworkExtensionEvents(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithModular;

	const bool bIncludeEngineModularSources = ReadOptionalBoolParam(Params, TEXT("include_engine_modular_sources"), false);
	const bool bIncludeMonolithSource = ReadOptionalBoolParam(Params, TEXT("include_monolith_source"), false);
	const bool bIncludeLineText = ReadOptionalBoolParam(Params, TEXT("include_line_text"), true);
	const int32 MaxResults = ReadOptionalIntParam(Params, TEXT("max_results"), 200, 1, 1000);
	const int32 MaxFiles = ReadOptionalIntParam(Params, TEXT("max_files"), 2000, 1, 10000);

	TArray<FString> Roots;
	FString Error;
	if (!ReadOptionalStringArrayParam(Params, TEXT("source_roots"), Roots, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}
	const FString SourceRoot = ReadOptionalStringParam(Params, TEXT("source_root"));
	if (!SourceRoot.IsEmpty())
	{
		Roots.AddUnique(SourceRoot);
	}
	if (Roots.Num() == 0)
	{
		Roots = DefaultProjectSourceRoots(bIncludeMonolithSource);
	}
	else
	{
		TArray<FString> NormalizedRoots;
		for (const FString& Root : Roots)
		{
			AddOptionalSourceRoot(Root, NormalizedRoots);
		}
		Roots = MoveTemp(NormalizedRoots);
	}
	if (bIncludeEngineModularSources)
	{
		AddEngineModularSourceRoots(Roots);
	}

	TArray<TSharedPtr<FJsonValue>> RootRows;
	TArray<FString> SourceFiles;
	CollectSourceFiles(Roots, bIncludeMonolithSource, MaxFiles, SourceFiles, RootRows);

	TArray<TSharedPtr<FJsonValue>> Matches;
	TArray<TSharedPtr<FJsonValue>> CanonicalCalls;
	TArray<TSharedPtr<FJsonValue>> EventTokens;
	TArray<TSharedPtr<FJsonValue>> ReceiverLifecycle;
	TArray<TSharedPtr<FJsonValue>> ExtensionHandlers;
	TArray<TSharedPtr<FJsonValue>> ComponentRequests;
	TArray<TSharedPtr<FJsonValue>> EventSenders;
	TMap<FString, int32> CountsByCode;
	TMap<FString, int32> CountsByRole;
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

				TSharedPtr<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("file"), RelativeDisplayPath(File));
				Match->SetNumberField(TEXT("line"), LineIndex + 1);
				Match->SetStringField(TEXT("code"), Pattern.Code);
				Match->SetStringField(TEXT("token"), Pattern.Token);
				Match->SetStringField(TEXT("role"), Pattern.Role);
				Match->SetStringField(TEXT("meaning"), Pattern.Meaning);
				Match->SetStringField(TEXT("function_context"), InferFunctionContext(Lines, LineIndex));
				Match->SetArrayField(TEXT("name_constants"), ExtractNameConstants(Line));
				if (bIncludeLineText)
				{
					Match->SetStringField(TEXT("line_text"), TrimForJson(Line, 300));
				}
				const TSharedPtr<FJsonValue> MatchValue = MakeShared<FJsonValueObject>(Match);
				Matches.Add(MatchValue);
				const FString Role(Pattern.Role);
				if (Role == TEXT("event_name") || Role == TEXT("project_event_name"))
				{
					EventTokens.Add(MatchValue);
				}
				else if (Role == TEXT("handler_registration"))
				{
					ExtensionHandlers.Add(MatchValue);
					CanonicalCalls.Add(MatchValue);
				}
				else if (Role == TEXT("component_request"))
				{
					ComponentRequests.Add(MatchValue);
					CanonicalCalls.Add(MatchValue);
				}
				else if (Role == TEXT("event_send"))
				{
					EventSenders.Add(MatchValue);
					CanonicalCalls.Add(MatchValue);
				}
				else
				{
					CanonicalCalls.Add(MatchValue);
				}
				CountsByCode.FindOrAdd(Pattern.Code)++;
				CountsByRole.FindOrAdd(Pattern.Role)++;
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
		for (const FString& Key : Keys)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("key"), Key);
			Row->SetNumberField(TEXT("count"), Counts[Key]);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	};

	const FString ActorClassPath = ReadOptionalStringParam(Params, TEXT("actor_class"));
	UClass* ActorClass = ActorClassPath.IsEmpty() ? nullptr : LoadClassPath(ActorClassPath, AActor::StaticClass());
	FString MatchedClassPath;
	FString MatchedName;
	const bool bKnownReceiver = TryMatchKnownReceiver(ActorClass, MatchedClassPath, MatchedName);

	TSharedPtr<FJsonObject> Classification = MakeShared<FJsonObject>();
	Classification->SetBoolField(TEXT("actor_class_supplied"), !ActorClassPath.IsEmpty());
	Classification->SetObjectField(TEXT("actor_class"), ClassSummary(ActorClassPath, ActorClass, AActor::StaticClass()));
	Classification->SetBoolField(TEXT("known_modular_receiver"), bKnownReceiver);
	Classification->SetStringField(TEXT("matched_receiver_name"), MatchedName);
	Classification->SetStringField(TEXT("matched_receiver_class_path"), MatchedClassPath);
	Classification->SetStringField(
		TEXT("receiver_lifecycle_status"),
		ActorClassPath.IsEmpty() ? TEXT("not_requested") : (bKnownReceiver ? TEXT("known_modular_receiver") : TEXT("source_trace_required_or_not_proven")));

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_results"), MaxResults);
	Limits->SetNumberField(TEXT("max_files"), MaxFiles);
	Limits->SetBoolField(TEXT("truncated"), bTruncated);

	for (const FKnownReceiverSpec& Spec : KnownReceiverSpecs)
	{
		ReceiverLifecycle.Add(MakeShared<FJsonValueObject>(KnownReceiverSummary(Spec)));
	}

	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bOk = true;
	AddCheck(Checks, bOk, TEXT("source_roots_present"), RootRows.Num() > 0, TEXT("error"), FString::Printf(TEXT("roots_checked=%d"), RootRows.Num()));
	AddCheck(Checks, bOk, TEXT("source_files_scanned"), SourceFiles.Num() > 0, TEXT("error"), FString::Printf(TEXT("files_scanned=%d"), SourceFiles.Num()));
	AddCheck(Checks, bOk, TEXT("trace_matches_collected"), Matches.Num() > 0, TEXT("warning"), FString::Printf(TEXT("match_count=%d"), Matches.Num()));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("modular"));
	Result->SetStringField(TEXT("action"), TEXT("trace_game_framework_extension_events"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("analysis_mode"), TEXT("static_source_trace"));
	Result->SetStringField(TEXT("runtime_execution"), TEXT("not_performed"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(TEXT("include_engine_modular_sources"), bIncludeEngineModularSources);
	Result->SetBoolField(TEXT("include_monolith_source"), bIncludeMonolithSource);
	Result->SetNumberField(TEXT("roots_checked"), RootRows.Num());
	Result->SetNumberField(TEXT("files_scanned"), SourceFiles.Num());
	Result->SetNumberField(TEXT("files_with_matches"), FilesWithMatches);
	Result->SetNumberField(TEXT("match_count"), Matches.Num());
	Result->SetObjectField(TEXT("limits"), Limits);
	Result->SetObjectField(TEXT("classification"), Classification);
	Result->SetArrayField(TEXT("source_roots"), RootRows);
	Result->SetArrayField(TEXT("patterns"), TracePatternRows());
	Result->SetArrayField(TEXT("counts_by_code"), MapToJsonRows(CountsByCode));
	Result->SetArrayField(TEXT("counts_by_role"), MapToJsonRows(CountsByRole));
	Result->SetArrayField(TEXT("canonical_calls"), CanonicalCalls);
	Result->SetArrayField(TEXT("event_tokens"), EventTokens);
	Result->SetArrayField(TEXT("receiver_lifecycle"), ReceiverLifecycle);
	Result->SetArrayField(TEXT("extension_handlers"), ExtensionHandlers);
	Result->SetArrayField(TEXT("component_requests"), ComponentRequests);
	Result->SetArrayField(TEXT("event_senders"), EventSenders);
	Result->SetArrayField(TEXT("matches"), Matches);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), TArray<TSharedPtr<FJsonValue>>());
	Result->SetArrayField(TEXT("limitations"), LimitationRows());
	Result->SetStringField(TEXT("trace_contract"), TEXT("Lexical source trace only: reports candidate call sites and event-name constants, but does not prove runtime reachability, GameFeature activation, local-player state, or branch coverage."));
	return FMonolithActionResult::Success(Result);
}
