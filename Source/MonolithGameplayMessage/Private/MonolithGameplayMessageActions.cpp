#include "MonolithGameplayMessageActions.h"

#include "MonolithGameplayMessageCommon.h"
#include "MonolithParamSchema.h"

#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace MonolithGameplayMessage
{
	namespace
	{
		const TCHAR* SubsystemPath = TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem");
		const TCHAR* AsyncActionPath = TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage");
		const TCHAR* ListenerHandlePath = TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle");
		const TCHAR* MatchEnumPath = TEXT("/Script/GameplayMessageRuntime.EGameplayMessageMatch");

		FString ObjectPath(const UObject* Object)
		{
			return Object ? Object->GetPathName() : FString();
		}

		TSharedPtr<FJsonObject> PluginStatus(const TCHAR* PluginName)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), PluginName);

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			Row->SetBoolField(TEXT("found"), Plugin.IsValid());
			if (Plugin.IsValid())
			{
				Row->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
				Row->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
				Row->SetStringField(TEXT("version_name"), BoundText(Plugin->GetDescriptor().VersionName, 256));
			}
			return Row;
		}

		TSharedPtr<FJsonObject> ModuleStatus(const TCHAR* ModuleName)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), ModuleName);
			Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
			Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(ModuleName)));
			return Row;
		}

		TSharedPtr<FJsonObject> ExactLoadStatus(const FExactObjectLoad& Load)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("requested_path"), Load.RequestedPath);
			Row->SetBoolField(TEXT("found_exact"), Load.IsExact());
			Row->SetStringField(TEXT("resolved_path"), Load.ResolvedPath);
			if (!Load.ErrorCode.IsEmpty())
			{
				Row->SetStringField(TEXT("error_code"), Load.ErrorCode);
				Row->SetStringField(TEXT("error_detail"), BoundText(Load.ErrorDetail));
			}
			return Row;
		}

		TSharedPtr<FJsonObject> ClassSummary(
			const FExactObjectLoad& Load,
			UClass* ExpectedBaseClass)
		{
			UClass* Class = Cast<UClass>(Load.Object);
			TSharedPtr<FJsonObject> Row = ExactLoadStatus(Load);
			Row->SetStringField(TEXT("class_path"), ObjectPath(Class));
			Row->SetStringField(TEXT("expected_base_class_path"), ObjectPath(ExpectedBaseClass));
			Row->SetBoolField(
				TEXT("child_of_expected_base"),
				Class && ExpectedBaseClass && Class->IsChildOf(ExpectedBaseClass));
			Row->SetBoolField(TEXT("abstract"), Class && Class->HasAnyClassFlags(CLASS_Abstract));
			Row->SetBoolField(TEXT("deprecated"), Class && Class->HasAnyClassFlags(CLASS_Deprecated));
			return Row;
		}

		int32 CountObjectReferenceProperties(const UScriptStruct* Struct)
		{
			int32 Count = 0;
			if (!Struct)
			{
				return Count;
			}

			const EPropertyObjectReferenceType AnyObjectReference =
				EPropertyObjectReferenceType::Strong
				| EPropertyObjectReferenceType::Weak
				| EPropertyObjectReferenceType::Soft
				| EPropertyObjectReferenceType::Conservative;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				TArray<const FStructProperty*> EncounteredStructProperties;
				if ((*It)->ContainsObjectReference(
					EncounteredStructProperties,
					AnyObjectReference))
				{
					++Count;
				}
			}
			return Count;
		}

		TSharedPtr<FJsonObject> StructSummary(const FExactObjectLoad& Load)
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(Load.Object);
			TSharedPtr<FJsonObject> Row = ExactLoadStatus(Load);
			Row->SetBoolField(TEXT("is_script_struct"), Struct != nullptr);
			Row->SetStringField(TEXT("struct_path"), ObjectPath(Struct));
			Row->SetStringField(
				TEXT("object_class_path"),
				Load.Object && Load.Object->GetClass() ? Load.Object->GetClass()->GetPathName() : FString());
			Row->SetBoolField(TEXT("blueprint_type"), Struct && Struct->HasMetaData(TEXT("BlueprintType")));
			Row->SetBoolField(TEXT("deprecated"), Struct && Struct->HasMetaData(TEXT("Deprecated")));
			Row->SetNumberField(
				TEXT("structure_size"),
				Struct ? Struct->GetStructureSize() : 0);

			int32 PropertyCount = 0;
			if (Struct)
			{
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					++PropertyCount;
				}
			}
			Row->SetNumberField(TEXT("property_count"), PropertyCount);
			Row->SetNumberField(
				TEXT("object_property_count"),
				CountObjectReferenceProperties(Struct));
			Row->SetBoolField(TEXT("object_reference_scan_recursive"), true);
			return Row;
		}

		TSharedPtr<FJsonObject> FunctionSummary(UClass* Class, const TCHAR* FunctionName)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), FunctionName);
			UFunction* Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
			Row->SetBoolField(TEXT("found"), Function != nullptr);
			Row->SetStringField(TEXT("function_path"), ObjectPath(Function));
			Row->SetBoolField(
				TEXT("blueprint_callable"),
				Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
			Row->SetBoolField(TEXT("custom_thunk"), Function && Function->HasMetaData(TEXT("CustomThunk")));
			return Row;
		}

		void AddCheck(
			TArray<TSharedPtr<FJsonValue>>& Checks,
			bool& bOk,
			const TCHAR* Name,
			bool bCheckOk,
			const TCHAR* Severity,
			const FString& Detail)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Name);
			Row->SetBoolField(TEXT("ok"), bCheckOk);
			Row->SetStringField(TEXT("severity"), Severity);
			Row->SetStringField(TEXT("detail"), BoundText(Detail));
			Checks.Add(MakeShared<FJsonValueObject>(Row));

			if (!bCheckOk && FCString::Stricmp(Severity, TEXT("error")) == 0)
			{
				bOk = false;
			}
		}

		void AddIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const TCHAR* Severity,
			const TCHAR* Code,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("severity"), Severity);
			Row->SetStringField(TEXT("code"), Code);
			Row->SetStringField(TEXT("message"), BoundText(Message));
			Issues.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> ListenerContractRows()
		{
			const TCHAR* ContractRows[] =
			{
				TEXT("UGameplayMessageSubsystem is a UGameInstanceSubsystem; route access through a world or game instance."),
				TEXT("A broadcast payload UScriptStruct must equal or derive from the listener's expected UScriptStruct; the listener type is the accepted parent type."),
				TEXT("ExactMatch receives only the exact channel; PartialMatch receives the root channel and child channels."),
				TEXT("Listener handles should be unregistered when the receiver lifetime ends."),
				TEXT("Blueprint async listeners use UAsyncAction_ListenForGameplayMessage plus GetPayload custom thunk.")
			};

			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(UE_ARRAY_COUNT(ContractRows));
			for (const TCHAR* Contract : ContractRows)
			{
				Rows.Add(MakeShared<FJsonValueString>(Contract));
			}
			return Rows;
		}

		bool IsInvalidObjectPathInput(const FExactObjectLoad& Load)
		{
			return Load.ErrorCode == TEXT("empty_object_path")
				|| Load.ErrorCode == TEXT("object_path_whitespace")
				|| Load.ErrorCode == TEXT("object_path_noncanonical")
				|| Load.ErrorCode == TEXT("object_path_extension")
				|| Load.ErrorCode == TEXT("invalid_object_path");
		}

		TSharedPtr<FJsonObject> ValidateMessageStructInternal(
			const FExactObjectLoad& Load,
			bool bRequireBlueprintType,
			bool bRequireNoObjectReferences,
			TArray<TSharedPtr<FJsonValue>>& Checks,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			bool& bOk)
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(Load.Object);
			TSharedPtr<FJsonObject> Summary = StructSummary(Load);

			const int32 ObjectPropertyCount =
				CountObjectReferenceProperties(Struct);

			AddCheck(
				Checks,
				bOk,
				TEXT("message_struct_loaded_exact"),
				Load.IsExact(),
				TEXT("error"),
				Load.IsExact() ? Load.ResolvedPath : Load.ErrorDetail);
			AddCheck(
				Checks,
				bOk,
				TEXT("message_struct_is_uscriptstruct"),
				Struct != nullptr,
				TEXT("error"),
				ObjectPath(Load.Object));
			AddCheck(
				Checks,
				bOk,
				TEXT("message_struct_not_deprecated"),
				Struct && !Struct->HasMetaData(TEXT("Deprecated")),
				TEXT("error"),
				ObjectPath(Struct));
			AddCheck(
				Checks,
				bOk,
				TEXT("message_struct_blueprint_type"),
				!bRequireBlueprintType || (Struct && Struct->HasMetaData(TEXT("BlueprintType"))),
				bRequireBlueprintType ? TEXT("error") : TEXT("info"),
				Struct && Struct->HasMetaData(TEXT("BlueprintType"))
					? TEXT("BlueprintType")
					: TEXT("not marked BlueprintType"));
			AddCheck(
				Checks,
				bOk,
				TEXT("message_struct_object_reference_policy"),
				!bRequireNoObjectReferences || ObjectPropertyCount == 0,
				bRequireNoObjectReferences ? TEXT("error") : TEXT("info"),
				FString::Printf(
					TEXT("recursive_object_reference_property_count=%d"),
					ObjectPropertyCount));

			if (!Load.IsExact())
			{
				AddIssue(
					Issues,
					TEXT("error"),
					Load.ErrorCode.IsEmpty() ? TEXT("message_struct_not_loaded") : *Load.ErrorCode,
					Load.ErrorDetail);
			}
			else if (!Struct)
			{
				AddIssue(
					Issues,
					TEXT("error"),
					TEXT("message_object_not_script_struct"),
					FString::Printf(
						TEXT("Object '%s' is a %s, not a UScriptStruct."),
						*Load.ResolvedPath,
						Load.Object && Load.Object->GetClass()
							? *Load.Object->GetClass()->GetPathName()
							: TEXT("<null>")));
			}
			else
			{
				if (Struct->HasMetaData(TEXT("Deprecated")))
				{
					AddIssue(
						Issues,
						TEXT("error"),
						TEXT("message_struct_deprecated"),
						FString::Printf(TEXT("Struct '%s' is deprecated."), *Struct->GetPathName()));
				}
				if (bRequireBlueprintType && !Struct->HasMetaData(TEXT("BlueprintType")))
				{
					AddIssue(
						Issues,
						TEXT("error"),
						TEXT("message_struct_not_blueprint_type"),
						FString::Printf(TEXT("Struct '%s' is not marked BlueprintType."), *Struct->GetPathName()));
				}
				if (bRequireNoObjectReferences && ObjectPropertyCount > 0)
				{
					AddIssue(
						Issues,
						TEXT("error"),
						TEXT("message_struct_has_object_references"),
						FString::Printf(
							TEXT("Struct '%s' has %d properties that directly or recursively contain UObject references."),
							*Struct->GetPathName(),
							ObjectPropertyCount));
				}
			}

			return Summary;
		}

		FMonolithActionResult InvalidParams(const FString& Error)
		{
			return FMonolithActionResult::Error(Error, ErrInvalidParams);
		}

	}
}

void FMonolithGameplayMessageActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("gameplay_message"),
		TEXT("get_status"),
		TEXT("Report exact GameplayMessageRouter plugin, module, class, struct, and enum availability without a hard runtime dependency."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"));

	Registry.RegisterAction(
		TEXT("gameplay_message"),
		TEXT("describe_listener_contract"),
		TEXT("Describe the reflected GameplayMessageRouter listener, broadcast, payload, and match-type contract read-only."),
		FMonolithActionHandler::CreateStatic(&DescribeListenerContract),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"));

	Registry.RegisterAction(
		TEXT("gameplay_message"),
		TEXT("validate_message_struct"),
		TEXT("Validate one exact GameplayMessageRouter payload UScriptStruct path without redirects, substitution, or asset mutation."),
		FMonolithActionHandler::CreateStatic(&ValidateMessageStruct),
		FParamSchemaBuilder()
			.Required(TEXT("message_struct"), TEXT("string"), TEXT("Exact payload UScriptStruct object path."))
			.Optional(
				TEXT("require_blueprint_type"),
				TEXT("boolean"),
				TEXT("Require BlueprintType on the payload struct."),
				TEXT("false"))
			.Optional(
				TEXT("require_no_object_references"),
				TEXT("boolean"),
				TEXT("Treat UObject reference properties as an error."),
				TEXT("false"))
			.Build(),
		TEXT("Validation"));

	Registry.RegisterAction(
		TEXT("gameplay_message"),
		TEXT("validate_channel_contract"),
		TEXT("Validate one exact gameplay message channel tag, match type, and optional exact payload struct contract read-only."),
		FMonolithActionHandler::CreateStatic(&ValidateChannelContract),
		FParamSchemaBuilder()
			.Required(TEXT("channel_tag"), TEXT("string"), TEXT("Exact gameplay tag channel."))
			.Optional(
				TEXT("message_struct"),
				TEXT("string"),
				TEXT("Optional exact payload UScriptStruct object path."))
			.Optional(
				TEXT("match_type"),
				TEXT("string"),
				TEXT("ExactMatch or PartialMatch."),
				TEXT("ExactMatch"))
			.Optional(
				TEXT("require_registered_tag"),
				TEXT("boolean"),
				TEXT("Require the exact channel tag to be registered."),
				TEXT("true"))
			.Optional(
				TEXT("require_blueprint_type"),
				TEXT("boolean"),
				TEXT("Require BlueprintType on message_struct when supplied."),
				TEXT("false"))
			.Build(),
		TEXT("Validation"));

	Registry.RegisterAction(
		TEXT("gameplay_message"),
		TEXT("trace_channel_usage"),
		TEXT("Perform bounded static source analysis of GameplayMessageRouter broadcaster/listener candidates inside approved source roots."),
		FMonolithActionHandler::CreateStatic(&TraceChannelUsage),
		FParamSchemaBuilder()
			.Optional(
				TEXT("channel_tag"),
				TEXT("string"),
				TEXT("Optional exact channel tag or tag-constant filter."))
			.Optional(
				TEXT("source_root"),
				TEXT("string"),
				TEXT("Optional project-contained source directory using forward slashes."))
			.Optional(
				TEXT("source_roots"),
				TEXT("array"),
				TEXT("Additional project-contained source directories using forward slashes."))
			.Optional(
				TEXT("include_monolith_source"),
				TEXT("boolean"),
				TEXT("Include Plugins/Monolith source in the project scan."),
				TEXT("false"))
			.Optional(
				TEXT("include_engine_gameplay_message_sources"),
				TEXT("boolean"),
				TEXT("Also scan the installed GameplayMessageRouter plugin source."),
				TEXT("false"))
			.Optional(
				TEXT("max_files"),
				TEXT("integer"),
				TEXT("Maximum source files to scan, strict range 1..5000."),
				TEXT("2000"))
			.Optional(
				TEXT("max_results"),
				TEXT("integer"),
				TEXT("Maximum source matches to return, strict range 1..1000."),
				TEXT("500"))
			.Optional(
				TEXT("include_line_text"),
				TEXT("boolean"),
				TEXT("Include a bounded source-line excerpt in each match."),
				TEXT("false"))
			.Build(),
		TEXT("Diagnostics"));
}

FMonolithActionResult FMonolithGameplayMessageActions::GetStatus(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	const FExactObjectLoad SubsystemLoad = LoadExactObjectPath(SubsystemPath);
	const FExactObjectLoad AsyncActionLoad = LoadExactObjectPath(AsyncActionPath);
	const FExactObjectLoad ListenerHandleLoad = LoadExactObjectPath(ListenerHandlePath);
	const FExactObjectLoad MatchEnumLoad = LoadExactObjectPath(MatchEnumPath);

	TArray<TSharedPtr<FJsonValue>> Plugins;
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameplayMessageRouter"))));

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageRuntime"))));
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageNodes"))));

	TArray<TSharedPtr<FJsonValue>> Classes;
	Classes.Add(MakeShared<FJsonValueObject>(
		ClassSummary(SubsystemLoad, UGameInstanceSubsystem::StaticClass())));
	Classes.Add(MakeShared<FJsonValueObject>(
		ClassSummary(AsyncActionLoad, UObject::StaticClass())));

	TArray<TSharedPtr<FJsonValue>> Structs;
	Structs.Add(MakeShared<FJsonValueObject>(StructSummary(ListenerHandleLoad)));

	TSharedPtr<FJsonObject> MatchEnum = ExactLoadStatus(MatchEnumLoad);
	MatchEnum->SetBoolField(TEXT("is_enum"), Cast<UEnum>(MatchEnumLoad.Object) != nullptr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(
		TEXT("gameplay_message_runtime_available"),
		Cast<UClass>(SubsystemLoad.Object) != nullptr);
	Result->SetBoolField(
		TEXT("async_action_available"),
		Cast<UClass>(AsyncActionLoad.Object) != nullptr);
	Result->SetBoolField(TEXT("match_enum_available"), Cast<UEnum>(MatchEnumLoad.Object) != nullptr);
	Result->SetArrayField(TEXT("plugins"), Plugins);
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("classes"), Classes);
	Result->SetArrayField(TEXT("structs"), Structs);
	Result->SetObjectField(TEXT("match_enum"), MatchEnum);
	Result->SetArrayField(TEXT("listener_contract"), ListenerContractRows());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::DescribeListenerContract(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	const FExactObjectLoad SubsystemLoad = LoadExactObjectPath(SubsystemPath);
	const FExactObjectLoad AsyncActionLoad = LoadExactObjectPath(AsyncActionPath);
	UClass* SubsystemClass = Cast<UClass>(SubsystemLoad.Object);
	UClass* AsyncActionClass = Cast<UClass>(AsyncActionLoad.Object);

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(
		FunctionSummary(SubsystemClass, TEXT("K2_BroadcastMessage"))));
	Functions.Add(MakeShared<FJsonValueObject>(
		FunctionSummary(AsyncActionClass, TEXT("ListenForGameplayMessages"))));
	Functions.Add(MakeShared<FJsonValueObject>(
		FunctionSummary(AsyncActionClass, TEXT("GetPayload"))));

	TArray<TSharedPtr<FJsonValue>> MatchTypes;
	MatchTypes.Add(MakeShared<FJsonValueString>(TEXT("ExactMatch")));
	MatchTypes.Add(MakeShared<FJsonValueString>(TEXT("PartialMatch")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetObjectField(
		TEXT("subsystem_class"),
		ClassSummary(SubsystemLoad, UGameInstanceSubsystem::StaticClass()));
	Result->SetObjectField(
		TEXT("async_action_class"),
		ClassSummary(AsyncActionLoad, UObject::StaticClass()));
	Result->SetArrayField(TEXT("functions"), Functions);
	Result->SetArrayField(TEXT("match_types"), MatchTypes);
	Result->SetArrayField(TEXT("listener_contract"), ListenerContractRows());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::ValidateMessageStruct(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	FStrictParamReader Reader(Params);
	FString StructPath;
	bool bRequireBlueprintType = false;
	bool bRequireNoObjectReferences = false;
	if (!Reader.RequiredString(TEXT("message_struct"), StructPath)
		|| !Reader.OptionalBool(TEXT("require_blueprint_type"), bRequireBlueprintType, false)
		|| !Reader.OptionalBool(
			TEXT("require_no_object_references"),
			bRequireNoObjectReferences,
			false))
	{
		return InvalidParams(Reader.GetError());
	}

	const FExactObjectLoad Load = LoadExactObjectPath(StructPath);
	if (IsInvalidObjectPathInput(Load))
	{
		return InvalidParams(Load.ErrorDetail);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> MessageStruct = ValidateMessageStructInternal(
		Load,
		bRequireBlueprintType,
		bRequireNoObjectReferences,
		Checks,
		Issues,
		bOk);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetObjectField(TEXT("message_struct"), MessageStruct);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayMessageActions::ValidateChannelContract(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	FStrictParamReader Reader(Params);
	FString ChannelTagString;
	FString MatchType;
	FString StructPath;
	bool bRequireRegisteredTag = true;
	bool bRequireBlueprintType = false;
	if (!Reader.RequiredString(TEXT("channel_tag"), ChannelTagString)
		|| !Reader.OptionalString(TEXT("match_type"), MatchType, TEXT("ExactMatch"))
		|| !Reader.OptionalBool(
			TEXT("require_registered_tag"),
			bRequireRegisteredTag,
			true)
		|| !Reader.OptionalBool(
			TEXT("require_blueprint_type"),
			bRequireBlueprintType,
			false))
	{
		return InvalidParams(Reader.GetError());
	}

	if (Params.IsValid() && Params->HasField(TEXT("message_struct")))
	{
		if (!Reader.RequiredString(TEXT("message_struct"), StructPath))
		{
			return InvalidParams(Reader.GetError());
		}
	}

	if (!MatchType.Equals(TEXT("ExactMatch"), ESearchCase::CaseSensitive)
		&& !MatchType.Equals(TEXT("PartialMatch"), ESearchCase::CaseSensitive))
	{
		return InvalidParams(FString::Printf(
			TEXT("Param 'match_type' must be exactly ExactMatch or PartialMatch, got '%s'"),
			*MatchType));
	}

	FString TagError;
	if (!IsCanonicalGameplayTagString(ChannelTagString, TagError))
	{
		return InvalidParams(FString::Printf(
			TEXT("Param 'channel_tag' is not a valid gameplay tag: %s"),
			*TagError));
	}

	const FGameplayTag RequestedTag =
		UGameplayTagsManager::Get().RequestGameplayTag(FName(*ChannelTagString), false);
	const bool bRegisteredExact = RequestedTag.IsValid()
		&& RequestedTag.ToString().Equals(ChannelTagString, ESearchCase::CaseSensitive);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;
	AddCheck(
		Checks,
		bOk,
		TEXT("channel_tag_registered_exact"),
		bRegisteredExact || !bRequireRegisteredTag,
		bRequireRegisteredTag ? TEXT("error") : TEXT("warning"),
		bRegisteredExact
			? RequestedTag.ToString()
			: TEXT("tag is not registered with exact spelling and case"));
	AddCheck(
		Checks,
		bOk,
		TEXT("match_type_supported"),
		true,
		TEXT("info"),
		MatchType);

	TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
	Channel->SetStringField(TEXT("requested_channel_tag"), ChannelTagString);
	Channel->SetBoolField(TEXT("registered_exact"), bRegisteredExact);
	Channel->SetStringField(
		TEXT("resolved_channel_tag"),
		RequestedTag.IsValid() ? RequestedTag.ToString() : FString());
	Channel->SetStringField(TEXT("match_type"), MatchType);
	Channel->SetBoolField(TEXT("require_registered_tag"), bRequireRegisteredTag);

	TSharedPtr<FJsonObject> MessageStruct;
	if (!StructPath.IsEmpty())
	{
		const FExactObjectLoad Load = LoadExactObjectPath(StructPath);
		if (IsInvalidObjectPathInput(Load))
		{
			return InvalidParams(Load.ErrorDetail);
		}
		MessageStruct = ValidateMessageStructInternal(
			Load,
			bRequireBlueprintType,
			false,
			Checks,
			Issues,
			bOk);
	}

	if (!bRegisteredExact)
	{
		AddIssue(
			Issues,
			bRequireRegisteredTag ? TEXT("error") : TEXT("warning"),
			TEXT("channel_tag_not_registered_exact"),
			FString::Printf(
				TEXT("Gameplay tag '%s' is not registered with exact spelling and case."),
				*ChannelTagString));
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
