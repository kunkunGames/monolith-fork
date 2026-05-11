#include "MonolithToolProfileActions.h"

#include "Dom/JsonValue.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolProfileManager.h"
#include "MonolithToolRegistry.h"

namespace
{
FString GetProfileIdParam(const TSharedPtr<FJsonObject>& Params, bool bDefaultActive = false)
{
	FString ProfileId;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("profile_id"), ProfileId);
	}
	if (ProfileId.IsEmpty() && bDefaultActive)
	{
		ProfileId = FMonolithToolProfileManager::Get().GetActiveProfileId();
	}
	return ProfileId;
}

TArray<TSharedPtr<FJsonValue>> ProfilesToJsonArray(const TArray<FMonolithToolProfile>& Profiles)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Profiles.Num());
	for (const FMonolithToolProfile& Profile : Profiles)
	{
		Result.Add(MakeShared<FJsonValueObject>(FMonolithToolProfileManager::Get().ProfileToJson(Profile)));
	}
	return Result;
}

void ReadArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& Field, TSet<FString>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(Field, Array) || !Array)
	{
		return;
	}

	OutValues.Empty();
	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
		{
			OutValues.Add(Text);
		}
	}
}

void ReadDescriptionOverridesParam(const TSharedPtr<FJsonObject>& Params, FMonolithToolProfile& Profile)
{
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("description_overrides"), Obj) || !Obj)
	{
		return;
	}

	Profile.DescriptionOverrides.Empty();
	for (const auto& Pair : (*Obj)->Values)
	{
		FString Text;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(Text))
		{
			Profile.DescriptionOverrides.Add(Pair.Key, Text);
		}
	}
}

bool FillProfileFromParams(const TSharedPtr<FJsonObject>& Params, FMonolithToolProfile& Profile, FString& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("profile_id"), Profile.Id) || Profile.Id.IsEmpty())
	{
		OutError = TEXT("'profile_id' parameter is required");
		return false;
	}

	Params->TryGetStringField(TEXT("display_name"), Profile.DisplayName);
	Params->TryGetStringField(TEXT("description"), Profile.Description);
	Params->TryGetStringField(TEXT("mode"), Profile.Mode);
	Params->TryGetStringField(TEXT("custom_instructions"), Profile.CustomInstructions);
	if (Profile.DisplayName.IsEmpty())
	{
		Profile.DisplayName = Profile.Id;
	}
	if (Profile.Mode.IsEmpty())
	{
		Profile.Mode = TEXT("denylist");
	}
	if (Profile.Mode != TEXT("denylist") && Profile.Mode != TEXT("allowlist"))
	{
		OutError = TEXT("'mode' must be 'denylist' or 'allowlist'");
		return false;
	}

	ReadArrayParam(Params, TEXT("enabled_namespaces"), Profile.EnabledNamespaces);
	ReadArrayParam(Params, TEXT("enabled_actions"), Profile.EnabledActions);
	ReadArrayParam(Params, TEXT("disabled_namespaces"), Profile.DisabledNamespaces);
	ReadArrayParam(Params, TEXT("disabled_actions"), Profile.DisabledActions);
	ReadDescriptionOverridesParam(Params, Profile);
	return true;
}

TSharedPtr<FJsonObject> MakeSuccessWithProfile(const FString& Field, const FMonolithToolProfile& Profile)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(Field, FMonolithToolProfileManager::Get().ProfileToJson(Profile));
	Result->SetStringField(TEXT("active_profile"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	return Result;
}
}

void FMonolithToolProfileActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("monolith"), TEXT("list_tool_profiles"),
		TEXT("List local Monolith tool-surface profiles. No provider credentials are included."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleListToolProfiles));

	Registry.RegisterAction(TEXT("monolith"), TEXT("get_tool_profile"),
		TEXT("Get a Monolith tool profile definition."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleGetToolProfile),
		FParamSchemaBuilder().Required(TEXT("profile_id"), TEXT("string"), TEXT("Tool profile id")).Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("create_tool_profile"),
		TEXT("Create a local Monolith tool profile without provider credentials."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleCreateToolProfile),
		FParamSchemaBuilder()
			.Required(TEXT("profile_id"), TEXT("string"), TEXT("New profile id"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Display name"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Profile description"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("denylist or allowlist"), TEXT("denylist"))
			.Optional(TEXT("custom_instructions"), TEXT("string"), TEXT("Optional profile guidance"))
			.Optional(TEXT("enabled_namespaces"), TEXT("array"), TEXT("Allowlist namespace ids"))
			.Optional(TEXT("enabled_actions"), TEXT("array"), TEXT("Allowlist action ids namespace.action"))
			.Optional(TEXT("disabled_namespaces"), TEXT("array"), TEXT("Denylist namespace ids"))
			.Optional(TEXT("disabled_actions"), TEXT("array"), TEXT("Denylist action ids namespace.action"))
			.Optional(TEXT("description_overrides"), TEXT("object"), TEXT("Action description overrides keyed by namespace.action"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("update_tool_profile"),
		TEXT("Replace a local Monolith tool profile definition."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleUpdateToolProfile),
		FParamSchemaBuilder()
			.Required(TEXT("profile_id"), TEXT("string"), TEXT("Profile id"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Display name"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Profile description"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("denylist or allowlist"), TEXT("denylist"))
			.Optional(TEXT("custom_instructions"), TEXT("string"), TEXT("Optional profile guidance"))
			.Optional(TEXT("enabled_namespaces"), TEXT("array"), TEXT("Allowlist namespace ids"))
			.Optional(TEXT("enabled_actions"), TEXT("array"), TEXT("Allowlist action ids namespace.action"))
			.Optional(TEXT("disabled_namespaces"), TEXT("array"), TEXT("Denylist namespace ids"))
			.Optional(TEXT("disabled_actions"), TEXT("array"), TEXT("Denylist action ids namespace.action"))
			.Optional(TEXT("description_overrides"), TEXT("object"), TEXT("Action description overrides keyed by namespace.action"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("delete_tool_profile"),
		TEXT("Delete a non-built-in, inactive Monolith tool profile."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleDeleteToolProfile),
		FParamSchemaBuilder().Required(TEXT("profile_id"), TEXT("string"), TEXT("Profile id")).Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("set_active_tool_profile"),
		TEXT("Set the active Monolith tool profile for discovery and execution filtering."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleSetActiveToolProfile),
		FParamSchemaBuilder().Required(TEXT("profile_id"), TEXT("string"), TEXT("Profile id")).Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("set_action_enabled"),
		TEXT("Enable or disable one action in a Monolith tool profile."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleSetActionEnabled),
		FParamSchemaBuilder()
			.Optional(TEXT("profile_id"), TEXT("string"), TEXT("Profile id; defaults to active profile"))
			.Required(TEXT("action_id"), TEXT("string"), TEXT("Action id formatted as namespace.action"))
			.Optional(TEXT("enabled"), TEXT("bool"), TEXT("Whether the action should be enabled"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("set_namespace_enabled"),
		TEXT("Enable or disable one namespace in a Monolith tool profile."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleSetNamespaceEnabled),
		FParamSchemaBuilder()
			.Optional(TEXT("profile_id"), TEXT("string"), TEXT("Profile id; defaults to active profile"))
			.Required(TEXT("namespace"), TEXT("string"), TEXT("Namespace id"))
			.Optional(TEXT("enabled"), TEXT("bool"), TEXT("Whether the namespace should be enabled"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("set_action_description_override"),
		TEXT("Set or clear a profile-specific action description override."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleSetActionDescriptionOverride),
		FParamSchemaBuilder()
			.Optional(TEXT("profile_id"), TEXT("string"), TEXT("Profile id; defaults to active profile"))
			.Required(TEXT("action_id"), TEXT("string"), TEXT("Action id formatted as namespace.action"))
			.Optional(TEXT("description_override"), TEXT("string"), TEXT("Override text; empty clears it"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("get_effective_discovery"),
		TEXT("Return discovery after applying the active Monolith tool profile."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleGetEffectiveDiscovery),
		FParamSchemaBuilder()
			.Optional(TEXT("namespace"), TEXT("string"), TEXT("Optional namespace filter"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Optional category filter"))
			.Build());

	Registry.RegisterAction(TEXT("monolith"), TEXT("validate_tool_profile"),
		TEXT("Validate profile namespace/action ids against the registered Monolith action surface."),
		FMonolithActionHandler::CreateStatic(&FMonolithToolProfileActions::HandleValidateToolProfile),
		FParamSchemaBuilder()
			.Optional(TEXT("profile_id"), TEXT("string"), TEXT("Profile id; defaults to active profile"))
			.Build());
}

FMonolithActionResult FMonolithToolProfileActions::HandleListToolProfiles(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("active_profile"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetArrayField(TEXT("profiles"), ProfilesToJsonArray(FMonolithToolProfileManager::Get().ListProfiles()));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleGetToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params);
	TOptional<FMonolithToolProfile> Profile = FMonolithToolProfileManager::Get().GetProfile(ProfileId);
	if (!Profile.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId), FMonolithJsonUtils::ErrInvalidParams);
	}
	return FMonolithActionResult::Success(MakeSuccessWithProfile(TEXT("profile"), Profile.GetValue()));
}

FMonolithActionResult FMonolithToolProfileActions::HandleCreateToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolProfile Profile;
	FString Error;
	if (!FillProfileFromParams(Params, Profile, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!FMonolithToolProfileManager::Get().UpsertProfile(Profile, true, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	return FMonolithActionResult::Success(MakeSuccessWithProfile(TEXT("profile"), Profile));
}

FMonolithActionResult FMonolithToolProfileActions::HandleUpdateToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolProfile Profile;
	FString Error;
	if (!FillProfileFromParams(Params, Profile, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!FMonolithToolProfileManager::Get().UpsertProfile(Profile, false, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	return FMonolithActionResult::Success(MakeSuccessWithProfile(TEXT("profile"), Profile));
}

FMonolithActionResult FMonolithToolProfileActions::HandleDeleteToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params);
	FString Error;
	if (!FMonolithToolProfileManager::Get().DeleteProfile(ProfileId, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("deleted_profile"), ProfileId);
	Result->SetStringField(TEXT("active_profile"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleSetActiveToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params);
	FString Error;
	if (!FMonolithToolProfileManager::Get().SetActiveProfile(ProfileId, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("active_profile"), ProfileId);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleSetActionEnabled(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params, true);
	FString ActionId;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("action_id"), ActionId) || ActionId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'action_id' parameter is required"), FMonolithJsonUtils::ErrInvalidParams);
	}
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	FString Error;
	if (!FMonolithToolProfileManager::Get().SetActionEnabled(ProfileId, ActionId, bEnabled, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("profile_id"), ProfileId);
	Result->SetStringField(TEXT("action_id"), ActionId);
	Result->SetBoolField(TEXT("enabled"), bEnabled);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleSetNamespaceEnabled(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params, true);
	FString Namespace;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("namespace"), Namespace) || Namespace.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'namespace' parameter is required"), FMonolithJsonUtils::ErrInvalidParams);
	}
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	FString Error;
	if (!FMonolithToolProfileManager::Get().SetNamespaceEnabled(ProfileId, Namespace, bEnabled, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("profile_id"), ProfileId);
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("enabled"), bEnabled);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleSetActionDescriptionOverride(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params, true);
	FString ActionId;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("action_id"), ActionId) || ActionId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'action_id' parameter is required"), FMonolithJsonUtils::ErrInvalidParams);
	}
	FString Override;
	Params->TryGetStringField(TEXT("description_override"), Override);

	FString Error;
	if (!FMonolithToolProfileManager::Get().SetDescriptionOverride(ProfileId, ActionId, Override, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("profile_id"), ProfileId);
	Result->SetStringField(TEXT("action_id"), ActionId);
	Result->SetBoolField(TEXT("cleared"), Override.IsEmpty());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleGetEffectiveDiscovery(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterNamespace;
	FString FilterCategory;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("namespace"), FilterNamespace);
		Params->TryGetStringField(TEXT("category"), FilterCategory);
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("active_profile"), FMonolithToolProfileManager::Get().GetActiveProfileId());

	auto ActionToJson = [](const FMonolithActionInfo& Info)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("namespace"), Info.Namespace);
		Obj->SetStringField(TEXT("action"), Info.Action);
		Obj->SetStringField(TEXT("description"), Info.Description);
		if (!Info.Category.IsEmpty())
		{
			Obj->SetStringField(TEXT("category"), Info.Category);
		}
		if (Info.ParamSchema.IsValid())
		{
			Obj->SetObjectField(TEXT("params"), Info.ParamSchema);
		}
		return Obj;
	};

	if (!FilterNamespace.IsEmpty())
	{
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(FilterNamespace);
		if (!FilterCategory.IsEmpty())
		{
			Actions = Actions.FilterByPredicate([&FilterCategory](const FMonolithActionInfo& Info)
			{
				return Info.Category.Equals(FilterCategory, ESearchCase::IgnoreCase);
			});
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Actions.Num());
		for (const FMonolithActionInfo& Info : Actions)
		{
			Items.Add(MakeShared<FJsonValueObject>(ActionToJson(Info)));
		}
		Result->SetStringField(TEXT("namespace"), FilterNamespace);
		Result->SetArrayField(TEXT("actions"), Items);
		Result->SetNumberField(TEXT("action_count"), Items.Num());
		return FMonolithActionResult::Success(Result);
	}

	TArray<TSharedPtr<FJsonValue>> Namespaces;
	for (const FString& Namespace : Registry.GetNamespaces())
	{
		TArray<FString> ActionNames = Registry.GetActionNames(Namespace);
		TArray<TSharedPtr<FJsonValue>> Actions;
		Actions.Reserve(ActionNames.Num());
		for (const FString& ActionName : ActionNames)
		{
			Actions.Add(MakeShared<FJsonValueString>(ActionName));
		}

		TSharedPtr<FJsonObject> Ns = MakeShared<FJsonObject>();
		Ns->SetStringField(TEXT("namespace"), Namespace);
		Ns->SetNumberField(TEXT("action_count"), ActionNames.Num());
		Ns->SetArrayField(TEXT("actions"), Actions);
		Namespaces.Add(MakeShared<FJsonValueObject>(Ns));
	}
	Result->SetArrayField(TEXT("namespaces"), Namespaces);
	Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithToolProfileActions::HandleValidateToolProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString ProfileId = GetProfileIdParam(Params, true);
	TArray<FString> UnknownNamespaces;
	TArray<FString> UnknownActions;
	FString Error;
	if (!FMonolithToolProfileManager::Get().ValidateProfile(ProfileId, UnknownNamespaces, UnknownActions, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	auto StringsToJson = [](const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("profile_id"), ProfileId);
	Result->SetBoolField(TEXT("valid"), UnknownNamespaces.Num() == 0 && UnknownActions.Num() == 0);
	Result->SetArrayField(TEXT("unknown_namespaces"), StringsToJson(UnknownNamespaces));
	Result->SetArrayField(TEXT("unknown_actions"), StringsToJson(UnknownActions));
	return FMonolithActionResult::Success(Result);
}
