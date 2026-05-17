#include "MonolithCoreTools.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithHttpServer.h"
#include "MonolithMcpSessionTracker.h"
#include "MonolithParamSchema.h"
#include "MonolithResourceRegistry.h"
#include "MonolithSettings.h"
#include "MonolithToolProfileManager.h"
#include "MonolithUpdateSubsystem.h"
#include "Dom/JsonValue.h"
#include "EditorSubsystem.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Editor.h"

// Known optional modules — namespaces that may not have registered actions
// depending on settings or missing plugin dependencies.
struct FKnownOptionalModule
{
	FString Namespace;
	FString SettingsField;   // bool property name on UMonolithSettings
	FString ToolName;        // MCP tool name (namespace_query)
	FString InstallHint;
};

static const TArray<FKnownOptionalModule>& GetKnownOptionalModules()
{
	static const TArray<FKnownOptionalModule> Modules = {
		{
			TEXT("gas"),
			TEXT("bEnableGAS"),
			TEXT("gas_query"),
			TEXT("MonolithGAS module provides Gameplay Ability System tooling (attributes, abilities, effects, cues). Requires GameplayAbilities plugin (engine-bundled).")
		},
		{
			TEXT("combograph"),
			TEXT("bEnableComboGraph"),
			TEXT("combograph_query"),
			TEXT("MonolithComboGraph module provides combo graph tooling (nodes, edges, transitions, effects). Requires ComboGraph plugin (Fab marketplace).")
		}
	};
	return Modules;
}

struct FNotificationBoolSetting
{
	const TCHAR* Name;
	bool UMonolithSettings::* Member;
};

static const TArray<FNotificationBoolSetting>& GetNotificationSettings()
{
	static const TArray<FNotificationBoolSetting> Settings = {
		{TEXT("editor_toasts"), &UMonolithSettings::bNotifyEditorToasts},
		{TEXT("sounds"), &UMonolithSettings::bNotifySounds},
		{TEXT("taskbar_attention"), &UMonolithSettings::bNotifyTaskbarAttention},
		{TEXT("server_errors"), &UMonolithSettings::bNotifyServerErrors},
		{TEXT("action_errors"), &UMonolithSettings::bNotifyActionErrors},
		{TEXT("long_running_action_complete"), &UMonolithSettings::bNotifyLongRunningActionComplete},
		{TEXT("indexing_complete"), &UMonolithSettings::bNotifyIndexingComplete},
		{TEXT("update_available"), &UMonolithSettings::bNotifyUpdateAvailable}
	};
	return Settings;
}

static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

static FString NormalizeDomainNamespace(FString Namespace)
{
	Namespace.TrimStartAndEndInline();
	Namespace.ToLowerInline();
	return Namespace;
}

static bool IsDeferredDomainCatalogEnabled(const UMonolithSettings* Settings)
{
	return Settings && Settings->bEnableDeferredDomainCatalog;
}

static bool IsDomainToolExposureEnabled(const UMonolithSettings* Settings)
{
	return Settings && Settings->bEnableDeferredDomainCatalog && Settings->bExposeLoadedDomainsAsMcpTools;
}

static TSharedPtr<FJsonObject> MakeFeatureStatus(bool bConfigured, bool bActive, const FString& State)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("compiled"), true);
	Obj->SetBoolField(TEXT("configured"), bConfigured);
	Obj->SetBoolField(TEXT("active"), bActive);
	Obj->SetStringField(TEXT("state"), State);
	return Obj;
}

static TSharedPtr<FJsonObject> MakeSettingsOnlyFeatureStatus(bool bConfigured, const FString& State)
{
	return MakeFeatureStatus(bConfigured, false, State);
}

static TSharedPtr<FJsonObject> MakeDeferredDomainCatalogStatus(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	const bool bConfigured = IsDeferredDomainCatalogEnabled(Settings);
	const bool bExposeTools = IsDomainToolExposureEnabled(Settings);
	// `active` must reflect *runtime* registry state, not just the config flag.
	// RegisterAll only registers the catalog handlers when bEnableDeferredDomainCatalog
	// was true at registration time; after a settings toggle without a restart, the
	// flag and registry can disagree.
	const bool bHandlersRegistered =
		FMonolithToolRegistry::Get().HasAction(TEXT("monolith"), TEXT("list_domains"));
	Obj->SetBoolField(TEXT("compiled"), true);
	Obj->SetBoolField(TEXT("configured"), bConfigured);
	Obj->SetBoolField(TEXT("active"), bHandlersRegistered);
	Obj->SetBoolField(TEXT("handlers_registered"), bHandlersRegistered);
	if (bConfigured != bHandlersRegistered)
	{
		Obj->SetBoolField(TEXT("restart_required"), true);
	}
	Obj->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Obj->SetBoolField(TEXT("domain_tool_exposure"), bExposeTools);
	Obj->SetStringField(TEXT("tool_exposure_mode"), bExposeTools ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Obj->SetStringField(TEXT("tool_list_mutation"), TEXT("not_implemented_in_metadata_slice"));
	return Obj;
}

static TSharedPtr<FJsonObject> MakeMcpResourcesStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableMcpResources;
	const bool bRegistryInitialized = FMonolithResourceRegistry::Get().HasDefaultResourcesRegistered();
	const int32 ResourceCount = FMonolithResourceRegistry::Get().GetResourceCount();
	const bool bActive = bConfigured && bRegistryInitialized;

	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bActive,
		bActive ? TEXT("active_readonly_registry") : (bConfigured ? TEXT("configured_restart_required") : TEXT("disabled")));
	Obj->SetBoolField(TEXT("handlers_registered"), bActive);
	Obj->SetBoolField(TEXT("provider_registry_initialized"), bRegistryInitialized);
	Obj->SetNumberField(TEXT("resource_count"), ResourceCount);
	Obj->SetStringField(TEXT("provider_mode"), TEXT("explicit_readonly"));
	Obj->SetStringField(TEXT("payload_mode"), TEXT("bounded_text"));
	if (bConfigured && !bRegistryInitialized)
	{
		Obj->SetBoolField(TEXT("restart_required"), true);
	}
	return Obj;
}

static TSharedPtr<FJsonObject> MakeStructuredToolResultsStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableStructuredToolResults;
	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bConfigured,
		bConfigured ? TEXT("active_structured_content") : TEXT("disabled"));
	Obj->SetBoolField(TEXT("legacy_text_json"), true);
	Obj->SetStringField(TEXT("content_mode"), bConfigured ? TEXT("text_plus_structured_content") : TEXT("legacy_text_json_only"));
	Obj->SetStringField(TEXT("scope"), TEXT("tools_call_response_envelope"));
	return Obj;
}

static TSharedPtr<FJsonObject> MakeMcpSessionModeStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableMcpSessionMode;
	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bConfigured,
		bConfigured ? TEXT("active_in_memory_observer") : TEXT("disabled"));
	Obj->SetStringField(TEXT("mode"), bConfigured ? TEXT("in_memory_observer") : TEXT("stateless"));
	Obj->SetBoolField(TEXT("raw_session_ids_stored"), false);
	Obj->SetBoolField(TEXT("persistent"), false);
	Obj->SetBoolField(TEXT("progress_notifications"), false);
	Obj->SetBoolField(TEXT("request_cancellation"), false);
	return Obj;
}

static FString BuildDomainDescription(const FString& Namespace, const TArray<FMonolithActionInfo>& Actions)
{
	if (Actions.Num() == 0)
	{
		return FString::Printf(TEXT("%s domain has no profile-allowed actions."), *Namespace);
	}

	TSet<FString> Categories;
	for (const FMonolithActionInfo& Action : Actions)
	{
		if (!Action.Category.IsEmpty())
		{
			Categories.Add(Action.Category);
		}
	}

	if (Categories.Num() > 0)
	{
		TArray<FString> CategoryList = Categories.Array();
		CategoryList.Sort();
		return FString::Printf(TEXT("%s domain with %d profile-allowed actions across categories: %s."),
			*Namespace,
			Actions.Num(),
			*FString::Join(CategoryList, TEXT(", ")));
	}

	return FString::Printf(TEXT("%s domain with %d profile-allowed actions."), *Namespace, Actions.Num());
}

static FCriticalSection GLoadedDomainCatalogLock;
static TMap<FString, TSet<FString>> GLoadedDomainsByProfile;

static TSet<FString>& GetLoadedDomainsForActiveProfile_NoLock()
{
	return GLoadedDomainsByProfile.FindOrAdd(FMonolithToolProfileManager::Get().GetActiveProfileId());
}

static bool IsDomainLoadedForActiveProfile(const FString& Namespace)
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	return GetLoadedDomainsForActiveProfile_NoLock().Contains(Namespace);
}

static TArray<FString> GetLoadedDomainsSnapshotForActiveProfile()
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	TArray<FString> Domains = GetLoadedDomainsForActiveProfile_NoLock().Array();
	Domains.Sort();
	return Domains;
}

static bool MarkDomainLoadedForActiveProfile(const FString& Namespace)
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	TSet<FString>& LoadedDomains = GetLoadedDomainsForActiveProfile_NoLock();
	const bool bAlreadyLoaded = LoadedDomains.Contains(Namespace);
	LoadedDomains.Add(Namespace);
	return !bAlreadyLoaded;
}

static TSharedPtr<FJsonObject> MakeDomainCatalogDisabledResult()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("disabled"));
	Result->SetBoolField(TEXT("deferred_enabled"), false);
	Result->SetStringField(TEXT("reason"), TEXT("bEnableDeferredDomainCatalog is false. Enable it in Monolith MCP Server discovery settings and restart the editor to register catalog tools."));
	Result->SetObjectField(TEXT("feature"), MakeDeferredDomainCatalogStatus(UMonolithSettings::Get()));
	return Result;
}

static TSharedPtr<FJsonObject> NotificationSettingsToJson(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Settings)
	{
		return Obj;
	}

	for (const FNotificationBoolSetting& Def : GetNotificationSettings())
	{
		Obj->SetBoolField(Def.Name, Settings->*(Def.Member));
	}
	return Obj;
}

static const TArray<FString>& GetOnboardingSteps()
{
	static const TArray<FString> Steps = {
		TEXT("server_ready"),
		TEXT("index_ready"),
		TEXT("optional_modules_reviewed"),
		TEXT("notifications_reviewed")
	};
	return Steps;
}

static FString GetNextOnboardingStep(const UMonolithSettings* Settings)
{
	if (!Settings)
	{
		return TEXT("server_ready");
	}

	for (const FString& Step : GetOnboardingSteps())
	{
		if (!Settings->OnboardingCompletedSteps.Contains(Step)
			&& !Settings->OnboardingSkippedSteps.Contains(Step))
		{
			return Step;
		}
	}
	return FString();
}

static TSharedPtr<FJsonObject> OnboardingStateToJson(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Settings)
	{
		Obj->SetStringField(TEXT("status"), TEXT("settings_unavailable"));
		return Obj;
	}

	Obj->SetStringField(TEXT("status"), TEXT("ok"));
	Obj->SetNumberField(TEXT("schema_version"), Settings->OnboardingSchemaVersion);
	Obj->SetArrayField(TEXT("known_steps"), StringArrayToJson(GetOnboardingSteps()));
	Obj->SetArrayField(TEXT("completed_steps"), StringArrayToJson(Settings->OnboardingCompletedSteps));
	Obj->SetArrayField(TEXT("skipped_steps"), StringArrayToJson(Settings->OnboardingSkippedSteps));
	const FString NextStep = GetNextOnboardingStep(Settings);
	Obj->SetStringField(TEXT("next_recommended_step"), NextStep.IsEmpty() ? TEXT("complete") : NextStep);
	Obj->SetBoolField(TEXT("complete"), NextStep.IsEmpty());
	return Obj;
}

static void AddReadinessItem(
	TArray<TSharedPtr<FJsonValue>>& Items,
	int32& ErrorCount,
	int32& WarningCount,
	const FString& Component,
	bool bOk,
	const FString& CurrentState,
	const FString& ExpectedState,
	const FString& Help,
	const FString& SeverityWhenNotOk = TEXT("error"))
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("component"), Component);
	Item->SetBoolField(TEXT("ok"), bOk);
	Item->SetStringField(TEXT("current_state"), CurrentState);
	Item->SetStringField(TEXT("expected_state"), ExpectedState);
	Item->SetStringField(TEXT("help"), Help);

	FString Severity = TEXT("ok");
	if (!bOk)
	{
		Severity = SeverityWhenNotOk;
		if (Severity == TEXT("warning"))
		{
			++WarningCount;
		}
		else
		{
			++ErrorCount;
		}
	}
	Item->SetStringField(TEXT("severity"), Severity);
	Items.Add(MakeShared<FJsonValueObject>(Item));
}

static TSharedPtr<FJsonObject> ReadinessHelpToJson(const FString& Component, const FString& Summary, const FString& Guidance)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("component"), Component);
	Obj->SetStringField(TEXT("summary"), Summary);
	Obj->SetStringField(TEXT("guidance"), Guidance);
	return Obj;
}

void FMonolithCoreTools::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// monolith_discover
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NsProp = MakeShared<FJsonObject>();
		NsProp->SetStringField(TEXT("type"), TEXT("string"));
		NsProp->SetStringField(TEXT("description"), TEXT("Optional: filter to a specific namespace"));
		Schema->SetObjectField(TEXT("namespace"), NsProp);

		TSharedPtr<FJsonObject> CatProp = MakeShared<FJsonObject>();
		CatProp->SetStringField(TEXT("type"), TEXT("string"));
		CatProp->SetStringField(TEXT("description"), TEXT("Optional: filter actions within the namespace by category (e.g. 'CommonUI' inside 'ui')"));
		Schema->SetObjectField(TEXT("category"), CatProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("discover"),
			TEXT("List available tool namespaces and their actions. Pass namespace (and optional category) to filter."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDiscover),
			Schema
		);
	}

	// monolith_status
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("status"),
			TEXT("Get Monolith server health: version, uptime, port, registered action count, module status."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleStatus)
		);
	}

	// monolith_update
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
		ActionProp->SetStringField(TEXT("type"), TEXT("string"));
		ActionProp->SetStringField(TEXT("description"), TEXT("'check' to compare versions, 'install' to download and stage update"));
		ActionProp->SetStringField(TEXT("default"), TEXT("check"));
		Schema->SetObjectField(TEXT("action"), ActionProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("update"),
			TEXT("Check for or install Monolith updates from GitHub Releases."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleUpdate),
			Schema
		);
	}

	// monolith_reindex
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("reindex"),
			TEXT("Re-index the Monolith project database. Incremental by default (delta only). Pass force=true for full wipe+rebuild."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleReindex),
			FParamSchemaBuilder()
				.Optional(TEXT("force"), TEXT("boolean"), TEXT("If true, performs a full wipe and rebuild instead of an incremental delta update."), TEXT("false"))
				.Build()
		);
	}

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_mcp_server_status"),
		TEXT("Return Monolith MCP transport status, CORS/header policy, protocol support, route state, and request limits."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetMcpServerStatus)
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("list_mcp_sessions"),
		TEXT("Report MCP session tracking availability. Current Monolith streamable HTTP mode does not persist per-client sessions."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleListMcpSessions),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum session rows to return when session tracking is available"), TEXT("100"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("terminate_mcp_session"),
		TEXT("Report MCP session termination availability without inventing session state."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleTerminateMcpSession),
		FParamSchemaBuilder()
			.Required(TEXT("session_id"), TEXT("string"), TEXT("MCP session id to terminate when session tracking is available"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_mcp_compatibility_options"),
		TEXT("Report MCP compatibility option mutability. Legacy route/browser compatibility remains disabled until settings-backed policy lands."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetMcpCompatibilityOptions),
		FParamSchemaBuilder()
			.Optional(TEXT("options"), TEXT("object"), TEXT("Requested compatibility options for a future settings-backed implementation"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_mcp_discovery_state"),
		TEXT("Return the current live registry discovery snapshot and refresh semantics."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetMcpDiscoveryState)
	);

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (IsDeferredDomainCatalogEnabled(Settings))
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("list_domains"),
			TEXT("Return cheap profile-filtered Monolith domain metadata without per-action schemas."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleListDomains),
			FParamSchemaBuilder()
				.Optional(TEXT("include_optional"), TEXT("boolean"), TEXT("Include known optional domains that are not currently registered"), TEXT("true"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("describe_domain"),
			TEXT("Return one Monolith domain's profile-filtered actions and schemas without changing tools/list."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDescribeDomain),
			FParamSchemaBuilder()
				.Required(TEXT("namespace"), TEXT("string"), TEXT("Domain namespace to describe"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("load_domain"),
			TEXT("Mark a Monolith domain loaded for discovery scope without exposing additional tools by default."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleLoadDomain),
			FParamSchemaBuilder()
				.Required(TEXT("namespace"), TEXT("string"), TEXT("Domain namespace to mark loaded"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("get_loaded_domains"),
			TEXT("Return process/profile-scoped loaded domain state for the deferred domain catalog."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetLoadedDomains)
		);
	}

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_onboarding_state"),
		TEXT("Return local Monolith onboarding progress, skipped steps, and next recommended setup step."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetOnboardingState)
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_onboarding_state"),
		TEXT("Mark a Monolith onboarding step completed, skipped, reopened, or reset."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetOnboardingState),
		FParamSchemaBuilder()
			.Optional(TEXT("action"), TEXT("string"), TEXT("complete, skip, reopen, or reset"), TEXT("complete"))
			.Optional(TEXT("step"), TEXT("string"), TEXT("Onboarding step id to update"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_readiness_status"),
		TEXT("Run read-only Monolith readiness checks for server, registry, index, optional modules, and settings gates."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetReadinessStatus)
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_readiness_help"),
		TEXT("Return safe help text for Monolith readiness check failures without running installers."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetReadinessHelp),
		FParamSchemaBuilder()
			.Optional(TEXT("component"), TEXT("string"), TEXT("Optional readiness component id to filter help"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_notification_settings"),
		TEXT("Return local Monolith notification preferences."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetNotificationSettings)
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_notification_settings"),
		TEXT("Persist local Monolith notification preferences with boolean validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetNotificationSettings),
		FParamSchemaBuilder()
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Object of notification preference booleans to update"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("test_notification"),
		TEXT("Trigger a harmless local Monolith notification test when editor toasts are enabled."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleTestNotification),
		FParamSchemaBuilder()
			.Optional(TEXT("message"), TEXT("string"), TEXT("Optional notification text"), TEXT("Monolith notification test"))
			.Build()
	);
}

FMonolithActionResult FMonolithCoreTools::HandleDiscover(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FString FilterNamespace;
	FString FilterCategory;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("namespace")) && !Params->TryGetStringField(TEXT("namespace"), FilterNamespace))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'namespace' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("category")) && !Params->TryGetStringField(TEXT("category"), FilterCategory))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'category' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<FString> Namespaces = Registry.GetNamespaces();

	if (!FilterNamespace.IsEmpty())
	{
		// Filter to specific namespace — return detailed action list
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(FilterNamespace);
		if (Actions.Num() == 0)
		{
			// Check if this is a known optional module
			const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
			const FKnownOptionalModule* Found = nullptr;
			for (const FKnownOptionalModule& Mod : OptionalModules)
			{
				if (Mod.Namespace.Equals(FilterNamespace, ESearchCase::IgnoreCase))
				{
					Found = &Mod;
					break;
				}
			}

			if (Found)
			{
				// Determine disabled vs not_installed by checking the settings toggle
				const UMonolithSettings* Settings = UMonolithSettings::Get();
				bool bSettingEnabled = false;
				if (Settings)
				{
					const FBoolProperty* Prop = CastField<FBoolProperty>(
						UMonolithSettings::StaticClass()->FindPropertyByName(*Found->SettingsField));
					if (Prop)
					{
						bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
					}
				}

				Result->SetStringField(TEXT("namespace"), Found->Namespace);
				Result->SetNumberField(TEXT("actions"), 0);

				if (!bSettingEnabled)
				{
					Result->SetStringField(TEXT("status"), TEXT("disabled"));
					Result->SetStringField(TEXT("hint"),
						FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."),
							*Found->SettingsField));
				}
				else
				{
					Result->SetStringField(TEXT("status"), TEXT("not_installed"));
					Result->SetStringField(TEXT("hint"), Found->InstallHint);
				}

				return FMonolithActionResult::Success(Result);
			}

			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown namespace: %s"), *FilterNamespace),
				FMonolithJsonUtils::ErrInvalidParams
			);
		}

		// Apply optional category filter (only meaningful when namespace is specified).
		if (!FilterCategory.IsEmpty())
		{
			Actions = Actions.FilterByPredicate([&FilterCategory](const FMonolithActionInfo& Info)
			{
				return Info.Category.Equals(FilterCategory, ESearchCase::IgnoreCase);
			});
		}

		Result->SetStringField(TEXT("namespace"), FilterNamespace);
		if (!FilterCategory.IsEmpty())
		{
			Result->SetStringField(TEXT("category"), FilterCategory);
		}
		TArray<TSharedPtr<FJsonValue>> ActionArray;
		ActionArray.Reserve(Actions.Num());
		for (const FMonolithActionInfo& ActionInfo : Actions)
		{
			TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
			ActionObj->SetStringField(TEXT("action"), ActionInfo.Action);
			ActionObj->SetStringField(TEXT("description"), ActionInfo.Description);
			if (!ActionInfo.Category.IsEmpty())
			{
				ActionObj->SetStringField(TEXT("category"), ActionInfo.Category);
			}
			if (ActionInfo.ParamSchema.IsValid())
			{
				ActionObj->SetObjectField(TEXT("params"), ActionInfo.ParamSchema);
			}
			ActionArray.Add(MakeShared<FJsonValueObject>(ActionObj));
		}
		Result->SetArrayField(TEXT("actions"), ActionArray);
	}
	else
	{
		// Return all namespaces with action counts
		TArray<TSharedPtr<FJsonValue>> NsArray;
		NsArray.Reserve(Namespaces.Num());
		for (const FString& Ns : Namespaces)
		{
			TArray<FString> Actions = Registry.GetActionNames(Ns);
			TSharedPtr<FJsonObject> NsObj = MakeShared<FJsonObject>();
			NsObj->SetStringField(TEXT("namespace"), Ns);
			NsObj->SetNumberField(TEXT("action_count"), Actions.Num());

			TArray<TSharedPtr<FJsonValue>> ActionNames;
			ActionNames.Reserve(Actions.Num());
			for (const FString& ActionName : Actions)
			{
				ActionNames.Add(MakeShared<FJsonValueString>(ActionName));
			}
			NsObj->SetArrayField(TEXT("actions"), ActionNames);
			NsArray.Add(MakeShared<FJsonValueObject>(NsObj));
		}
		// Append known optional modules that aren't already registered
		const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
		const UMonolithSettings* Settings = UMonolithSettings::Get();

		TArray<TSharedPtr<FJsonValue>> OptionalArray;
		OptionalArray.Reserve(OptionalModules.Num());
		for (const FKnownOptionalModule& Mod : OptionalModules)
		{
			// Skip if this namespace already has registered actions (it's active)
			if (Namespaces.Contains(Mod.Namespace))
			{
				continue;
			}

			TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
			OptObj->SetStringField(TEXT("namespace"), Mod.Namespace);
			OptObj->SetStringField(TEXT("tool"), Mod.ToolName);
			OptObj->SetNumberField(TEXT("action_count"), 0);

			bool bSettingEnabled = false;
			if (Settings)
			{
				const FBoolProperty* Prop = CastField<FBoolProperty>(
					UMonolithSettings::StaticClass()->FindPropertyByName(*Mod.SettingsField));
				if (Prop)
				{
					bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
				}
			}

			OptObj->SetStringField(TEXT("status"), bSettingEnabled ? TEXT("not_installed") : TEXT("disabled"));
			OptObj->SetStringField(TEXT("hint"), bSettingEnabled ? Mod.InstallHint
				: FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."), *Mod.SettingsField));

			OptionalArray.Add(MakeShared<FJsonValueObject>(OptObj));
		}

		if (OptionalArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("optional_modules"), OptionalArray);
		}

		Result->SetArrayField(TEXT("namespaces"), NsArray);
		Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Version
	Result->SetStringField(TEXT("version"), MONOLITH_VERSION);

	// Server status
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	Result->SetBoolField(TEXT("server_running"), Server != nullptr && Server->IsRunning());
	Result->SetNumberField(TEXT("server_port"), Server ? Server->GetPort() : 0);

	// Registry stats
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	Result->SetNumberField(TEXT("namespaces"), Registry.GetNamespaceCount());

	// Engine info
	Result->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());

	// Project info
	Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleUpdate(const TSharedPtr<FJsonObject>& Params)
{
	FString Action = TEXT("check");
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("action")) && !Params->TryGetStringField(TEXT("action"), Action))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'action' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithUpdateSubsystem* UpdateSubsystem = GEditor->GetEditorSubsystem<UMonolithUpdateSubsystem>();
	if (!UpdateSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithUpdateSubsystem not available"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Action == TEXT("check"))
	{
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		Result->SetStringField(TEXT("current_version"), Info.Current);
		Result->SetStringField(TEXT("pending_version"), Info.Pending.IsEmpty() ? TEXT("none") : Info.Pending);
		Result->SetBoolField(TEXT("staging"), Info.bStaging);
		Result->SetStringField(TEXT("status"), TEXT("check_initiated"));

		// Trigger async check — result will come via notification
		UpdateSubsystem->CheckForUpdate();

		return FMonolithActionResult::Success(Result);
	}
	else if (Action == TEXT("install"))
	{
		// Install requires a previous check to have found a version
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		if (Info.bStaging)
		{
			Result->SetStringField(TEXT("status"), TEXT("already_staged"));
			Result->SetStringField(TEXT("pending_version"), Info.Pending);
			Result->SetStringField(TEXT("message"), TEXT("Update already staged. Restart the editor to apply."));
			return FMonolithActionResult::Success(Result);
		}

		// Trigger a check that will show the notification with install button
		UpdateSubsystem->CheckForUpdate();
		Result->SetStringField(TEXT("status"), TEXT("checking_for_installable_update"));
		Result->SetStringField(TEXT("message"), TEXT("Checking GitHub for latest release. If available, an install notification will appear."));
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(
		FString::Printf(TEXT("Unknown update action: %s. Use 'check' or 'install'."), *Action),
		FMonolithJsonUtils::ErrInvalidParams
	);
}

FMonolithActionResult FMonolithCoreTools::HandleReindex(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("MonolithIndex")))
	{
		Result->SetStringField(TEXT("status"), TEXT("module_not_loaded"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndex module is not loaded."));
		return FMonolithActionResult::Success(Result);
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bForce = false;
	if (Params.IsValid() && Params->HasField(TEXT("force")))
	{
		if (!Params->TryGetBoolField(TEXT("force"), bForce))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'force' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	UClass* IndexSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/MonolithIndex.MonolithIndexSubsystem"));
	if (!IndexSubsystemClass)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem class not found."));
		return FMonolithActionResult::Success(Result);
	}

	UEditorSubsystem* IndexSubsystem = GEditor->GetEditorSubsystemBase(IndexSubsystemClass);
	if (!IndexSubsystem)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem instance not available."));
		return FMonolithActionResult::Success(Result);
	}

	FString FuncName;
	if (bForce)
	{
		FuncName = TEXT("StartFullIndex");
	}
	else
	{
		// Check if incremental is possible
		UFunction* CanIncrementalFunc = IndexSubsystemClass->FindFunctionByName(TEXT("CanDoIncrementalIndex"));
		if (CanIncrementalFunc)
		{
			struct { uint8 ReturnValue = 0; } Parms;
			FMemory::Memzero(&Parms, sizeof(Parms));
			IndexSubsystem->ProcessEvent(CanIncrementalFunc, &Parms);
			FuncName = Parms.ReturnValue != 0 ? TEXT("StartIncrementalIndex") : TEXT("StartFullIndex");
		}
		else
		{
			// Fallback if CanDoIncrementalIndex not found (old MonolithIndex version)
			FuncName = TEXT("StartFullIndex");
		}
	}

	UFunction* Func = IndexSubsystemClass->FindFunctionByName(*FuncName);
	if (Func)
	{
		IndexSubsystem->ProcessEvent(Func, nullptr);
		Result->SetStringField(TEXT("status"), TEXT("reindex_started"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s triggered successfully."),
				FuncName == TEXT("StartFullIndex") ? TEXT("Full re-index") : TEXT("Incremental index")));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("function_not_found"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Function %s not found."), *FuncName));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetOnboardingState(const TSharedPtr<FJsonObject>& /*Params*/)
{
	return FMonolithActionResult::Success(OnboardingStateToJson(UMonolithSettings::Get()));
}

FMonolithActionResult FMonolithCoreTools::HandleSetOnboardingState(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("Monolith settings unavailable"));
	}

	FString Action = TEXT("complete");
	FString Step;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("action")) && !Params->TryGetStringField(TEXT("action"), Action))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'action' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Params->HasField(TEXT("step")) && !Params->TryGetStringField(TEXT("step"), Step))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'step' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Action = Action.ToLower();

	if (Action == TEXT("reset"))
	{
		Settings->OnboardingCompletedSteps.Reset();
		Settings->OnboardingSkippedSteps.Reset();
	}
	else
	{
		if (Step.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("step is required unless action is reset"), FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!GetOnboardingSteps().Contains(Step))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown onboarding step: %s"), *Step), FMonolithJsonUtils::ErrInvalidParams);
		}

		if (Action != TEXT("complete") && Action != TEXT("skip") && Action != TEXT("reopen"))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown onboarding action: %s"), *Action), FMonolithJsonUtils::ErrInvalidParams);
		}

		Settings->OnboardingCompletedSteps.Remove(Step);
		Settings->OnboardingSkippedSteps.Remove(Step);

		if (Action == TEXT("complete"))
		{
			Settings->OnboardingCompletedSteps.AddUnique(Step);
		}
		else if (Action == TEXT("skip"))
		{
			Settings->OnboardingSkippedSteps.AddUnique(Step);
		}
		else if (Action == TEXT("reopen"))
		{
			// Removed from both arrays above.
		}
	}

	Settings->SaveConfig();
	TSharedPtr<FJsonObject> Result = OnboardingStateToJson(Settings);
	Result->SetStringField(TEXT("updated_action"), Action);
	if (!Step.IsEmpty())
	{
		Result->SetStringField(TEXT("updated_step"), Step);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetReadinessStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	const bool bServerEnabled = Settings && Settings->bMcpServerEnabled;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_enabled"),
		bServerEnabled,
		bServerEnabled ? TEXT("enabled") : TEXT("disabled"),
		TEXT("enabled"),
		TEXT("Enable Project Settings > Plugins > Monolith > MCP Server > Mcp Server Enabled, then restart the editor."));

	const bool bServerRunning = Server && Server->IsRunning();
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_running"),
		bServerRunning,
		bServerRunning ? TEXT("running") : TEXT("not_running"),
		TEXT("running"),
		TEXT("Check the configured Monolith server port, restart the editor, and review the Output Log for bind failures."));

	const int32 ConfiguredPort = Settings ? Settings->ServerPort : 0;
	const int32 ActualPort = Server ? Server->GetPort() : 0;
	const bool bPortReady = bServerRunning && ConfiguredPort == ActualPort && ConfiguredPort > 0;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_port"),
		bPortReady,
		FString::Printf(TEXT("configured=%d actual=%d"), ConfiguredPort, ActualPort),
		TEXT("configured port equals running server port"),
		TEXT("Change Project Settings > Plugins > Monolith > Server Port if the port is occupied, then restart the editor."));

	const int32 ActionCount = Registry.GetActionCount();
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("action_registry"),
		ActionCount > 0,
		FString::Printf(TEXT("%d actions"), ActionCount),
		TEXT("one or more registered actions"),
		TEXT("If no actions are registered, ensure Monolith modules are enabled and loaded."));

	const bool bIndexEnabled = Settings && Settings->bEnableIndex;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("project_index_setting"),
		bIndexEnabled,
		bIndexEnabled ? TEXT("enabled") : TEXT("disabled"),
		TEXT("enabled"),
		TEXT("Enable Project Settings > Plugins > Monolith > Modules > Enable Index if search/index actions are needed."),
		TEXT("warning"));

	const bool bIndexLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("MonolithIndex"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("project_index_module"),
		bIndexLoaded,
		bIndexLoaded ? TEXT("loaded") : TEXT("not_loaded"),
		TEXT("loaded"),
		TEXT("Enable the MonolithIndex module and restart the editor, then run monolith.reindex if the index is stale."),
		TEXT("warning"));

	const bool bEditorActionsReady = Registry.HasAction(TEXT("editor"), TEXT("get_selected_actors"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("editor_actions"),
		bEditorActionsReady,
		bEditorActionsReady ? TEXT("registered") : TEXT("missing"),
		TEXT("registered"),
		TEXT("Enable Project Settings > Plugins > Monolith > Modules > Enable Editor and restart the editor."),
		TEXT("warning"));

	for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
	{
		const FBoolProperty* Prop = CastField<FBoolProperty>(
			UMonolithSettings::StaticClass()->FindPropertyByName(*Module.SettingsField));
		const bool bSettingEnabled = Settings && Prop && Prop->GetPropertyValue_InContainer(Settings);
		const bool bRegistered = Registry.GetNamespaceActionCount(Module.Namespace) > 0;
		const bool bOk = !bSettingEnabled || bRegistered;
		AddReadinessItem(
			Items, ErrorCount, WarningCount,
			FString::Printf(TEXT("optional_module_%s"), *Module.Namespace),
			bOk,
			!bSettingEnabled ? TEXT("disabled") : (bRegistered ? TEXT("registered") : TEXT("enabled_not_registered")),
			TEXT("disabled or registered"),
			!bSettingEnabled ? TEXT("Optional module is disabled by settings.")
				: FString::Printf(TEXT("%s If installed, restart the editor after enabling the setting."), *Module.InstallHint),
			TEXT("warning"));
	}

	const FString ReleaseGate = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_RELEASE_BUILD"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("release_build_gate"),
		true,
		ReleaseGate == TEXT("1") ? TEXT("MONOLITH_RELEASE_BUILD=1") : TEXT("not_set"),
		TEXT("reported"),
		TEXT("MONOLITH_RELEASE_BUILD=1 disables optional hard dependencies for release validation."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("overall_status"), ErrorCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("ok")));
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetObjectField(TEXT("notification_settings"), NotificationSettingsToJson(Settings));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetReadinessHelp(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterComponent;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("component")) && !Params->TryGetStringField(TEXT("component"), FilterComponent))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'component' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	TArray<TSharedPtr<FJsonValue>> HelpRows;
	auto AddHelp = [&HelpRows, &FilterComponent](const FString& Component, const FString& Summary, const FString& Guidance)
	{
		if (FilterComponent.IsEmpty() || Component.Equals(FilterComponent, ESearchCase::IgnoreCase))
		{
			HelpRows.Add(MakeShared<FJsonValueObject>(ReadinessHelpToJson(Component, Summary, Guidance)));
		}
	};

	AddHelp(TEXT("mcp_server_enabled"), TEXT("MCP server setting is disabled"), TEXT("Enable bMcpServerEnabled in Monolith project settings and restart the editor."));
	AddHelp(TEXT("mcp_server_running"), TEXT("MCP server is not running"), TEXT("Check Output Log for bind errors, confirm the configured port is free, then restart the editor."));
	AddHelp(TEXT("mcp_server_port"), TEXT("Configured port does not match the running server"), TEXT("Update ServerPort in Monolith settings or stop the process occupying the configured port."));
	AddHelp(TEXT("action_registry"), TEXT("No Monolith actions are registered"), TEXT("Confirm MonolithCore loaded and module toggles are enabled."));
	AddHelp(TEXT("project_index_setting"), TEXT("Project indexing is disabled"), TEXT("Enable bEnableIndex if project search, context, or semantic indexing actions are needed."));
	AddHelp(TEXT("project_index_module"), TEXT("Project index module is not loaded"), TEXT("Enable MonolithIndex and restart the editor; use monolith.reindex after it loads."));
	AddHelp(TEXT("editor_actions"), TEXT("Editor actions are unavailable"), TEXT("Enable bEnableEditor and restart the editor."));
	AddHelp(TEXT("release_build_gate"), TEXT("Release build optional dependency gate"), TEXT("Set MONOLITH_RELEASE_BUILD=1 during release validation to force optional hard dependencies off."));

	for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
	{
		AddHelp(
			FString::Printf(TEXT("optional_module_%s"), *Module.Namespace),
			FString::Printf(TEXT("Optional module %s is not registered"), *Module.Namespace),
			Module.InstallHint);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("component"), FilterComponent);
	Result->SetNumberField(TEXT("help_count"), HelpRows.Num());
	Result->SetArrayField(TEXT("help"), HelpRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetNotificationSettings(const TSharedPtr<FJsonObject>& /*Params*/)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("settings"), NotificationSettingsToJson(UMonolithSettings::Get()));
	Result->SetStringField(TEXT("scope"), TEXT("local_monolith_events"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleSetNotificationSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsObj) || !SettingsObj || !SettingsObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("Monolith settings unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> Changed;
	for (const FNotificationBoolSetting& Def : GetNotificationSettings())
	{
		bool NewValue = false;
		if ((*SettingsObj)->TryGetBoolField(Def.Name, NewValue))
		{
			Settings->*(Def.Member) = NewValue;
			Changed.Add(MakeShared<FJsonValueString>(Def.Name));
		}
	}

	Settings->SaveConfig();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("changed_count"), Changed.Num());
	Result->SetArrayField(TEXT("changed"), Changed);
	Result->SetObjectField(TEXT("settings"), NotificationSettingsToJson(Settings));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleTestNotification(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bNotifyEditorToasts)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("shown"), false);
		Result->SetStringField(TEXT("reason"), TEXT("editor_toasts_disabled"));
		return FMonolithActionResult::Success(Result);
	}

	FString Message = TEXT("Monolith notification test");
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("message")) && !Params->TryGetStringField(TEXT("message"), Message))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'message' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	if (Message.IsEmpty())
	{
		Message = TEXT("Monolith notification test");
	}

	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = 3.0f;
	Info.bUseSuccessFailIcons = false;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("shown"), true);
	Result->SetStringField(TEXT("message"), Message);
	Result->SetStringField(TEXT("channel"), TEXT("editor_toast"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetMcpServerStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	constexpr int32 MaxRequestBodyBytes = 16 * 1024 * 1024;

	TArray<TSharedPtr<FJsonValue>> Protocols;
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2024-11-05")));
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2025-03-26")));

	TSharedPtr<FJsonObject> Routes = MakeShared<FJsonObject>();
	Routes->SetBoolField(TEXT("mcp_post"), true);
	Routes->SetBoolField(TEXT("mcp_get_sse_endpoint"), true);
	Routes->SetBoolField(TEXT("legacy_sse"), false);
	Routes->SetBoolField(TEXT("legacy_message"), false);

	TSharedPtr<FJsonObject> Cors = MakeShared<FJsonObject>();
	Cors->SetStringField(TEXT("mode"), TEXT("loopback_origin_allowlist"));
	Cors->SetArrayField(TEXT("allowed_request_headers"), StringArrayToJson({
		TEXT("Content-Type"),
		TEXT("Accept"),
		TEXT("MCP-Session-Id"),
		TEXT("MCP-Protocol-Version")
	}));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("server_setting_enabled"), Settings && Settings->bMcpServerEnabled);
	Result->SetBoolField(TEXT("server_running"), Server && Server->IsRunning());
	Result->SetNumberField(TEXT("configured_port"), Settings ? Settings->ServerPort : 0);
	Result->SetNumberField(TEXT("actual_port"), Server ? Server->GetPort() : 0);
	Result->SetStringField(TEXT("primary_route"), TEXT("/mcp"));
	Result->SetObjectField(TEXT("routes"), Routes);
	Result->SetObjectField(TEXT("cors"), Cors);
	Result->SetArrayField(TEXT("supported_protocol_versions"), Protocols);
	Result->SetNumberField(TEXT("max_request_body_bytes"), MaxRequestBodyBytes);
	const bool bSessionObserverEnabled = Settings && Settings->bEnableMcpSessionMode;
	Result->SetStringField(TEXT("session_tracking"), bSessionObserverEnabled ? TEXT("in_memory_observer") : TEXT("not_persistent"));
	Result->SetStringField(TEXT("session_tracking_note"), bSessionObserverEnabled
		? TEXT("MCP session headers are observed in a bounded process-local table with redacted ids only; progress and request cancellation are not active.")
		: TEXT("Current streamable HTTP handling accepts MCP session/protocol headers but does not persist per-client session rows."));
	TSharedPtr<FJsonObject> Features = MakeShared<FJsonObject>();
	Features->SetObjectField(TEXT("deferred_domain_catalog"), MakeDeferredDomainCatalogStatus(Settings));
	Features->SetObjectField(TEXT("mcp_resources"), MakeMcpResourcesStatus(Settings));
	Features->SetObjectField(TEXT("structured_tool_results"), MakeStructuredToolResultsStatus(Settings));
	Features->SetObjectField(TEXT("mcp_session_mode"), MakeMcpSessionModeStatus(Settings));
	Features->SetObjectField(TEXT("advanced_tool_call_records"), MakeFeatureStatus(
		Settings && Settings->bEnableAdvancedToolCallRecords,
		Settings && Settings->bEnableAdvancedToolCallRecords,
		Settings && Settings->bEnableAdvancedToolCallRecords ? TEXT("active_local_redacted_records") : TEXT("disabled")));
	Result->SetObjectField(TEXT("features"), Features);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleListMcpSessions(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 100.0;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'limit' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	if (!FMath::IsFinite(LimitValue) ||
		LimitValue < static_cast<double>(TNumericLimits<int32>::Min()) ||
		LimitValue > static_cast<double>(TNumericLimits<int32>::Max()))
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'limit' must be a finite number within int32 range"), FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableMcpSessionMode)
	{
		return FMonolithActionResult::Success(
			FMonolithMcpSessionTracker::Get().ListSessionsJson(FMath::FloorToInt(LimitValue)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("unavailable"));
	Result->SetStringField(TEXT("reason"), TEXT("Monolith currently uses stateless streamable HTTP handling and does not persist MCP session rows."));
	Result->SetNumberField(TEXT("requested_limit"), FMath::Clamp(FMath::FloorToInt(LimitValue), 1, 1000));
	Result->SetNumberField(TEXT("session_count"), 0);
	Result->SetArrayField(TEXT("sessions"), TArray<TSharedPtr<FJsonValue>>());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleTerminateMcpSession(const TSharedPtr<FJsonObject>& Params)
{
	FString SessionId;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("session_id")) && !Params->TryGetStringField(TEXT("session_id"), SessionId))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'session_id' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	SessionId.TrimStartAndEndInline();
	if (SessionId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter 'session_id'"), FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableMcpSessionMode)
	{
		return FMonolithActionResult::Success(FMonolithMcpSessionTracker::Get().RemoveSessionJson(SessionId));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("unavailable"));
	Result->SetBoolField(TEXT("terminated"), false);
	Result->SetStringField(TEXT("reason"), TEXT("No persistent MCP session registry exists yet, so there is no session object to terminate."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleSetMcpCompatibilityOptions(const TSharedPtr<FJsonObject>& /*Params*/)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("unavailable"));
	Result->SetBoolField(TEXT("changed"), false);
	Result->SetStringField(TEXT("reason"), TEXT("MCP compatibility options are currently hard-coded safe defaults; settings-backed mutation is future guarded work."));
	Result->SetBoolField(TEXT("legacy_sse_route_enabled"), false);
	Result->SetBoolField(TEXT("legacy_message_route_enabled"), false);
	Result->SetStringField(TEXT("browser_access"), TEXT("loopback_only"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetMcpDiscoveryState(const TSharedPtr<FJsonObject>& /*Params*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FString> Namespaces = Registry.GetNamespaces();

	TArray<TSharedPtr<FJsonValue>> NamespaceRows;
	NamespaceRows.Reserve(Namespaces.Num());
	for (const FString& Namespace : Namespaces)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetNumberField(TEXT("action_count"), Registry.GetNamespaceActionCount(Namespace));
		NamespaceRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("snapshot_mode"), TEXT("live_registry"));
	Result->SetStringField(TEXT("last_refresh"), TEXT("on_demand"));
	Result->SetNumberField(TEXT("namespace_count"), Namespaces.Num());
	Result->SetNumberField(TEXT("action_count"), Registry.GetActionCount());
	Result->SetArrayField(TEXT("namespaces"), NamespaceRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleListDomains(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	bool bIncludeOptional = true;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("include_optional")) && !Params->TryGetBoolField(TEXT("include_optional"), bIncludeOptional))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'include_optional' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TArray<FString> Namespaces = Registry.GetNamespaces();
	Namespaces.Sort();

	TArray<TSharedPtr<FJsonValue>> DomainRows;
	DomainRows.Reserve(Namespaces.Num());
	for (const FString& Namespace : Namespaces)
	{
		const TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetNumberField(TEXT("action_count"), Actions.Num());
		Row->SetBoolField(TEXT("loaded"), IsDomainLoadedForActiveProfile(Namespace));
		Row->SetBoolField(TEXT("profile_allowed"), Actions.Num() > 0);
		Row->SetStringField(TEXT("description"), BuildDomainDescription(Namespace, Actions));

		TSet<FString> CategorySet;
		for (const FMonolithActionInfo& Action : Actions)
		{
			if (!Action.Category.IsEmpty())
			{
				CategorySet.Add(Action.Category);
			}
		}
		TArray<FString> Categories = CategorySet.Array();
		Categories.Sort();
		Row->SetArrayField(TEXT("categories"), StringArrayToJson(Categories));

		DomainRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> OptionalRows;
	if (bIncludeOptional)
	{
		TSet<FString> RegisteredNamespaces;
		for (const FString& Namespace : Namespaces)
		{
			RegisteredNamespaces.Add(Namespace);
		}
		for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
		{
			if (RegisteredNamespaces.Contains(Module.Namespace))
			{
				continue;
			}

			bool bSettingEnabled = false;
			if (Settings)
			{
				const FBoolProperty* Prop = CastField<FBoolProperty>(
					UMonolithSettings::StaticClass()->FindPropertyByName(*Module.SettingsField));
				bSettingEnabled = Prop && Prop->GetPropertyValue_InContainer(Settings);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("namespace"), Module.Namespace);
			Row->SetStringField(TEXT("tool"), Module.ToolName);
			Row->SetNumberField(TEXT("action_count"), 0);
			Row->SetBoolField(TEXT("loaded"), false);
			Row->SetBoolField(TEXT("profile_allowed"), false);
			Row->SetStringField(TEXT("status"), bSettingEnabled ? TEXT("not_installed") : TEXT("disabled"));
			Row->SetStringField(TEXT("description"), Module.InstallHint);
			OptionalRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("deferred_enabled"), true);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetStringField(TEXT("tool_exposure_mode"), IsDomainToolExposureEnabled(Settings) ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Result->SetNumberField(TEXT("domain_count"), DomainRows.Num());
	Result->SetArrayField(TEXT("domains"), DomainRows);
	Result->SetArrayField(TEXT("loaded_domains"), StringArrayToJson(GetLoadedDomainsSnapshotForActiveProfile()));
	if (OptionalRows.Num() > 0)
	{
		Result->SetArrayField(TEXT("optional_domains"), OptionalRows);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleDescribeDomain(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	FString Namespace;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("namespace")) && !Params->TryGetStringField(TEXT("namespace"), Namespace))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'namespace' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Namespace = NormalizeDomainNamespace(Namespace);
	if (Namespace.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("namespace is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
	if (Actions.Num() == 0)
	{
		if (Registry.HasNamespace(Namespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Domain '%s' has no actions allowed by the active Monolith tool profile '%s'."),
					*Namespace,
					*FMonolithToolProfileManager::Get().GetActiveProfileId()),
				FMonolithJsonUtils::ErrInvalidRequest);
		}
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown domain namespace: %s"), *Namespace),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	Actions.Sort([](const FMonolithActionInfo& Left, const FMonolithActionInfo& Right)
	{
		return Left.Action < Right.Action;
	});

	TArray<TSharedPtr<FJsonValue>> ActionRows;
	ActionRows.Reserve(Actions.Num());
	for (const FMonolithActionInfo& Action : Actions)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Action.Action);
		Row->SetStringField(TEXT("action"), Action.Action);
		Row->SetStringField(TEXT("description"), Action.Description);
		if (!Action.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("category"), Action.Category);
		}
		if (Action.ParamSchema.IsValid())
		{
			Row->SetObjectField(TEXT("inputSchema"), Action.ParamSchema);
			Row->SetObjectField(TEXT("params"), Action.ParamSchema);
		}
		ActionRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("loaded"), IsDomainLoadedForActiveProfile(Namespace));
	Result->SetBoolField(TEXT("profile_allowed"), true);
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetNumberField(TEXT("action_count"), Actions.Num());
	Result->SetStringField(TEXT("description"), BuildDomainDescription(Namespace, Actions));
	Result->SetArrayField(TEXT("actions"), ActionRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleLoadDomain(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	FString Namespace;
	if (Params.IsValid())
	{
		if (Params->HasField(TEXT("namespace")) && !Params->TryGetStringField(TEXT("namespace"), Namespace))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'namespace' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Namespace = NormalizeDomainNamespace(Namespace);
	if (Namespace.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("namespace is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
	if (Actions.Num() == 0)
	{
		if (Registry.HasNamespace(Namespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Cannot load domain '%s' because the active Monolith tool profile '%s' allows no actions in that namespace."),
					*Namespace,
					*FMonolithToolProfileManager::Get().GetActiveProfileId()),
				FMonolithJsonUtils::ErrInvalidRequest);
		}
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown domain namespace: %s"), *Namespace),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bNewlyLoaded = MarkDomainLoadedForActiveProfile(Namespace);

	TSharedPtr<FJsonObject> RecommendedDispatch = MakeShared<FJsonObject>();
	RecommendedDispatch->SetStringField(TEXT("namespace"), Namespace);
	RecommendedDispatch->SetStringField(TEXT("current_mcp_tool"), FString::Printf(TEXT("%s_query"), *Namespace));
	RecommendedDispatch->SetStringField(TEXT("offline_cli"), TEXT("monolith_query.exe"));
	RecommendedDispatch->SetBoolField(TEXT("action_argument_required"), true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("loaded"), true);
	Result->SetBoolField(TEXT("newly_loaded"), bNewlyLoaded);
	Result->SetBoolField(TEXT("already_loaded"), !bNewlyLoaded);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("execution_surface_changed"), false);
	Result->SetBoolField(TEXT("visible_tools_changed"), false);
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetStringField(TEXT("tool_exposure_mode"), IsDomainToolExposureEnabled(Settings) ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Result->SetNumberField(TEXT("action_count"), Actions.Num());
	Result->SetObjectField(TEXT("recommended_dispatch"), RecommendedDispatch);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetLoadedDomains(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	const TArray<FString> LoadedDomains = GetLoadedDomainsSnapshotForActiveProfile();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("deferred_enabled"), true);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetNumberField(TEXT("loaded_domain_count"), LoadedDomains.Num());
	Result->SetArrayField(TEXT("loaded_domains"), StringArrayToJson(LoadedDomains));
	return FMonolithActionResult::Success(Result);
}
