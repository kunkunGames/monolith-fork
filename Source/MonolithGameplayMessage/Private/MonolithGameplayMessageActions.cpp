#include "MonolithGameplayMessageActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace MonolithGameplayMessage
{
	static constexpr int32 ErrInvalidParams = -32602;

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
