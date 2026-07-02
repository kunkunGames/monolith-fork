#include "MonolithConfigActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "Interfaces/IPluginManager.h"

#if WITH_EDITOR
// `set_developer_setting` (dev-gated write) deps.
#include "Engine/DeveloperSettings.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "Misc/StringOutputDevice.h"
#include "MonolithJsonUtils.h"
#endif // WITH_EDITOR

// ============================================================================
// Registration
// ============================================================================

void FMonolithConfigActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("config"), TEXT("resolve_setting"),
		TEXT("Get effective value of a config key across the full INI hierarchy"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::ResolveSetting),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config category (e.g. Engine, Game, Input)"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Config section (e.g. /Script/Engine.RendererSettings)"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Config key name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("explain_setting"),
		TEXT("Show where a config value comes from across Base->Default->User layers"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::ExplainSetting),
		FParamSchemaBuilder()
			.Optional(TEXT("file"), TEXT("string"), TEXT("Config category (e.g. Engine, Game)"))
			.Optional(TEXT("section"), TEXT("string"), TEXT("Config section"))
			.Optional(TEXT("key"), TEXT("string"), TEXT("Config key name"))
			.Optional(TEXT("setting"), TEXT("string"), TEXT("Convenience: search for this key across common categories"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("diff_from_default"),
		TEXT("Show project config overrides vs engine defaults for a category"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::DiffFromDefault),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config category to diff (e.g. Engine, Game)"))
			.Optional(TEXT("section"), TEXT("string"), TEXT("Filter to a specific section"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("search_config"),
		TEXT("Full-text search across all config files"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::SearchConfig),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search text"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a config category"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_section"),
		TEXT("Read an entire config section from a specific file"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetSection),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config file name or category"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Section name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_config_files"),
		TEXT("List all config files with their hierarchy level"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetConfigFiles),
		FParamSchemaBuilder()
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a specific category"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("list_plugins"),
		TEXT("List discovered plugins with enabled state and descriptor metadata. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::ListPlugins),
		FParamSchemaBuilder()
			.Optional(TEXT("name_contains"), TEXT("string"), TEXT("Case-insensitive plugin name substring filter"))
			.Optional(TEXT("enabled_only"), TEXT("bool"), TEXT("Only return enabled plugins"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max plugins to return"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_plugin"),
		TEXT("Get descriptor metadata for one discovered plugin. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetPlugin),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Plugin name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_cvar"),
		TEXT("Get one console variable value and flags. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetCVar),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Console variable name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("find_cvars"),
		TEXT("Find console variables by prefix or substring. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::FindCVars),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("Prefix or substring to search for"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("prefix (default) or contains"), TEXT("prefix"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max CVars to return"), TEXT("100"))
			.Build());

#if WITH_EDITOR
	// DEV-ONLY (write): mutate a UDeveloperSettings CDO at runtime. Never registers
	// in shipping/runtime builds — wraps both registration AND handler. Solves the
	// "INI edit + editor restart fights config hierarchy" loop documented in
	// Docs/plans/2026-05-29-ri-ergonomics-improvements-handover.md (item #7).
	Registry.RegisterAction(TEXT("config"), TEXT("set_developer_setting"),
		TEXT("DEV-ONLY (write): set a property on a UDeveloperSettings CDO at runtime. "
			 "Resolves a settings class by short-name (e.g. 'MonolithReflectionIntelSettings') "
			 "or full path ('/Script/Module.Class'), parses `value` via UProperty::ImportText_Direct, "
			 "and optionally persists back to the INI via SaveConfig(). #if WITH_EDITOR-gated."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::SetDeveloperSetting),
		FParamSchemaBuilder()
			.Required(TEXT("class"), TEXT("string"),
				TEXT("Settings class short-name (e.g. 'MonolithReflectionIntelSettings') or full path ('/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings')."))
			.Required(TEXT("property"), TEXT("string"),
				TEXT("Property name on the CDO (e.g. 'bIndexMarketplacePluginReflection')."))
			.Required(TEXT("value"), TEXT("string"),
				TEXT("Value as text — parsed via UProperty::ImportText_Direct. Examples: 'true', '42', '0.75', '(X=1,Y=2)'."))
			.Optional(TEXT("save_config"), TEXT("boolean"),
				TEXT("Also write back to the persistent INI via UObject::SaveConfig()."),
				TEXT("false"))
			.Build());
#endif // WITH_EDITOR

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("resolve_setting"),
		{ TEXT("effective ini value"), TEXT("merged config value"), TEXT("what is this setting set to"), TEXT("read project setting"), TEXT("DefaultEngine.ini value"), TEXT("resolve config key") },
		{ TEXT("get_config"), TEXT("get_setting"), TEXT("read_ini"), TEXT("get_ini_value") },
		{ TEXT("what is r.ScreenPercentage set to in Engine config"), TEXT("get the effective value of bUseFixedFrameRate in Game"), TEXT("read the RendererSettings DefaultFeatureAntiAliasing value") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("get_cvar"),
		{ TEXT("console variable value"), TEXT("r. variable"), TEXT("current cvar setting"), TEXT("cvar flags"), TEXT("read only cheat cvar"), TEXT("console var lookup") },
		{ TEXT("get_console_variable"), TEXT("read_cvar"), TEXT("cvar_value"), TEXT("show_cvar") },
		{ TEXT("what is the value of r.ScreenPercentage"), TEXT("get the cvar t.MaxFPS"), TEXT("is sg.ShadowQuality read-only") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("find_cvars"),
		{ TEXT("list console variables"), TEXT("search cvars by prefix"), TEXT("all r. variables"), TEXT("find scalability cvars"), TEXT("console variable autocomplete"), TEXT("matching cvars") },
		{ TEXT("search_cvars"), TEXT("list_cvars"), TEXT("cvar_search"), TEXT("grep_cvars") },
		{ TEXT("list all cvars starting with r.Shadow"), TEXT("find console variables containing Nanite"), TEXT("show every sg. scalability cvar") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("get_section"),
		{ TEXT("read ini section"), TEXT("all keys in a section"), TEXT("dump config block"), TEXT("settings under a header"), TEXT("Script section entries"), TEXT("bracketed ini header") },
		{ TEXT("read_section"), TEXT("dump_section"), TEXT("get_ini_section"), TEXT("section_entries") },
		{ TEXT("read the /Script/Engine.RendererSettings section in DefaultEngine"), TEXT("show all keys under /Script/EngineSettings.GameMapsSettings"), TEXT("dump the SystemSettings section") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("diff_from_default"),
		{ TEXT("overrides vs defaults"), TEXT("what did the project change"), TEXT("non-default settings"), TEXT("customized config"), TEXT("compare to engine base"), TEXT("modified added removed keys") },
		{ TEXT("config_diff"), TEXT("show_overrides"), TEXT("diff_config"), TEXT("compare_config") },
		{ TEXT("what Engine config does this project override"), TEXT("show settings that differ from engine defaults"), TEXT("diff the Game config against base") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("config"), TEXT("set_developer_setting"),
		{ TEXT("change a setting at runtime"), TEXT("edit project settings"), TEXT("set developer settings property"), TEXT("mutate settings CDO"), TEXT("write config and save"), TEXT("toggle a settings flag") },
		{ TEXT("set_setting"), TEXT("set_config"), TEXT("update_setting"), TEXT("set_project_setting") },
		{ TEXT("set bIndexMarketplacePluginReflection to true on MonolithReflectionIntelSettings"), TEXT("change a project setting and save it to the ini"), TEXT("toggle a developer settings boolean property") });
}

// ============================================================================
// Helpers
// ============================================================================

FString FMonolithConfigActions::ResolveConfigFilePath(const FString& ShortName)
{
	if (ShortName.Contains(TEXT("..")))
	{
		return FString();
	}

	// Handle known shortnames
	if (ShortName.StartsWith(TEXT("Base")))
	{
		// Engine base configs: e.g. BaseEngine.ini
		return FPaths::Combine(FPaths::EngineConfigDir(), ShortName + TEXT(".ini"));
	}
	else if (ShortName.StartsWith(TEXT("Default")))
	{
		// Project default configs: e.g. DefaultEngine.ini
		return FPaths::Combine(FPaths::ProjectConfigDir(), ShortName + TEXT(".ini"));
	}
	else if (ShortName.Contains(TEXT("/")) || ShortName.Contains(TEXT("\\")))
	{
		// Already a path
		return ShortName;
	}
	else
	{
		// Try project config dir first
		FString ProjectPath = FPaths::Combine(FPaths::ProjectConfigDir(), ShortName + TEXT(".ini"));
		if (FPaths::FileExists(ProjectPath))
		{
			return ProjectPath;
		}
		// Fall back to engine config dir
		return FPaths::Combine(FPaths::EngineConfigDir(), ShortName + TEXT(".ini"));
	}
}

TArray<TPair<FString, FString>> FMonolithConfigActions::GetConfigHierarchy(const FString& Category)
{
	TArray<TPair<FString, FString>> Hierarchy;

	if (Category.Contains(TEXT("..")))
	{
		return Hierarchy;
	}

	// Engine base
	FString BaseFile = FPaths::Combine(FPaths::EngineConfigDir(), FString::Printf(TEXT("Base%s.ini"), *Category));
	if (FPaths::FileExists(BaseFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("Engine Base"), BaseFile));
	}

	// Project default
	FString DefaultFile = FPaths::Combine(FPaths::ProjectConfigDir(), FString::Printf(TEXT("Default%s.ini"), *Category));
	if (FPaths::FileExists(DefaultFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("Project Default"), DefaultFile));
	}

	// User saved (platform-specific)
	FString SavedFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), FPlatformProperties::IniPlatformName(),
		FString::Printf(TEXT("%s.ini"), *Category));
	if (FPaths::FileExists(SavedFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("User Saved"), SavedFile));
	}

	return Hierarchy;
}

namespace
{
	TSharedPtr<FJsonObject> PluginToJson(const TSharedRef<IPlugin>& Plugin)
	{
		const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Plugin->GetName());
		Obj->SetStringField(TEXT("friendly_name"), Descriptor.FriendlyName);
		Obj->SetStringField(TEXT("description"), Descriptor.Description);
		Obj->SetStringField(TEXT("category"), Descriptor.Category);
		Obj->SetStringField(TEXT("version_name"), Descriptor.VersionName);
		Obj->SetNumberField(TEXT("version"), Descriptor.Version);
		Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Obj->SetBoolField(TEXT("enabled_by_default"), Plugin->IsEnabledByDefault(true));
		Obj->SetBoolField(TEXT("can_contain_content"), Descriptor.bCanContainContent);
		Obj->SetBoolField(TEXT("installed"), Descriptor.bInstalled);
		Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
		return Obj;
	}

	TSharedPtr<FJsonObject> CVarToJson(const FString& Name, IConsoleVariable* Variable)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetBoolField(TEXT("found"), Variable != nullptr);
		if (!Variable)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("value"), Variable->GetString());
		Obj->SetStringField(TEXT("help"), Variable->GetHelp());
		Obj->SetNumberField(TEXT("flags"), static_cast<double>(static_cast<uint32>(Variable->GetFlags())));
		Obj->SetBoolField(TEXT("read_only"), Variable->TestFlags(ECVF_ReadOnly));
		Obj->SetBoolField(TEXT("cheat"), Variable->TestFlags(ECVF_Cheat));
		Obj->SetStringField(TEXT("set_by"), GetConsoleVariableSetByName(Variable->GetFlags()));
		return Obj;
	}
}

// ============================================================================
// Action: resolve_setting
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/...", "key": "..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::ResolveSetting(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("resolve_setting: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Category;
	const TSharedPtr<FJsonValue> FileField = Params->TryGetField(TEXT("file"));
	if (!FileField.IsValid() || FileField->IsNull() || !FileField->TryGetString(Category) || Category.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'file' parameter"));
	}
	if (Category.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'file' parameter. Cannot contain path traversal characters."));
	}

	FString Section;
	const TSharedPtr<FJsonValue> SectionField = Params->TryGetField(TEXT("section"));
	if (!SectionField.IsValid() || SectionField->IsNull() || !SectionField->TryGetString(Section) || Section.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'section' parameter"));
	}

	FString Key;
	const TSharedPtr<FJsonValue> KeyField = Params->TryGetField(TEXT("key"));
	if (!KeyField.IsValid() || KeyField->IsNull() || !KeyField->TryGetString(Key) || Key.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'key' parameter"));
	}

	// Use GConfig to get the effective (fully-resolved) value
	FString Value;
	bool bFound = GConfig->GetString(*Section, *Key, Value, GConfig->GetConfigFilename(*Category));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetStringField(TEXT("key"), Key);
	ResultJson->SetBoolField(TEXT("found"), bFound);

	if (bFound)
	{
		ResultJson->SetStringField(TEXT("value"), Value);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: explain_setting
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/...", "key": "..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::ExplainSetting(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("explain_setting: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Category, Section, Key;
	const TSharedPtr<FJsonValue> FileField = Params->TryGetField(TEXT("file"));
	if (FileField.IsValid() && !FileField->IsNull() && !FileField->TryGetString(Category))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: file must be a string"));
	}
	if (Category.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'file' parameter. Cannot contain path traversal characters."));
	}
	const TSharedPtr<FJsonValue> SectionField = Params->TryGetField(TEXT("section"));
	if (SectionField.IsValid() && !SectionField->IsNull() && !SectionField->TryGetString(Section))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: section must be a string"));
	}
	const TSharedPtr<FJsonValue> KeyField = Params->TryGetField(TEXT("key"));
	if (KeyField.IsValid() && !KeyField->IsNull() && !KeyField->TryGetString(Key))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: key must be a string"));
	}

	// Convenience: if 'setting' param provided instead of file/section/key, search for it
	if (Category.IsEmpty() && Section.IsEmpty() && Key.IsEmpty())
	{
		FString Setting;
		const TSharedPtr<FJsonValue> SettingField = Params->TryGetField(TEXT("setting"));
		if (SettingField.IsValid() && !SettingField->IsNull() && !SettingField->TryGetString(Setting))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: setting must be a string"));
		}
		if (!Setting.IsEmpty())
		{
			Key = Setting;
			// Search common config categories for this key
			TArray<FString> SearchCategories = { TEXT("Engine"), TEXT("Game"), TEXT("Input"), TEXT("Editor") };
			for (const FString& Cat : SearchCategories)
			{
				FString ConfigFile = GConfig->GetConfigFilename(*Cat);
				TArray<FString> SectionNames;
				GConfig->GetSectionNames(ConfigFile, SectionNames);
				for (const FString& Sec : SectionNames)
				{
					FString Value;
					if (GConfig->GetString(*Sec, *Setting, Value, ConfigFile))
					{
						Category = Cat;
						Section = Sec;
						break;
					}
				}
				if (!Category.IsEmpty()) break;
			}
		}
	}

	TArray<TPair<FString, FString>> Hierarchy = GetConfigHierarchy(Category);

	TArray<TSharedPtr<FJsonValue>> LayersArray;
	FString EffectiveValue;
	FString EffectiveSource;

	// Parse each layer file as text to find the key
	for (const auto& Layer : Hierarchy)
	{
		FString FileContents;
		if (!FFileHelper::LoadFileToString(FileContents, *Layer.Value))
		{
			continue;
		}

		TArray<FString> Lines;
		FileContents.ParseIntoArrayLines(Lines);

		bool bInSection = false;
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();

			if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
			{
				int32 EndBracket;
				if (Trimmed.FindChar(']', EndBracket))
				{
					FString SectionName = Trimmed.Mid(1, EndBracket - 1);
					bInSection = (SectionName == Section);
				}
				continue;
			}

			if (bInSection && !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT(";")))
			{
				FString LineKey, LineValue;
				if (Trimmed.Split(TEXT("="), &LineKey, &LineValue))
				{
					// Strip +/- prefixes for array operations
					FString CleanKey = LineKey.TrimStartAndEnd();
					if (CleanKey.StartsWith(TEXT("+")) || CleanKey.StartsWith(TEXT("-")) || CleanKey.StartsWith(TEXT(".")))
					{
						CleanKey = CleanKey.Mid(1);
					}

					if (CleanKey == Key)
					{
						FString Val = LineValue.TrimStartAndEnd();

						auto LayerJson = MakeShared<FJsonObject>();
						LayerJson->SetStringField(TEXT("layer"), Layer.Key);
						LayerJson->SetStringField(TEXT("file"), Layer.Value);
						LayerJson->SetStringField(TEXT("value"), Val);
						LayerJson->SetStringField(TEXT("raw_line"), Trimmed);
						LayersArray.Add(MakeShared<FJsonValueObject>(LayerJson));

						EffectiveValue = Val;
						EffectiveSource = Layer.Key;
					}
				}
			}
		}
	}

	// Also get the final resolved value from GConfig
	FString ResolvedValue;
	bool bFound = GConfig->GetString(*Section, *Key, ResolvedValue, GConfig->GetConfigFilename(*Category));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetStringField(TEXT("key"), Key);
	ResultJson->SetArrayField(TEXT("layers"), LayersArray);
	ResultJson->SetBoolField(TEXT("found"), bFound);

	if (bFound)
	{
		ResultJson->SetStringField(TEXT("effective_value"), ResolvedValue);
		ResultJson->SetStringField(TEXT("effective_source"), EffectiveSource);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: diff_from_default
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/..." (optional) }
// ============================================================================

/** Helper: collect entries from GConfig's public GetSection API (returns "Key=Value" pairs) */
static TMap<FString, TArray<FString>> CollectEntriesFromGConfig(const FString& SectionName, const FString& ConfigFilename)
{
	TMap<FString, TArray<FString>> Result;
	TArray<FString> Pairs;
	if (GConfig->GetSection(*SectionName, Pairs, ConfigFilename))
	{
		for (const FString& Pair : Pairs)
		{
			FString Key, Value;
			if (Pair.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				Result.FindOrAdd(Key).Add(Value);
			}
		}
	}
	return Result;
}

/** Helper: parse all sections from INI file text into a nested map */
static TMap<FString, TMap<FString, TArray<FString>>> ParseIniTextSections(const FString& IniText)
{
	TMap<FString, TMap<FString, TArray<FString>>> AllSections;
	TArray<FString> Lines;
	IniText.ParseIntoArrayLines(Lines);

	FString CurrentSection;
	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT(";")))
		{
			continue;
		}

		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
		{
			int32 EndBracket;
			Trimmed.FindChar(']', EndBracket);
			CurrentSection = Trimmed.Mid(1, EndBracket - 1);
			continue;
		}

		if (!CurrentSection.IsEmpty())
		{
			FString Key, Value;
			if (Trimmed.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				// Strip INI action prefixes (+, -, ., !)
				if (Key.Len() > 0 && (Key[0] == '+' || Key[0] == '-' || Key[0] == '.' || Key[0] == '!'))
				{
					Key.RightChopInline(1);
				}
				Value.TrimStartAndEndInline();
				AllSections.FindOrAdd(CurrentSection).FindOrAdd(Key).Add(Value);
			}
		}
	}
	return AllSections;
}

/** Helper: emit a single diff entry as JSON, handling scalar vs array values */
static TSharedPtr<FJsonObject> MakeDiffEntry(
	const FString& SectionName,
	const FString& Key,
	const FString& ChangeType,
	const TArray<FString>& ResolvedValues,
	const TArray<FString>* BaseValues)
{
	auto DiffJson = MakeShared<FJsonObject>();
	DiffJson->SetStringField(TEXT("section"), SectionName);
	DiffJson->SetStringField(TEXT("key"), Key);
	DiffJson->SetStringField(TEXT("change_type"), ChangeType);

	if (ResolvedValues.Num() == 1)
	{
		DiffJson->SetStringField(TEXT("project_value"), ResolvedValues[0]);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& V : ResolvedValues)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(V));
		}
		DiffJson->SetArrayField(TEXT("project_values"), JsonValues);
	}

	if (BaseValues)
	{
		if (BaseValues->Num() == 1)
		{
			DiffJson->SetStringField(TEXT("engine_value"), (*BaseValues)[0]);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& V : *BaseValues)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(V));
			}
			DiffJson->SetArrayField(TEXT("engine_values"), JsonValues);
		}
	}

	return DiffJson;
}

FMonolithActionResult FMonolithConfigActions::DiffFromDefault(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("diff_from_default: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Category;
	const TSharedPtr<FJsonValue> FileField = Params->TryGetField(TEXT("file"));
	if (!FileField.IsValid() || FileField->IsNull() || !FileField->TryGetString(Category) || Category.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'file' parameter"));
	}
	if (Category.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'file' parameter. Category cannot contain path traversal characters."));
	}

	FString FilterSection;
	const TSharedPtr<FJsonValue> FilterSectionField = Params->TryGetField(TEXT("section"));
	if (FilterSectionField.IsValid() && !FilterSectionField->IsNull() && !FilterSectionField->TryGetString(FilterSection))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: section must be a string"));
	}

	// Strip 'Default' or 'Base' prefix if user passed it (e.g. "DefaultEngine" -> "Engine")
	if (Category.StartsWith(TEXT("Default")))
	{
		Category = Category.Mid(7);
	}
	else if (Category.StartsWith(TEXT("Base")))
	{
		Category = Category.Mid(4);
	}

	// Get the fully-resolved config from GConfig (all layers merged: Base + Default + Platform + Saved)
	FString ConfigFilename = GConfig->GetConfigFilename(*Category);
	if (ConfigFilename.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No config found for category '%s'"), *Category));
	}

	// Load engine base config as text for comparison (avoids private FConfigFile API)
	FString BaseConfigPath = FPaths::EngineConfigDir() / (TEXT("Base") + Category + TEXT(".ini"));
	FString BaseConfigText;
	FFileHelper::LoadFileToString(BaseConfigText, *BaseConfigPath);
	auto BaseData = ParseIniTextSections(BaseConfigText);

	// Iterate all sections in the resolved config and diff against engine base
	TArray<FString> SectionNames;
	GConfig->GetSectionNames(ConfigFilename, SectionNames);

	TArray<TSharedPtr<FJsonValue>> DiffsArray;

	for (const FString& SectionName : SectionNames)
	{
		if (!FilterSection.IsEmpty() && SectionName != FilterSection)
		{
			continue;
		}

		// Get effective (resolved) entries from GConfig — includes all merged layers
		auto ResolvedEntries = CollectEntriesFromGConfig(SectionName, ConfigFilename);

		// Get engine base entries from the parsed base config text
		TMap<FString, TArray<FString>> BaseEntries;
		if (const auto* BaseSectionPtr = BaseData.Find(SectionName))
		{
			BaseEntries = *BaseSectionPtr;
		}

		// Find keys that were added or modified by the project
		for (const auto& Entry : ResolvedEntries)
		{
			const FString& Key = Entry.Key;
			const TArray<FString>& ResolvedValues = Entry.Value;
			const TArray<FString>* BaseValues = BaseEntries.Find(Key);

			if (!BaseValues)
			{
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, Key, TEXT("added"), ResolvedValues, nullptr)));
			}
			else if (*BaseValues != ResolvedValues)
			{
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, Key, TEXT("modified"), ResolvedValues, BaseValues)));
			}
		}

		// Find keys that were removed by the project (present in base but not in resolved)
		for (const auto& BaseEntry : BaseEntries)
		{
			if (!ResolvedEntries.Contains(BaseEntry.Key))
			{
				TArray<FString> EmptyValues;
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, BaseEntry.Key, TEXT("removed"), EmptyValues, &BaseEntry.Value)));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	if (!FilterSection.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("filter_section"), FilterSection);
	}
	ResultJson->SetNumberField(TEXT("diff_count"), DiffsArray.Num());
	ResultJson->SetArrayField(TEXT("diffs"), DiffsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: search_config
// Params: { "query": "...", "file": "Engine" (optional) }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::SearchConfig(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("search_config: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Query;
	const TSharedPtr<FJsonValue> QueryField = Params->TryGetField(TEXT("query"));
	if (!QueryField.IsValid() || QueryField->IsNull() || !QueryField->TryGetString(Query) || Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'query' parameter"));
	}

	FString FilterCategory;
	const TSharedPtr<FJsonValue> FilterCategoryField = Params->TryGetField(TEXT("category"));
	if (FilterCategoryField.IsValid() && !FilterCategoryField->IsNull() && !FilterCategoryField->TryGetString(FilterCategory))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: category must be a string"));
	}
	if (FilterCategory.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'category' parameter. Cannot contain path traversal characters."));
	}

	// Gather config directories to search
	TArray<FString> ConfigDirs;
	ConfigDirs.Add(FPaths::EngineConfigDir());
	ConfigDirs.Add(FPaths::ProjectConfigDir());

	FString SavedConfigDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), FPlatformProperties::IniPlatformName());
	if (FPaths::DirectoryExists(SavedConfigDir))
	{
		ConfigDirs.Add(SavedConfigDir);
	}

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	int32 MaxResults = 100;

	for (const FString& ConfigDir : ConfigDirs)
	{
		TArray<FString> IniFiles;
		IFileManager::Get().FindFiles(IniFiles, *FPaths::Combine(ConfigDir, TEXT("*.ini")), true, false);

		for (const FString& IniFile : IniFiles)
		{
			// If filtering by category, check filename
			if (!FilterCategory.IsEmpty())
			{
				if (!IniFile.Contains(FilterCategory))
				{
					continue;
				}
			}

			FString FullPath = FPaths::Combine(ConfigDir, IniFile);
			FString FileContents;
			if (!FFileHelper::LoadFileToString(FileContents, *FullPath))
			{
				continue;
			}

			// Search line by line
			TArray<FString> Lines;
			FileContents.ParseIntoArrayLines(Lines);

			FString CurrentSection;
			for (int32 LineIdx = 0; LineIdx < Lines.Num() && MatchesArray.Num() < MaxResults; ++LineIdx)
			{
				const FString& Line = Lines[LineIdx];

				// Track sections
				if (Line.StartsWith(TEXT("[")) && Line.Contains(TEXT("]")))
				{
					int32 EndBracket;
					if (Line.FindChar(']', EndBracket))
					{
						CurrentSection = Line.Mid(1, EndBracket - 1);
					}
				}

				if (Line.Contains(Query))
				{
					auto MatchJson = MakeShared<FJsonObject>();
					MatchJson->SetStringField(TEXT("file"), IniFile);
					MatchJson->SetStringField(TEXT("path"), FullPath);
					MatchJson->SetStringField(TEXT("section"), CurrentSection);
					MatchJson->SetNumberField(TEXT("line"), LineIdx + 1);
					MatchJson->SetStringField(TEXT("text"), Line.TrimStartAndEnd());
					MatchesArray.Add(MakeShared<FJsonValueObject>(MatchJson));
				}
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("query"), Query);
	ResultJson->SetNumberField(TEXT("match_count"), MatchesArray.Num());
	ResultJson->SetArrayField(TEXT("matches"), MatchesArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_section
// Params: { "file": "DefaultEngine"|"BaseEngine"|..., "section": "/Script/..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::GetSection(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("get_section: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString FileShortName;
	const TSharedPtr<FJsonValue> FileField = Params->TryGetField(TEXT("file"));
	if (!FileField.IsValid() || FileField->IsNull() || !FileField->TryGetString(FileShortName) || FileShortName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'file' parameter"));
	}
	if (FileShortName.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'file' parameter. Cannot contain path traversal characters."));
	}

	FString Section;
	const TSharedPtr<FJsonValue> SectionField = Params->TryGetField(TEXT("section"));
	if (!SectionField.IsValid() || SectionField->IsNull() || !SectionField->TryGetString(Section) || Section.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'section' parameter"));
	}

	// Support category-style names (e.g., "Engine" -> "DefaultEngine" or "BaseEngine")
	if (!FileShortName.StartsWith(TEXT("Default")) && !FileShortName.StartsWith(TEXT("Base"))
		&& !FileShortName.Contains(TEXT("/")) && !FileShortName.Contains(TEXT("\\")))
	{
		FString DefaultPath = ResolveConfigFilePath(TEXT("Default") + FileShortName);
		FString BasePath = ResolveConfigFilePath(TEXT("Base") + FileShortName);

		if (FPaths::FileExists(DefaultPath))
		{
			FileShortName = TEXT("Default") + FileShortName;
		}
		else if (FPaths::FileExists(BasePath))
		{
			FileShortName = TEXT("Base") + FileShortName;
		}
	}

	FString FilePath = ResolveConfigFilePath(FileShortName);

	if (!FPaths::FileExists(FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Config file not found: '%s' (resolved to '%s')"), *FileShortName, *FilePath));
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read config file: '%s'"), *FilePath));
	}

	// Parse manually to get the raw section content
	TArray<FString> Lines;
	FileContents.ParseIntoArrayLines(Lines);

	bool bInSection = false;
	auto EntriesJson = MakeShared<FJsonObject>();
	int32 EntryCount = 0;

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
		{
			if (bInSection)
			{
				break; // We've passed our section
			}

			int32 EndBracket;
			if (Trimmed.FindChar(']', EndBracket))
			{
				FString SectionName = Trimmed.Mid(1, EndBracket - 1);
				if (SectionName == Section)
				{
					bInSection = true;
				}
			}
			continue;
		}

		if (bInSection && !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT(";")))
		{
			// Parse key=value or +key=value
			FString Key, Value;
			if (Trimmed.Split(TEXT("="), &Key, &Value))
			{
				Key = Key.TrimStartAndEnd();
				Value = Value.TrimStartAndEnd();
				EntriesJson->SetStringField(Key, Value);
				EntryCount++;
			}
		}
	}

	if (!bInSection)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Section '%s' not found in '%s'"), *Section, *FileShortName));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("file"), FileShortName);
	ResultJson->SetStringField(TEXT("file_path"), FilePath);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetNumberField(TEXT("entry_count"), EntryCount);
	ResultJson->SetObjectField(TEXT("entries"), EntriesJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_config_files
// Params: { "category": "Engine" (optional — if omitted, lists all) }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::GetConfigFiles(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("get_config_files: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString FilterCategory;
	const TSharedPtr<FJsonValue> FilterCategoryField = Params->TryGetField(TEXT("category"));
	if (FilterCategoryField.IsValid() && !FilterCategoryField->IsNull() && !FilterCategoryField->TryGetString(FilterCategory))
	{
		return FMonolithActionResult::Error(TEXT("Malformed parameter: category must be a string"));
	}
	if (FilterCategory.Contains(TEXT("..")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid 'category' parameter. Cannot contain path traversal characters."));
	}

	TArray<TSharedPtr<FJsonValue>> FilesArray;

	// Helper to add files from a directory with a label
	auto AddFilesFromDir = [&](const FString& Dir, const FString& HierarchyLevel)
	{
		TArray<FString> IniFiles;
		IFileManager::Get().FindFiles(IniFiles, *FPaths::Combine(Dir, TEXT("*.ini")), true, false);

		for (const FString& IniFile : IniFiles)
		{
			if (!FilterCategory.IsEmpty())
			{
				if (!IniFile.Contains(FilterCategory))
				{
					continue;
				}
			}

			FString FullPath = FPaths::Combine(Dir, IniFile);
			int64 FileSize = IFileManager::Get().FileSize(*FullPath);

			auto FileJson = MakeShared<FJsonObject>();
			FileJson->SetStringField(TEXT("name"), IniFile);
			FileJson->SetStringField(TEXT("path"), FullPath);
			FileJson->SetStringField(TEXT("hierarchy_level"), HierarchyLevel);
			FileJson->SetNumberField(TEXT("size_bytes"), static_cast<double>(FileSize));
			FilesArray.Add(MakeShared<FJsonValueObject>(FileJson));
		}
	};

	AddFilesFromDir(FPaths::EngineConfigDir(), TEXT("Engine Base"));
	AddFilesFromDir(FPaths::ProjectConfigDir(), TEXT("Project Default"));

	FString SavedConfigDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), FPlatformProperties::IniPlatformName());
	if (FPaths::DirectoryExists(SavedConfigDir))
	{
		AddFilesFromDir(SavedConfigDir, TEXT("User Saved"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	if (!FilterCategory.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("filter_category"), FilterCategory);
	}
	ResultJson->SetNumberField(TEXT("file_count"), FilesArray.Num());
	ResultJson->SetArrayField(TEXT("files"), FilesArray);

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithConfigActions::ListPlugins(const TSharedPtr<FJsonObject>& Params)
{
	FString NameContains;
	bool bEnabledOnly = false;
	int32 Limit = 200;
	if (Params.IsValid())
	{
			const TSharedPtr<FJsonValue> NameContainsField = Params->TryGetField(TEXT("name_contains"));
			if (NameContainsField.IsValid() && !NameContainsField->IsNull() && !NameContainsField->TryGetString(NameContains))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: name_contains must be a string"));
		}
			const TSharedPtr<FJsonValue> EnabledOnlyField = Params->TryGetField(TEXT("enabled_only"));
			if (EnabledOnlyField.IsValid() && !EnabledOnlyField->IsNull() && !EnabledOnlyField->TryGetBool(bEnabledOnly))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: enabled_only must be a boolean"));
		}
			const TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
			if (LimitField.IsValid() && !LimitField->IsNull())
		{
			double LimitValue = 0.0;
				if (!LimitField->TryGetNumber(LimitValue))
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'limit' must be a number"), -32602);
			}
			Limit = FMath::Clamp((int32)LimitValue, 1, 1000);
		}
	}

	const FString Needle = NameContains.ToLower();
	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
	Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B)
	{
		return A->GetName() < B->GetName();
	});

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 Matched = 0;
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		if (bEnabledOnly && !Plugin->IsEnabled())
		{
			continue;
		}
		if (!Needle.IsEmpty() && !Plugin->GetName().ToLower().Contains(Needle))
		{
			continue;
		}

		++Matched;
		if (Rows.Num() < Limit)
		{
			Rows.Add(MakeShared<FJsonValueObject>(PluginToJson(Plugin)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("matched_count"), Matched);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetArrayField(TEXT("plugins"), Rows);
	if (Matched > Rows.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), Matched - Rows.Num());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithConfigActions::GetPlugin(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: name"));
	}

	FString Name;
	const TSharedPtr<FJsonValue> NameField = Params->TryGetField(TEXT("name"));
	if (!NameField.IsValid() || NameField->IsNull() || !NameField->TryGetString(Name) || Name.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: name"));
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);
	Result->SetBoolField(TEXT("found"), Plugin.IsValid());
	if (Plugin.IsValid())
	{
		Result->SetObjectField(TEXT("plugin"), PluginToJson(Plugin.ToSharedRef()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithConfigActions::GetCVar(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: name"));
	}

	FString Name;
	const TSharedPtr<FJsonValue> NameField = Params->TryGetField(TEXT("name"));
	if (!NameField.IsValid() || NameField->IsNull() || !NameField->TryGetString(Name) || Name.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: name"));
	}

	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
	return FMonolithActionResult::Success(CVarToJson(Name, Variable));
}

FMonolithActionResult FMonolithConfigActions::FindCVars(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FString Mode = TEXT("prefix");
	int32 Limit = 100;
	if (Params.IsValid())
	{
			const TSharedPtr<FJsonValue> QueryField = Params->TryGetField(TEXT("query"));
			if (QueryField.IsValid() && !QueryField->IsNull() && !QueryField->TryGetString(Query))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: query must be a string"));
		}
			const TSharedPtr<FJsonValue> ModeField = Params->TryGetField(TEXT("mode"));
			if (ModeField.IsValid() && !ModeField->IsNull() && !ModeField->TryGetString(Mode))
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: mode must be a string"));
		}
			const TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
			if (LimitField.IsValid() && !LimitField->IsNull())
		{
			double LimitValue = 0.0;
				if (!LimitField->TryGetNumber(LimitValue))
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'limit' must be a number"), -32602);
			}
			Limit = FMath::Clamp((int32)LimitValue, 1, 1000);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 Matched = 0;
	FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
		[&Rows, &Matched, Limit](const TCHAR* Name, IConsoleObject* Object)
		{
			if (!Object || !Object->AsVariable())
			{
				return;
			}

			++Matched;
			if (Rows.Num() < Limit)
			{
				Rows.Add(MakeShared<FJsonValueObject>(CVarToJson(Name, Object->AsVariable())));
			}
		});

	if (Mode.Equals(TEXT("contains"), ESearchCase::IgnoreCase))
	{
		IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *Query);
	}
	else
	{
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(Visitor, *Query);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), Query);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetNumberField(TEXT("matched_count"), Matched);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetArrayField(TEXT("cvars"), Rows);
	if (Matched > Rows.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), Matched - Rows.Num());
	}
	return FMonolithActionResult::Success(Result);
}

#if WITH_EDITOR
// ============================================================================
// Action: set_developer_setting   (DEV-ONLY, #if WITH_EDITOR gated)
// Params: { "class": "...", "property": "...", "value": "...", "save_config"?: bool }
// ============================================================================
//
// API verification (per .claude/rules/always/ue57-api.md, editor was down — verified
// via Grep on UE 5.7 engine source at C:\Program Files (x86)\UE_5.7\Engine\Source):
//
//   - `FProperty::ImportText_Direct(const TCHAR* Buffer, void* PropertyPtr,
//       UObject* OwnerObject, int32 PortFlags, FOutputDevice* ErrorText = (FOutputDevice*)GWarn) const`
//     — Runtime/CoreUObject/Public/UObject/UnrealType.h:623
//
//   - `FProperty::ExportText_Direct(FString& ValueStr, const void* Data, const void* Delta,
//       UObject* Parent, int32 PortFlags, UObject* ExportRootScope = nullptr) const`
//     — Runtime/CoreUObject/Public/UObject/UnrealType.h:723
//
//   - `EFindFirstObjectOptions::NativeFirst` (enum-class flag, UObject/UObjectGlobals.h:495).
//     Project pattern (28+ call sites) uses
//     `FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst)` for short-name
//     resolution. Full-path resolution uses `FindObject<UClass>(nullptr, *FullPath)` — both
//     forms are tried here for caller convenience.
//
//   - `UDeveloperSettings` (Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:23).
//     Module is `DeveloperSettings` (NOT `Engine`) — added to MonolithConfig.Build.cs.
//
//   - `UObject::SaveConfig(uint64 Flags = CPF_Config, const TCHAR* Filename = nullptr, ...)`
//     — canonical persistence path; respects the class's `config=...` UCLASS meta and writes
//     to the resolved INI (e.g. `Config/MonolithSettings.ini`).
// ============================================================================

FMonolithActionResult FMonolithConfigActions::SetDeveloperSetting(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: missing params object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString ClassName;
	const TSharedPtr<FJsonValue> ClassField = Params->TryGetField(TEXT("class"));
	if (!ClassField.IsValid() || ClassField->IsNull() || !ClassField->TryGetString(ClassName) || ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: 'class' is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString PropertyName;
	const TSharedPtr<FJsonValue> PropertyField = Params->TryGetField(TEXT("property"));
	if (!PropertyField.IsValid() || PropertyField->IsNull() || !PropertyField->TryGetString(PropertyName) || PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: 'property' is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString NewValueText;
	const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	if (!ValueField.IsValid() || ValueField->IsNull() || !ValueField->TryGetString(NewValueText))
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: 'value' is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bSaveConfig = false;
	const TSharedPtr<FJsonValue> SaveConfigField = Params->TryGetField(TEXT("save_config"));
	if (SaveConfigField.IsValid() && !SaveConfigField->IsNull() && !SaveConfigField->TryGetBool(bSaveConfig))
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: 'save_config' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
	}

	// 1) Resolve class. Try full-path first (works for '/Script/Module.Class'),
	//    then short-name lookup biased toward native classes.
	UClass* TargetClass = nullptr;
	if (ClassName.StartsWith(TEXT("/Script/")) || ClassName.Contains(TEXT(".")))
	{
		TargetClass = FindObject<UClass>(nullptr, *ClassName);
	}
	if (TargetClass == nullptr)
	{
		TargetClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}
	// Common 'U'-prefix convenience (matches MonolithBlueprint/MonolithAnimation patterns).
	if (TargetClass == nullptr && !ClassName.StartsWith(TEXT("U")))
	{
		TargetClass = FindFirstObject<UClass>(
			*FString::Printf(TEXT("U%s"), *ClassName), EFindFirstObjectOptions::NativeFirst);
	}

	if (TargetClass == nullptr)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: unknown class '%s' — supply short name "
				 "like 'MonolithReflectionIntelSettings' or full path "
				 "'/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings'"),
			*ClassName));
	}

	// 2) Verify the class is UDeveloperSettings-derived (or at minimum a UObject with a CDO).
	//    UDeveloperSettings is enforced because non-developer-settings classes have no
	//    `config=...` UCLASS meta and SaveConfig() would either no-op or write to a
	//    surprising file. Lifting this check would be a larger design decision.
	if (!TargetClass->IsChildOf(UDeveloperSettings::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: class '%s' is not derived from UDeveloperSettings "
				 "(this action only mutates settings-class CDOs)."),
			*TargetClass->GetName()));
	}

	UObject* CDO = TargetClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
	if (CDO == nullptr)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: class '%s' has no CDO"), *TargetClass->GetName()));
	}

	// 3) Resolve property by name; on miss, surface up to the first 10 property
	//    names on the class as a did-you-mean hint.
	FProperty* TargetProperty = TargetClass->FindPropertyByName(*PropertyName);
	if (TargetProperty == nullptr)
	{
		TArray<FString> KnownNames;
		for (TFieldIterator<FProperty> It(TargetClass); It && KnownNames.Num() < 10; ++It)
		{
			KnownNames.Add(It->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: unknown property '%s' on class '%s' — known properties (first %d): [%s]"),
			*PropertyName, *TargetClass->GetName(), KnownNames.Num(), *FString::Join(KnownNames, TEXT(", "))));
	}

	void* PropertyValuePtr = TargetProperty->ContainerPtrToValuePtr<void>(CDO);

	// 4) Capture old value via ExportText_Direct for the response payload.
	FString OldValueText;
	TargetProperty->ExportText_Direct(OldValueText, PropertyValuePtr, PropertyValuePtr, CDO, PPF_None);

	// 5) Parse + set via ImportText_Direct. Returns nullptr on parse failure.
	//    Suppress the engine's default GWarn output so a malformed `value` doesn't
	//    spam the log — caller gets a clean error from the action result.
	FStringOutputDevice ImportErrors;
	const TCHAR* ImportResult = TargetProperty->ImportText_Direct(
		*NewValueText, PropertyValuePtr, CDO, PPF_None, &ImportErrors);

	if (ImportResult == nullptr)
	{
		// Restore prior value defensively — ImportText_Direct may have partially
		// mutated the destination on some property kinds before returning null.
		TargetProperty->ImportText_Direct(*OldValueText, PropertyValuePtr, CDO, PPF_None, &ImportErrors);
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: failed to parse value '%s' for property '%s' (type=%s)%s"),
			*NewValueText, *PropertyName, *TargetProperty->GetCPPType(),
			ImportErrors.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *ImportErrors)));
	}

	// 6) Capture the post-import value (canonical text form may differ from input —
	//    e.g. "1" → "True" for bool, normalized casing for enums).
	FString NewValueCanonical;
	TargetProperty->ExportText_Direct(NewValueCanonical, PropertyValuePtr, PropertyValuePtr, CDO, PPF_None);

	// 7) Optional persistence. UCLASS(config=Foo, defaultconfig) determines the
	//    target INI; SaveConfig() walks UCLASS meta to pick the right file.
	bool bSaved = false;
	FString SaveError;
	if (bSaveConfig)
	{
		// SaveConfig is non-const + virtual; UDeveloperSettings inherits the UObject impl.
		CDO->SaveConfig();
		bSaved = true;
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("class"), TargetClass->GetName());
	ResultJson->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
	ResultJson->SetStringField(TEXT("property"), PropertyName);
	ResultJson->SetStringField(TEXT("property_type"), TargetProperty->GetCPPType());
	ResultJson->SetStringField(TEXT("old_value"), OldValueText);
	ResultJson->SetStringField(TEXT("new_value"), NewValueCanonical);
	ResultJson->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(ResultJson);
}
#endif // WITH_EDITOR
