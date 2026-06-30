#include "MonolithLoadingActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace MonolithLoading
{
	static constexpr int32 ErrInvalidParams = -32602;

	struct FKnownClassSpec
	{
		const TCHAR* Name;
		const TCHAR* ClassPath;
		const TCHAR* Role;
	};

	static const FKnownClassSpec KnownClassSpecs[] =
	{
		{ TEXT("LoadingScreenManager"), TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"), TEXT("runtime GameInstance subsystem") },
		{ TEXT("LoadingProcessInterface"), TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"), TEXT("native loading processor interface") },
		{ TEXT("LoadingProcessTask"), TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"), TEXT("BlueprintType loading blocker task") },
		{ TEXT("CommonLoadingScreenSettings"), TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"), TEXT("CVar-backed settings CDO") },
		{ TEXT("LyraLoadingScreenSubsystem"), TEXT("/Script/LyraGame.LyraLoadingScreenSubsystem"), TEXT("optional Lyra loading-widget handoff") },
		{ TEXT("LyraUserFacingExperienceDefinition"), TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"), TEXT("optional Lyra session/loading widget contract") }
	};

	static const TCHAR* KnownCVarNames[] =
	{
		TEXT("CommonLoadingScreen.HoldLoadingScreenAdditionalSecs"),
		TEXT("CommonLoadingScreen.LogLoadingScreenReasonEveryFrame"),
		TEXT("CommonLoadingScreen.AlwaysShow")
	};

	static FMonolithActionExecutionPolicy ExplicitReadOnlyPolicy()
	{
		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.bDefaulted = false;
		return Policy;
	}

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
		Obj->SetBoolField(TEXT("implements_expected_interface"), Class && ExpectedBaseClass && ExpectedBaseClass->HasAnyClassFlags(CLASS_Interface) && Class->ImplementsInterface(ExpectedBaseClass));
		Obj->SetBoolField(TEXT("abstract"), Class && Class->HasAnyClassFlags(CLASS_Abstract));
		Obj->SetBoolField(TEXT("deprecated"), Class && Class->HasAnyClassFlags(CLASS_Deprecated));
		Obj->SetBoolField(TEXT("native"), Class && Class->HasAnyClassFlags(CLASS_Native));
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

		FProperty* ReturnProperty = nullptr;
		if (Function)
		{
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ReturnProperty = *It;
					break;
				}
			}
		}
		Obj->SetBoolField(TEXT("has_return_value"), ReturnProperty != nullptr);
		Obj->SetStringField(TEXT("return_cpp_type"), ReturnProperty ? ReturnProperty->GetCPPType() : FString());
		return Obj;
	}

	static TSharedPtr<FJsonObject> PropertySummary(UClass* Class, const TCHAR* PropertyName, const UObject* SourceObject = nullptr)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), PropertyName);
		FProperty* Property = Class ? Class->FindPropertyByName(FName(PropertyName)) : nullptr;
		Obj->SetBoolField(TEXT("found"), Property != nullptr);
		Obj->SetStringField(TEXT("property_path"), Property ? Property->GetFullName() : FString());
		Obj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
		Obj->SetBoolField(TEXT("config"), Property && Property->HasAnyPropertyFlags(CPF_Config));
		Obj->SetBoolField(TEXT("transient"), Property && Property->HasAnyPropertyFlags(CPF_Transient));
		if (Property && Property->HasMetaData(TEXT("ConsoleVariable")))
		{
			Obj->SetStringField(TEXT("console_variable"), Property->GetMetaData(TEXT("ConsoleVariable")));
		}
		if (Property && SourceObject)
		{
			FString Value;
			Property->ExportText_InContainer(0, Value, SourceObject, SourceObject, const_cast<UObject*>(SourceObject), PPF_None);
			Obj->SetStringField(TEXT("value"), Value);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> KnownClassSummary(const FKnownClassSpec& Spec)
	{
		UClass* Class = LoadClassPath(Spec.ClassPath);
		TSharedPtr<FJsonObject> Obj = ClassSummary(Spec.ClassPath, Class, UObject::StaticClass());
		Obj->SetStringField(TEXT("name"), Spec.Name);
		Obj->SetStringField(TEXT("role"), Spec.Role);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> KnownClassSummaries()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FKnownClassSpec& Spec : KnownClassSpecs)
		{
			Rows.Add(MakeShared<FJsonValueObject>(KnownClassSummary(Spec)));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> CVarSummary(const TCHAR* CVarName)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), CVarName);
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
		Obj->SetBoolField(TEXT("found"), Var != nullptr);
		if (Var)
		{
			Obj->SetStringField(TEXT("value"), Var->GetString());
			Obj->SetNumberField(TEXT("int_value"), Var->GetInt());
			Obj->SetNumberField(TEXT("float_value"), Var->GetFloat());
			Obj->SetNumberField(TEXT("flags"), Var->GetFlags());
		}
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> CVarSummaries()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TCHAR* CVarName : KnownCVarNames)
		{
			Rows.Add(MakeShared<FJsonValueObject>(CVarSummary(CVarName)));
		}
		return Rows;
	}

	static TArray<TSharedPtr<FJsonValue>> SettingsPropertySummaries()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		UClass* SettingsClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"));
		UObject* CDO = SettingsClass ? SettingsClass->GetDefaultObject(false) : nullptr;
		for (const TCHAR* PropertyName : {
			TEXT("LoadingScreenWidget"),
			TEXT("LoadingScreenZOrder"),
			TEXT("HoldLoadingScreenAdditionalSecs"),
			TEXT("LoadingScreenHeartbeatHangDuration"),
			TEXT("LogLoadingScreenHeartbeatInterval"),
			TEXT("LogLoadingScreenReasonEveryFrame"),
			TEXT("ForceLoadingScreenVisible"),
			TEXT("HoldLoadingScreenAdditionalSecsEvenInEditor"),
			TEXT("ForceTickLoadingScreenEvenInEditor")
		})
		{
			Rows.Add(MakeShared<FJsonValueObject>(PropertySummary(SettingsClass, PropertyName, CDO)));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> ConfigSummary()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

		FString WidgetPath;
		const bool bHasWidget = GConfig && GConfig->GetString(
			TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"),
			TEXT("LoadingScreenWidget"),
			WidgetPath,
			GGameIni);
		Obj->SetBoolField(TEXT("default_game_loading_screen_widget_set"), bHasWidget && !WidgetPath.IsEmpty());
		Obj->SetStringField(TEXT("default_game_loading_screen_widget"), bHasWidget ? WidgetPath : FString());

		float HangMultiplier = 0.0f;
		const bool bHasHangMultiplier = GConfig && GConfig->GetFloat(
			TEXT("Core.System"),
			TEXT("LoadingScreenHangDurationMultiplier"),
			HangMultiplier,
			GEngineIni);
		Obj->SetBoolField(TEXT("engine_hang_duration_multiplier_set"), bHasHangMultiplier);
		if (bHasHangMultiplier)
		{
			Obj->SetNumberField(TEXT("engine_hang_duration_multiplier"), HangMultiplier);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> LyraHandoffSummary()
	{
		UClass* SubsystemClass = LoadClassPath(TEXT("/Script/LyraGame.LyraLoadingScreenSubsystem"));
		UClass* UserFacingClass = LoadClassPath(TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"));

		TArray<TSharedPtr<FJsonValue>> Functions;
		Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SubsystemClass, TEXT("SetLoadingScreenContentWidget"))));
		Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SubsystemClass, TEXT("GetLoadingScreenContentWidget"))));

		TArray<TSharedPtr<FJsonValue>> Properties;
		Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(UserFacingClass, TEXT("LoadingScreenWidget"))));

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("lyra_loading_screen_subsystem"), ClassSummary(TEXT("/Script/LyraGame.LyraLoadingScreenSubsystem"), SubsystemClass, UGameInstanceSubsystem::StaticClass()));
		Obj->SetObjectField(TEXT("lyra_user_facing_experience"), ClassSummary(TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"), UserFacingClass, UObject::StaticClass()));
		Obj->SetArrayField(TEXT("functions"), Functions);
		Obj->SetArrayField(TEXT("properties"), Properties);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> ContractRows()
	{
		const TCHAR* Rows[] =
		{
			TEXT("ULoadingScreenManager is the authoritative runtime subsystem for the current loading-screen reason exposed through GetDebugReasonForShowingOrHidingLoadingScreen()."),
			TEXT("ILoadingProcessInterface::ShouldShowLoadingScreen(FString&) is native-only and not a UFUNCTION; reflection can inventory processor candidates but cannot ask each processor for its private reason."),
			TEXT("UCommonLoadingScreenSettings is private-header/no API macro in the plugin; Monolith reads it by class path and CDO property reflection only."),
			TEXT("CommonLoadingScreen.AlwaysShow and HoldLoadingScreenAdditionalSecs CVars affect runtime visibility and hold timing; status output reports their current values."),
			TEXT("Lyra loading widget handoff is optional reflected evidence through ULyraLoadingScreenSubsystem and ULyraUserFacingExperienceDefinition.LoadingScreenWidget.")
		};

		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Row : Rows)
		{
			Values.Add(MakeShared<FJsonValueString>(Row));
		}
		return Values;
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

	static bool ReadOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
	{
		if (!Params.IsValid())
		{
			return DefaultValue;
		}
		bool Value = DefaultValue;
		return Params->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
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

	static int32 ReadOptionalIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		int32 Value = DefaultValue;
		if (Params.IsValid())
		{
			double NumberValue = 0.0;
			if (Params->TryGetNumberField(FieldName, NumberValue))
			{
				Value = FMath::RoundToInt(NumberValue);
			}
		}
		return FMath::Clamp(Value, MinValue, MaxValue);
	}

	static bool DoesClassMatchFilter(UClass* Class, const FString& ClassFilter)
	{
		return Class && (ClassFilter.IsEmpty()
			|| Class->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase)
			|| Class->GetPathName().Contains(ClassFilter, ESearchCase::IgnoreCase));
	}

	static TArray<TSharedPtr<FJsonValue>> ProcessorClassRows(bool bIncludeAllImplementers, const FString& ClassFilter, int32 MaxObjects)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		UClass* InterfaceClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"));
		UClass* TaskClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"));

		if (TaskClass && DoesClassMatchFilter(TaskClass, ClassFilter))
		{
			TSharedPtr<FJsonObject> Obj = ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"), TaskClass, TaskClass);
			Obj->SetBoolField(TEXT("known_common_loading_task"), true);
			Obj->SetStringField(TEXT("proof_level"), TEXT("known_class_reflection"));
			Rows.Add(MakeShared<FJsonValueObject>(Obj));
		}

		if (bIncludeAllImplementers && InterfaceClass)
		{
			for (TObjectIterator<UClass> It; It && Rows.Num() < MaxObjects; ++It)
			{
				UClass* Candidate = *It;
				if (!Candidate || Candidate == TaskClass || Candidate->HasAnyClassFlags(CLASS_Deprecated))
				{
					continue;
				}
				const bool bImplementsInterface = Candidate->ImplementsInterface(InterfaceClass);
				const bool bIsTask = TaskClass && Candidate->IsChildOf(TaskClass);
				if ((bImplementsInterface || bIsTask) && DoesClassMatchFilter(Candidate, ClassFilter))
				{
					TSharedPtr<FJsonObject> Obj = ClassSummary(Candidate->GetPathName(), Candidate, InterfaceClass);
					Obj->SetBoolField(TEXT("implements_loading_process_interface"), bImplementsInterface);
					Obj->SetBoolField(TEXT("child_of_loading_process_task"), bIsTask);
					Obj->SetStringField(TEXT("proof_level"), TEXT("loaded_class_inventory"));
					Rows.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
		}

		return Rows;
	}

	static UWorld* FindWorld(const FString& WorldContext)
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
			{
				continue;
			}
			if (WorldContext.Equals(TEXT("pie"), ESearchCase::IgnoreCase) && Context.WorldType == EWorldType::PIE)
			{
				return World;
			}
			if (WorldContext.Equals(TEXT("game"), ESearchCase::IgnoreCase) && Context.WorldType == EWorldType::Game)
			{
				return World;
			}
			if (WorldContext.Equals(TEXT("any"), ESearchCase::IgnoreCase) && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
			{
				return World;
			}
		}
		return nullptr;
	}

	static FString CallManagerReason(UObject* Manager)
	{
		if (!Manager)
		{
			return FString();
		}
		UFunction* Function = Manager->GetClass()->FindFunctionByName(FName(TEXT("GetDebugReasonForShowingOrHidingLoadingScreen")));
		if (!Function)
		{
			return FString();
		}
		struct FReasonParams
		{
			FString ReturnValue;
		};
		FReasonParams Params;
		Manager->ProcessEvent(Function, &Params);
		return Params.ReturnValue;
	}
}

void FMonolithLoadingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("loading"), TEXT("get_status"),
		TEXT("Report CommonLoadingScreen plugin/module/class/settings/CVar availability and optional Lyra handoff reflection without hard runtime dependencies."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder()
			.Optional(TEXT("include_settings"), TEXT("boolean"), TEXT("Include CommonLoadingScreenSettings reflected CDO properties."), TEXT("true"))
			.Optional(TEXT("include_cvars"), TEXT("boolean"), TEXT("Include known CommonLoadingScreen CVars."), TEXT("true"))
			.Optional(TEXT("include_lyra_handoff"), TEXT("boolean"), TEXT("Include optional Lyra loading-widget handoff classes."), TEXT("false"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithLoading::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("loading"), TEXT("describe_loading_processors"),
		TEXT("Describe CommonLoadingScreen loading-processor interface/task classes and optionally list loaded implementer classes read-only."),
		FMonolithActionHandler::CreateStatic(&DescribeLoadingProcessors),
		FParamSchemaBuilder()
			.Optional(TEXT("include_all_implementers"), TEXT("boolean"), TEXT("List loaded UClass implementers of LoadingProcessInterface."), TEXT("false"))
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Optional class name/path substring filter."))
			.Optional(TEXT("max_objects"), TEXT("number"), TEXT("Maximum implementer classes to report."), TEXT("100"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithLoading::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("loading"), TEXT("validate_loading_reason_contract"),
		TEXT("Validate the reflected CommonLoadingScreen reason/settings/CVar/Lyra handoff contract without querying native-only processor reasons."),
		FMonolithActionHandler::CreateStatic(&ValidateLoadingReasonContract),
		FParamSchemaBuilder()
			.Optional(TEXT("include_known_lyra"), TEXT("boolean"), TEXT("Include optional Lyra loading handoff checks."), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("Treat optional Lyra handoff absence as an error."), TEXT("false"))
			.Build(),
		TEXT("Validation"),
		MonolithLoading::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("loading"), TEXT("trace_loading_screen_blockers"),
		TEXT("Read the live ULoadingScreenManager debug reason when PIE/game is running, plus settings/CVar/processor-candidate context; returns pie_not_running cleanly when no runtime world exists."),
		FMonolithActionHandler::CreateStatic(&TraceLoadingScreenBlockers),
		FParamSchemaBuilder()
			.Optional(TEXT("world_context"), TEXT("string"), TEXT("pie, game, or any."), TEXT("pie"))
			.Optional(TEXT("include_settings"), TEXT("boolean"), TEXT("Include CommonLoadingScreenSettings reflected CDO properties."), TEXT("true"))
			.Optional(TEXT("include_cvars"), TEXT("boolean"), TEXT("Include known CommonLoadingScreen CVars."), TEXT("true"))
			.Optional(TEXT("include_processor_candidates"), TEXT("boolean"), TEXT("Include loaded processor candidate class inventory."), TEXT("true"))
			.Optional(TEXT("include_lyra_handoff"), TEXT("boolean"), TEXT("Include optional Lyra handoff reflection."), TEXT("true"))
			.Optional(TEXT("max_candidates"), TEXT("number"), TEXT("Maximum processor candidate classes to list."), TEXT("64"))
			.Build(),
		TEXT("Diagnostics"),
		MonolithLoading::ExplicitReadOnlyPolicy());
}

FMonolithActionResult FMonolithLoadingActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLoading;

	const bool bIncludeSettings = ReadOptionalBoolParam(Params, TEXT("include_settings"), true);
	const bool bIncludeCVars = ReadOptionalBoolParam(Params, TEXT("include_cvars"), true);
	const bool bIncludeLyraHandoff = ReadOptionalBoolParam(Params, TEXT("include_lyra_handoff"), false);

	TArray<TSharedPtr<FJsonValue>> Plugins;
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonLoadingScreen"))));

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonLoadingScreen"))));
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonStartupLoadingScreen"))));

	UClass* ManagerClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"));
	UClass* InterfaceClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"));
	UClass* SettingsClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("loading"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(TEXT("common_loading_screen_available"), ManagerClass != nullptr);
	Result->SetBoolField(TEXT("loading_process_interface_available"), InterfaceClass != nullptr);
	Result->SetBoolField(TEXT("settings_class_available"), SettingsClass != nullptr);
	Result->SetArrayField(TEXT("plugins"), Plugins);
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("classes"), KnownClassSummaries());
	Result->SetArrayField(TEXT("contract"), ContractRows());
	Result->SetObjectField(TEXT("config"), ConfigSummary());
	if (bIncludeSettings)
	{
		Result->SetArrayField(TEXT("settings_properties"), SettingsPropertySummaries());
	}
	if (bIncludeCVars)
	{
		Result->SetArrayField(TEXT("cvars"), CVarSummaries());
	}
	if (bIncludeLyraHandoff)
	{
		Result->SetObjectField(TEXT("lyra_handoff"), LyraHandoffSummary());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLoadingActions::DescribeLoadingProcessors(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLoading;

	const bool bIncludeAllImplementers = ReadOptionalBoolParam(Params, TEXT("include_all_implementers"), false);
	const FString ClassFilter = ReadOptionalStringParam(Params, TEXT("class_filter"));
	const int32 MaxObjects = ReadOptionalIntParam(Params, TEXT("max_objects"), 100, 1, 1000);

	UClass* InterfaceClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"));
	UClass* TaskClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"));

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(InterfaceClass, TEXT("ShouldShowLoadingScreen"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(TaskClass, TEXT("CreateLoadingScreenProcessTask"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(TaskClass, TEXT("Unregister"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(TaskClass, TEXT("SetShowLoadingScreenReason"))));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("loading"));
	Result->SetObjectField(TEXT("interface_class"), ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"), InterfaceClass, UObject::StaticClass()));
	Result->SetObjectField(TEXT("task_class"), ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"), TaskClass, UObject::StaticClass()));
	Result->SetArrayField(TEXT("functions"), Functions);
	Result->SetArrayField(TEXT("processor_classes"), ProcessorClassRows(bIncludeAllImplementers, ClassFilter, MaxObjects));
	Result->SetStringField(TEXT("reason_proof_level"), TEXT("processor inventory only; ILoadingProcessInterface::ShouldShowLoadingScreen(FString&) is native-only and not callable by reflection"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLoadingActions::ValidateLoadingReasonContract(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLoading;

	const bool bIncludeKnownLyra = ReadOptionalBoolParam(Params, TEXT("include_known_lyra"), true);
	const bool bStrict = ReadOptionalBoolParam(Params, TEXT("strict"), false);

	UClass* ManagerClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"));
	UClass* InterfaceClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"));
	UClass* SettingsClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"));
	UClass* LyraSubsystemClass = LoadClassPath(TEXT("/Script/LyraGame.LyraLoadingScreenSubsystem"));
	UClass* LyraUserFacingClass = LoadClassPath(TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"));

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;

	UFunction* ReasonFunction = ManagerClass ? ManagerClass->FindFunctionByName(FName(TEXT("GetDebugReasonForShowingOrHidingLoadingScreen"))) : nullptr;
	AddCheck(Checks, bOk, TEXT("manager_class_loaded"), ManagerClass != nullptr, TEXT("error"), TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"));
	AddCheck(Checks, bOk, TEXT("manager_reason_function_reflected"), ReasonFunction != nullptr, TEXT("error"), TEXT("GetDebugReasonForShowingOrHidingLoadingScreen"));
	AddCheck(Checks, bOk, TEXT("processor_interface_loaded"), InterfaceClass != nullptr, TEXT("error"), TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"));
	AddCheck(Checks, bOk, TEXT("settings_class_loaded"), SettingsClass != nullptr, TEXT("error"), TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"));

	for (const TCHAR* CVarName : KnownCVarNames)
	{
		const bool bCVarFound = IConsoleManager::Get().FindConsoleVariable(CVarName) != nullptr;
		AddCheck(Checks, bOk, *FString::Printf(TEXT("cvar_%s"), CVarName), bCVarFound, TEXT("error"), CVarName);
		if (!bCVarFound)
		{
			AddIssue(Issues, TEXT("error"), TEXT("cvar_missing"), FString::Printf(TEXT("Expected CommonLoadingScreen CVar '%s' was not registered."), CVarName));
		}
	}

	if (!ManagerClass)
	{
		AddIssue(Issues, TEXT("error"), TEXT("manager_class_not_found"), TEXT("ULoadingScreenManager could not be loaded by path."));
	}
	if (!ReasonFunction)
	{
		AddIssue(Issues, TEXT("error"), TEXT("manager_reason_function_missing"), TEXT("GetDebugReasonForShowingOrHidingLoadingScreen is missing or not reflected."));
	}

	if (bIncludeKnownLyra)
	{
		AddCheck(Checks, bOk, TEXT("lyra_loading_subsystem_loaded"), LyraSubsystemClass != nullptr || !bStrict, bStrict ? TEXT("error") : TEXT("warning"), TEXT("/Script/LyraGame.LyraLoadingScreenSubsystem"));
		AddCheck(Checks, bOk, TEXT("lyra_user_facing_experience_loaded"), LyraUserFacingClass != nullptr || !bStrict, bStrict ? TEXT("error") : TEXT("warning"), TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition"));
		if (bStrict && !LyraSubsystemClass)
		{
			AddIssue(Issues, TEXT("error"), TEXT("lyra_loading_subsystem_missing"), TEXT("LyraLoadingScreenSubsystem is required by strict mode but was not found."));
		}
		if (bStrict && !LyraUserFacingClass)
		{
			AddIssue(Issues, TEXT("error"), TEXT("lyra_user_facing_experience_missing"), TEXT("LyraUserFacingExperienceDefinition is required by strict mode but was not found."));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("loading"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("strict"), bStrict);
	Result->SetObjectField(TEXT("manager_class"), ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"), ManagerClass, UGameInstanceSubsystem::StaticClass()));
	Result->SetObjectField(TEXT("processor_interface"), ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"), InterfaceClass, UObject::StaticClass()));
	Result->SetObjectField(TEXT("settings_class"), ClassSummary(TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"), SettingsClass, UObject::StaticClass()));
	Result->SetArrayField(TEXT("contract"), ContractRows());
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	if (bIncludeKnownLyra)
	{
		Result->SetObjectField(TEXT("lyra_handoff"), LyraHandoffSummary());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLoadingActions::TraceLoadingScreenBlockers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLoading;

	const FString WorldContext = ReadOptionalStringParam(Params, TEXT("world_context"), TEXT("pie"));
	if (!WorldContext.Equals(TEXT("pie"), ESearchCase::IgnoreCase)
		&& !WorldContext.Equals(TEXT("game"), ESearchCase::IgnoreCase)
		&& !WorldContext.Equals(TEXT("any"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("Param 'world_context' must be pie, game, or any"), ErrInvalidParams);
	}

	const bool bIncludeSettings = ReadOptionalBoolParam(Params, TEXT("include_settings"), true);
	const bool bIncludeCVars = ReadOptionalBoolParam(Params, TEXT("include_cvars"), true);
	const bool bIncludeProcessorCandidates = ReadOptionalBoolParam(Params, TEXT("include_processor_candidates"), true);
	const bool bIncludeLyraHandoff = ReadOptionalBoolParam(Params, TEXT("include_lyra_handoff"), true);
	const int32 MaxCandidates = ReadOptionalIntParam(Params, TEXT("max_candidates"), 64, 1, 1000);

	UClass* ManagerClass = LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("loading"));
	Result->SetStringField(TEXT("world_context"), WorldContext);
	Result->SetObjectField(TEXT("manager_class"), ClassSummary(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"), ManagerClass, UGameInstanceSubsystem::StaticClass()));

	UWorld* World = FindWorld(WorldContext);
	if (!World)
	{
		Result->SetStringField(TEXT("status"), WorldContext.Equals(TEXT("pie"), ESearchCase::IgnoreCase) ? TEXT("pie_not_running") : TEXT("world_not_running"));
		Result->SetStringField(TEXT("proof_level"), TEXT("no runtime manager; static reflection context only"));
	}
	else if (!ManagerClass)
	{
		Result->SetStringField(TEXT("status"), TEXT("manager_class_unavailable"));
		Result->SetStringField(TEXT("proof_level"), TEXT("world found but CommonLoadingScreen manager class missing"));
	}
	else
	{
		UGameInstance* GameInstance = World->GetGameInstance();
		UObject* Manager = GameInstance
			? GameInstance->GetSubsystemBase(TSubclassOf<UGameInstanceSubsystem>(ManagerClass))
			: nullptr;
		Result->SetStringField(TEXT("status"), Manager ? TEXT("manager_found") : TEXT("manager_not_found"));
		Result->SetStringField(TEXT("world_name"), World->GetName());
		Result->SetStringField(TEXT("manager_object_path"), ObjectPath(Manager));
		Result->SetStringField(TEXT("manager_reason"), CallManagerReason(Manager));
		Result->SetStringField(TEXT("proof_level"), Manager ? TEXT("manager_debug_reason_authoritative") : TEXT("world found but subsystem instance unavailable"));
	}

	if (bIncludeSettings)
	{
		Result->SetArrayField(TEXT("settings_properties"), SettingsPropertySummaries());
		Result->SetObjectField(TEXT("config"), ConfigSummary());
	}
	if (bIncludeCVars)
	{
		Result->SetArrayField(TEXT("cvars"), CVarSummaries());
	}
	if (bIncludeProcessorCandidates)
	{
		Result->SetArrayField(TEXT("processor_candidates"), ProcessorClassRows(true, FString(), MaxCandidates));
		Result->SetStringField(TEXT("processor_reason_proof_level"), TEXT("candidate inventory only; per-processor reason is native-only"));
	}
	if (bIncludeLyraHandoff)
	{
		Result->SetObjectField(TEXT("lyra_handoff"), LyraHandoffSummary());
	}
	return FMonolithActionResult::Success(Result);
}
