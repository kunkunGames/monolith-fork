#include "MonolithGameSettingsActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PlayerMappableInputConfig.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

namespace MonolithGameSettings
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
		{ TEXT("GameSetting"), TEXT("/Script/GameSettings.GameSetting"), TEXT("base setting node") },
		{ TEXT("GameSettingCollection"), TEXT("/Script/GameSettings.GameSettingCollection"), TEXT("tree collection node") },
		{ TEXT("GameSettingCollectionPage"), TEXT("/Script/GameSettings.GameSettingCollectionPage"), TEXT("navigable collection page") },
		{ TEXT("GameSettingValue"), TEXT("/Script/GameSettings.GameSettingValue"), TEXT("value setting base") },
		{ TEXT("GameSettingValueDiscreteDynamic"), TEXT("/Script/GameSettings.GameSettingValueDiscreteDynamic"), TEXT("dynamic discrete value setting") },
		{ TEXT("GameSettingValueScalarDynamic"), TEXT("/Script/GameSettings.GameSettingValueScalarDynamic"), TEXT("dynamic scalar value setting") },
		{ TEXT("GameSettingAction"), TEXT("/Script/GameSettings.GameSettingAction"), TEXT("action setting") },
		{ TEXT("GameSettingRegistry"), TEXT("/Script/GameSettings.GameSettingRegistry"), TEXT("registry root") },
		{ TEXT("GameSettingScreen"), TEXT("/Script/GameSettings.GameSettingScreen"), TEXT("CommonUI settings screen") },
		{ TEXT("GameSettingVisualData"), TEXT("/Script/GameSettings.GameSettingVisualData"), TEXT("entry widget/detail extension data asset") },
		{ TEXT("PlayerMappableInputConfig"), TEXT("/Script/EnhancedInput.PlayerMappableInputConfig"), TEXT("deprecated Enhanced Input mappable config asset used by Lyra settings") },
		{ TEXT("InputMappingContext"), TEXT("/Script/EnhancedInput.InputMappingContext"), TEXT("Enhanced Input mapping context that owns player mappable key rows") },
		{ TEXT("PlayerMappableKeySettings"), TEXT("/Script/EnhancedInput.PlayerMappableKeySettings"), TEXT("per-action or per-mapping save/display metadata") }
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
		FString NormalizedPath = ObjectPathValue;
		NormalizedPath.TrimStartAndEndInline();
		if (NormalizedPath.IsEmpty())
		{
			return nullptr;
		}
		if (NormalizedPath.StartsWith(TEXT("/")) && !NormalizedPath.StartsWith(TEXT("/Script/")) && !NormalizedPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedPath);
			if (!AssetName.IsEmpty())
			{
				NormalizedPath = NormalizedPath + TEXT(".") + AssetName;
			}
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizedPath, nullptr, LOAD_NoWarn);
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
		Obj->SetBoolField(TEXT("blueprint_event"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
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
		Obj->SetBoolField(TEXT("transient"), Property && Property->HasAnyPropertyFlags(CPF_Transient));
		Obj->SetBoolField(TEXT("config"), Property && Property->HasAnyPropertyFlags(CPF_Config));
		if (Property && SourceObject)
		{
			FString Value;
			Property->ExportText_InContainer(0, Value, SourceObject, SourceObject, const_cast<UObject*>(SourceObject), PPF_None);
			Obj->SetStringField(TEXT("value"), Value);
		}
		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			Obj->SetBoolField(TEXT("is_map"), true);
			if (SourceObject)
			{
				FScriptMapHelper Helper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(SourceObject));
				Obj->SetNumberField(TEXT("entry_count"), Helper.Num());
			}
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

	static TArray<TSharedPtr<FJsonValue>> ContractRows()
	{
		const TCHAR* Rows[] =
		{
			TEXT("UGameSettingScreen creates one concrete UGameSettingRegistry and hosts a UGameSettingPanel bound as Settings_Panel."),
			TEXT("UGameSettingRegistry owns TopLevelSettings and recursively registers child settings returned by UGameSetting::GetChildSettings."),
			TEXT("UGameSetting::DevName is a native FName identifier, not a reflected UPROPERTY; authoring flows must set it explicitly and keep it unique."),
			TEXT("UGameSettingCollection::AddSetting builds the tree; UGameSettingValueDynamic variants use getter/setter data sources for live values."),
			TEXT("FGameSettingDataSourceDynamic resolves a cached property path against the local player at runtime; static validation can only verify path shape without a player instance."),
			TEXT("Lyra's keyboard settings screen builds setting rows from UPlayerMappableInputConfig::GetPlayerMappableKeys and rejects duplicate mapping names, missing mapping names, or missing display names.")
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
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
				return false;
			}
			StringValue.TrimStartAndEndInline();
			if (!StringValue.IsEmpty())
			{
				OutValues.Add(StringValue);
			}
		}
		return true;
	}

	static bool AddOptionalStringParamToArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& InOutValues, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		FString Value;
		if (!Params->TryGetStringField(FieldName, Value))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		Value.TrimStartAndEndInline();
		if (!Value.IsEmpty())
		{
			InOutValues.AddUnique(Value);
		}
		return true;
	}

	static bool ContainsWhitespace(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (FChar::IsWhitespace(Value[Index]))
			{
				return true;
			}
		}
		return false;
	}

	static TArray<FString> SplitPathSegments(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("."));
		Path.ReplaceInline(TEXT("/"), TEXT("."));

		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), false);
		for (FString& Segment : Segments)
		{
			Segment.TrimStartAndEndInline();
		}
		return Segments;
	}

	static TSharedPtr<FJsonObject> ValidateDynamicPath(const FString& Label, const FString& Path, bool bRequired, TArray<TSharedPtr<FJsonValue>>& Checks, TArray<TSharedPtr<FJsonValue>>& Issues, bool& bOk)
	{
		TArray<FString> Segments = SplitPathSegments(Path);

		bool bValid = !Path.IsEmpty() && Segments.Num() > 0;
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || ContainsWhitespace(Segment))
			{
				bValid = false;
				break;
			}
		}

		const bool bCheckOk = bValid || !bRequired;
		AddCheck(Checks, bOk, *FString::Printf(TEXT("%s_path_shape"), *Label), bCheckOk, bRequired ? TEXT("error") : TEXT("warning"), Path);
		if (!bCheckOk)
		{
			AddIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), TEXT("data_source_path_invalid"), FString::Printf(TEXT("%s path '%s' must be a non-empty dotted property path with non-empty segments."), *Label, *Path));
		}

		TArray<TSharedPtr<FJsonValue>> JsonSegments;
		for (const FString& Segment : Segments)
		{
			JsonSegments.Add(MakeShared<FJsonValueString>(Segment));
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("label"), Label);
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetBoolField(TEXT("required"), bRequired);
		Obj->SetBoolField(TEXT("shape_valid"), bValid);
		Obj->SetNumberField(TEXT("segment_count"), Segments.Num());
		Obj->SetArrayField(TEXT("segments"), JsonSegments);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> MappingObjectClassRows(const TArray<TObjectPtr<UObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TObjectPtr<UObject>& Object : Objects)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("class_path"), Object ? Object->GetClass()->GetPathName() : FString());
			Row->SetStringField(TEXT("object_path"), Object ? Object->GetPathName() : FString());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> MappingRow(
		const FEnhancedActionKeyMapping& Mapping,
		const UInputMappingContext* Context,
		const FString& Source,
		const FString& ProfileId,
		int32 Index,
		int32 Priority)
	{
		TArray<TObjectPtr<UObject>> TriggerObjects;
		for (UInputTrigger* Trigger : Mapping.Triggers)
		{
			TriggerObjects.Add(Trigger);
		}

		TArray<TObjectPtr<UObject>> ModifierObjects;
		for (UInputModifier* Modifier : Mapping.Modifiers)
		{
			ModifierObjects.Add(Modifier);
		}

		const UInputAction* Action = Mapping.Action.Get();
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("context_path"), ObjectPath(Context));
		Row->SetStringField(TEXT("source"), Source);
		Row->SetStringField(TEXT("profile_id"), ProfileId);
		Row->SetNumberField(TEXT("priority"), Priority);
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("action_path"), ObjectPath(Action));
		Row->SetStringField(TEXT("action_name"), Action ? Action->GetName() : FString());
		Row->SetStringField(TEXT("key"), Mapping.Key.ToString());
		Row->SetStringField(TEXT("key_name"), Mapping.Key.GetFName().ToString());
		Row->SetBoolField(TEXT("key_valid"), Mapping.Key.IsValid());
		Row->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());
		Row->SetStringField(TEXT("mapping_name"), Mapping.GetMappingName().ToString());
		Row->SetStringField(TEXT("display_name"), Mapping.GetDisplayName().ToString());
		Row->SetStringField(TEXT("display_category"), Mapping.GetDisplayCategory().ToString());
		Row->SetBoolField(TEXT("has_player_mappable_key_settings"), Mapping.GetPlayerMappableKeySettings() != nullptr);
		Row->SetBoolField(TEXT("action_has_player_mappable_key_settings"), Action && Action->GetPlayerMappableKeySettings() != nullptr);
		Row->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
		Row->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
		Row->SetArrayField(TEXT("triggers"), MappingObjectClassRows(TriggerObjects));
		Row->SetArrayField(TEXT("modifiers"), MappingObjectClassRows(ModifierObjects));
		return Row;
	}

	struct FPlayerMappableValidationOptions
	{
		bool bRequireConfigName = true;
		bool bRequireConfigDisplayName = true;
		bool bRequireContexts = true;
		bool bRequireMappableKeys = true;
		bool bRequireUniqueMappingNames = true;
		bool bRequireMappingDisplayNames = true;
		bool bRequireValidKeys = true;
		bool bRequireActions = true;
		bool bIncludeMappingProfileOverrides = true;
	};

	static void ValidateMappingRows(
		const TArray<TSharedPtr<FJsonObject>>& MappingRows,
		const FPlayerMappableValidationOptions& Options,
		TArray<TSharedPtr<FJsonValue>>& Checks,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		bool& bOk,
		int32& OutMappableCount,
		int32& OutDuplicateCount)
	{
		OutMappableCount = 0;
		OutDuplicateCount = 0;

		TMap<FString, TArray<TSharedPtr<FJsonObject>>> RowsByScopedName;
		for (const TSharedPtr<FJsonObject>& Row : MappingRows)
		{
			if (!Row.IsValid())
			{
				continue;
			}

			const bool bIsMappable = Row->GetBoolField(TEXT("is_player_mappable"));
			if (!bIsMappable)
			{
				continue;
			}

			++OutMappableCount;

			const FString Source = Row->GetStringField(TEXT("source"));
			const FString ProfileId = Row->GetStringField(TEXT("profile_id"));
			const FString MappingName = Row->GetStringField(TEXT("mapping_name"));
			const FString DisplayName = Row->GetStringField(TEXT("display_name"));
			const FString ActionPath = Row->GetStringField(TEXT("action_path"));
			const FString KeyName = Row->GetStringField(TEXT("key_name"));
			const bool bKeyValid = Row->GetBoolField(TEXT("key_valid"));

			const FString RowDetail = FString::Printf(
				TEXT("%s[%s] %s key=%s action=%s"),
				*Source,
				*ProfileId,
				*MappingName,
				*KeyName,
				*ActionPath);

			if (ActionPath.IsEmpty() && Options.bRequireActions)
			{
				AddIssue(Issues, TEXT("error"), TEXT("mappable_mapping_missing_action"), RowDetail);
				bOk = false;
			}
			if (!bKeyValid && Options.bRequireValidKeys)
			{
				AddIssue(Issues, TEXT("error"), TEXT("mappable_mapping_invalid_key"), RowDetail);
				bOk = false;
			}
			if (MappingName.IsEmpty() || MappingName == TEXT("None"))
			{
				AddIssue(Issues, TEXT("error"), TEXT("mappable_mapping_name_missing"), RowDetail);
				bOk = false;
			}
			if (DisplayName.IsEmpty() && Options.bRequireMappingDisplayNames)
			{
				AddIssue(Issues, TEXT("error"), TEXT("mappable_display_name_missing"), RowDetail);
				bOk = false;
			}
			if (!MappingName.IsEmpty() && MappingName != TEXT("None"))
			{
				const FString Scope = Source == TEXT("profile_override") ? FString::Printf(TEXT("profile:%s"), *ProfileId) : TEXT("default");
				RowsByScopedName.FindOrAdd(Scope + TEXT("|") + MappingName).Add(Row);
			}
		}

		for (const TPair<FString, TArray<TSharedPtr<FJsonObject>>>& Pair : RowsByScopedName)
		{
			if (Pair.Value.Num() <= 1)
			{
				continue;
			}

			OutDuplicateCount += Pair.Value.Num();
			if (Options.bRequireUniqueMappingNames)
			{
				TArray<TSharedPtr<FJsonValue>> Locations;
				for (const TSharedPtr<FJsonObject>& Row : Pair.Value)
				{
					TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
					Location->SetStringField(TEXT("context_path"), Row->GetStringField(TEXT("context_path")));
					Location->SetStringField(TEXT("source"), Row->GetStringField(TEXT("source")));
					Location->SetStringField(TEXT("profile_id"), Row->GetStringField(TEXT("profile_id")));
					Location->SetNumberField(TEXT("index"), Row->GetNumberField(TEXT("index")));
					Locations.Add(MakeShared<FJsonValueObject>(Location));
				}

				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("duplicate_mapping_name"));
				Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Duplicate player-mappable mapping name in scope '%s'."), *Pair.Key));
				Issue->SetArrayField(TEXT("locations"), Locations);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				bOk = false;
			}
		}

		AddCheck(
			Checks,
			bOk,
			TEXT("mappable_keys_present"),
			!Options.bRequireMappableKeys || OutMappableCount > 0,
			Options.bRequireMappableKeys ? TEXT("error") : TEXT("info"),
			FString::Printf(TEXT("mappable_mapping_count=%d"), OutMappableCount));
		if (Options.bRequireMappableKeys && OutMappableCount == 0)
		{
			AddIssue(Issues, TEXT("error"), TEXT("mappable_keys_missing"), TEXT("No player-mappable mappings were found in the inspected config or context."));
		}
	}

	static void AppendContextMappings(
		const UInputMappingContext* Context,
		int32 Priority,
		bool bIncludeMappingProfileOverrides,
		TArray<TSharedPtr<FJsonObject>>& OutRows)
	{
		if (!Context)
		{
			return;
		}

		const TArray<FEnhancedActionKeyMapping>& DefaultMappings = Context->GetMappings();
		for (int32 Index = 0; Index < DefaultMappings.Num(); ++Index)
		{
			OutRows.Add(MappingRow(DefaultMappings[Index], Context, TEXT("default"), FString(), Index, Priority));
		}

		if (!bIncludeMappingProfileOverrides)
		{
			return;
		}

		const TArray<FString> ProfileIds = Context->GetProfilesWithOverridenMappings();
		for (const FString& ProfileId : ProfileIds)
		{
			const TArray<FEnhancedActionKeyMapping>& ProfileMappings = Context->GetMappingsForProfile(ProfileId);
			for (int32 Index = 0; Index < ProfileMappings.Num(); ++Index)
			{
				OutRows.Add(MappingRow(ProfileMappings[Index], Context, TEXT("profile_override"), ProfileId, Index, Priority));
			}
		}
	}

	static TSharedPtr<FJsonObject> ValidatePlayerMappableConfigObject(
		const FString& RequestedPath,
		UPlayerMappableInputConfig* Config,
		const FPlayerMappableValidationOptions& Options,
		TArray<TSharedPtr<FJsonValue>>& AllIssues,
		bool& bGlobalOk)
	{
		bool bOk = true;
		TArray<TSharedPtr<FJsonValue>> Checks;
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonObject>> MappingRows;
		TArray<TSharedPtr<FJsonValue>> ContextRows;

		UClass* ConfigClass = LoadClassPath(TEXT("/Script/EnhancedInput.PlayerMappableInputConfig"));
		AddCheck(Checks, bOk, TEXT("config_loaded"), Config != nullptr, TEXT("error"), RequestedPath);
		AddCheck(Checks, bOk, TEXT("config_type"), Config && ConfigClass && Config->GetClass()->IsChildOf(ConfigClass), TEXT("error"), Config ? Config->GetClass()->GetPathName() : FString());

		if (!Config)
		{
			AddIssue(Issues, TEXT("error"), TEXT("config_not_found_or_wrong_type"), FString::Printf(TEXT("Object '%s' is not a UPlayerMappableInputConfig."), *RequestedPath));
		}
		else
		{
			const FString ConfigName = Config->GetConfigName().ToString();
			const FString DisplayName = Config->GetDisplayName().ToString();
			const TMap<TObjectPtr<UInputMappingContext>, int32>& Contexts = Config->GetMappingContexts();

			AddCheck(Checks, bOk, TEXT("config_name_present"), !Options.bRequireConfigName || (Config->GetConfigName() != NAME_None && !ConfigName.IsEmpty()), Options.bRequireConfigName ? TEXT("error") : TEXT("info"), ConfigName);
			AddCheck(Checks, bOk, TEXT("config_display_name_present"), !Options.bRequireConfigDisplayName || !DisplayName.IsEmpty(), Options.bRequireConfigDisplayName ? TEXT("error") : TEXT("info"), DisplayName);
			AddCheck(Checks, bOk, TEXT("config_not_deprecated"), !Config->IsDeprecated(), TEXT("warning"), Config->IsDeprecated() ? TEXT("deprecated") : TEXT("active"));
			AddCheck(Checks, bOk, TEXT("contexts_present"), !Options.bRequireContexts || Contexts.Num() > 0, Options.bRequireContexts ? TEXT("error") : TEXT("info"), FString::Printf(TEXT("context_count=%d"), Contexts.Num()));

			if (Options.bRequireConfigName && (Config->GetConfigName() == NAME_None || ConfigName.IsEmpty()))
			{
				AddIssue(Issues, TEXT("error"), TEXT("config_name_missing"), FString::Printf(TEXT("PlayerMappableInputConfig '%s' has no ConfigName."), *Config->GetPathName()));
			}
			if (Options.bRequireConfigDisplayName && DisplayName.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("config_display_name_missing"), FString::Printf(TEXT("PlayerMappableInputConfig '%s' has no display name."), *Config->GetPathName()));
			}
			if (Options.bRequireContexts && Contexts.Num() == 0)
			{
				AddIssue(Issues, TEXT("error"), TEXT("config_contexts_missing"), FString::Printf(TEXT("PlayerMappableInputConfig '%s' has no InputMappingContext rows."), *Config->GetPathName()));
			}

			for (const TPair<TObjectPtr<UInputMappingContext>, int32>& Pair : Contexts)
			{
				const UInputMappingContext* Context = Pair.Key.Get();
				TSharedPtr<FJsonObject> ContextRow = MakeShared<FJsonObject>();
				ContextRow->SetStringField(TEXT("context_path"), ObjectPath(Context));
				ContextRow->SetNumberField(TEXT("priority"), Pair.Value);
				ContextRow->SetBoolField(TEXT("loaded"), Context != nullptr);
				if (Context)
				{
					ContextRow->SetNumberField(TEXT("default_mapping_count"), Context->GetMappings().Num());
					ContextRow->SetNumberField(TEXT("profile_override_count"), Context->GetProfilesWithOverridenMappings().Num());
					AppendContextMappings(Context, Pair.Value, Options.bIncludeMappingProfileOverrides, MappingRows);
				}
				else
				{
					AddIssue(Issues, TEXT("error"), TEXT("config_context_null"), FString::Printf(TEXT("PlayerMappableInputConfig '%s' has a null InputMappingContext row."), *Config->GetPathName()));
					bOk = false;
				}
				ContextRows.Add(MakeShared<FJsonValueObject>(ContextRow));
			}
		}

		int32 MappableCount = 0;
		int32 DuplicateCount = 0;
		ValidateMappingRows(MappingRows, Options, Checks, Issues, bOk, MappableCount, DuplicateCount);

		TArray<TSharedPtr<FJsonValue>> MappingValues;
		for (const TSharedPtr<FJsonObject>& Row : MappingRows)
		{
			MappingValues.Add(MakeShared<FJsonValueObject>(Row));
		}

		for (const TSharedPtr<FJsonValue>& Issue : Issues)
		{
			AllIssues.Add(Issue);
		}
		bGlobalOk = bGlobalOk && bOk;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), bOk);
		Result->SetStringField(TEXT("requested_path"), RequestedPath);
		Result->SetStringField(TEXT("object_path"), ObjectPath(Config));
		Result->SetStringField(TEXT("config_name"), Config ? Config->GetConfigName().ToString() : FString());
		Result->SetStringField(TEXT("display_name"), Config ? Config->GetDisplayName().ToString() : FString());
		Result->SetBoolField(TEXT("deprecated"), Config && Config->IsDeprecated());
		Result->SetNumberField(TEXT("context_count"), Config ? Config->GetMappingContexts().Num() : 0);
		Result->SetNumberField(TEXT("mapping_count"), MappingRows.Num());
		Result->SetNumberField(TEXT("mappable_mapping_count"), MappableCount);
		Result->SetNumberField(TEXT("duplicate_mapping_row_count"), DuplicateCount);
		Result->SetArrayField(TEXT("contexts"), ContextRows);
		Result->SetArrayField(TEXT("mappings"), MappingValues);
		Result->SetArrayField(TEXT("checks"), Checks);
		Result->SetArrayField(TEXT("issues"), Issues);
		return Result;
	}

	static TSharedPtr<FJsonObject> ValidateStandaloneInputMappingContext(
		const FString& RequestedPath,
		UInputMappingContext* Context,
		const FPlayerMappableValidationOptions& Options,
		TArray<TSharedPtr<FJsonValue>>& AllIssues,
		bool& bGlobalOk)
	{
		bool bOk = true;
		TArray<TSharedPtr<FJsonValue>> Checks;
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonObject>> MappingRows;

		UClass* ContextClass = LoadClassPath(TEXT("/Script/EnhancedInput.InputMappingContext"));
		AddCheck(Checks, bOk, TEXT("context_loaded"), Context != nullptr, TEXT("error"), RequestedPath);
		AddCheck(Checks, bOk, TEXT("context_type"), Context && ContextClass && Context->GetClass()->IsChildOf(ContextClass), TEXT("error"), Context ? Context->GetClass()->GetPathName() : FString());

		if (!Context)
		{
			AddIssue(Issues, TEXT("error"), TEXT("context_not_found_or_wrong_type"), FString::Printf(TEXT("Object '%s' is not a UInputMappingContext."), *RequestedPath));
		}
		else
		{
			AppendContextMappings(Context, 0, Options.bIncludeMappingProfileOverrides, MappingRows);
		}

		int32 MappableCount = 0;
		int32 DuplicateCount = 0;
		ValidateMappingRows(MappingRows, Options, Checks, Issues, bOk, MappableCount, DuplicateCount);

		TArray<TSharedPtr<FJsonValue>> MappingValues;
		for (const TSharedPtr<FJsonObject>& Row : MappingRows)
		{
			MappingValues.Add(MakeShared<FJsonValueObject>(Row));
		}

		for (const TSharedPtr<FJsonValue>& Issue : Issues)
		{
			AllIssues.Add(Issue);
		}
		bGlobalOk = bGlobalOk && bOk;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), bOk);
		Result->SetStringField(TEXT("requested_path"), RequestedPath);
		Result->SetStringField(TEXT("context_path"), ObjectPath(Context));
		Result->SetNumberField(TEXT("mapping_count"), MappingRows.Num());
		Result->SetNumberField(TEXT("mappable_mapping_count"), MappableCount);
		Result->SetNumberField(TEXT("duplicate_mapping_row_count"), DuplicateCount);
		Result->SetArrayField(TEXT("mappings"), MappingValues);
		Result->SetArrayField(TEXT("checks"), Checks);
		Result->SetArrayField(TEXT("issues"), Issues);
		return Result;
	}
}

void FMonolithGameSettingsActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("settings"), TEXT("get_status"),
		TEXT("Report GameSettings plugin/module/class availability without hard-linking the optional runtime plugin."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("settings"), TEXT("describe_registry_tree"),
		TEXT("Describe the GameSettings registry/tree contract and optionally classify screen, registry, or setting classes read-only."),
		FMonolithActionHandler::CreateStatic(&DescribeRegistryTree),
		FParamSchemaBuilder()
			.Optional(TEXT("screen_class"), TEXT("string"), TEXT("Optional UGameSettingScreen subclass path to classify."))
			.Optional(TEXT("registry_class"), TEXT("string"), TEXT("Optional UGameSettingRegistry subclass path to classify."))
			.Optional(TEXT("setting_class"), TEXT("string"), TEXT("Optional UGameSetting subclass path to classify."))
			.Build(),
		TEXT("Diagnostics"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("settings"), TEXT("validate_setting_class_contract"),
		TEXT("Validate a UGameSetting class against base, collection, value, concrete, and reflected function expectations."),
		FMonolithActionHandler::CreateStatic(&ValidateSettingClassContract),
		FParamSchemaBuilder()
			.Required(TEXT("setting_class"), TEXT("string"), TEXT("UGameSetting-derived class path to validate."))
			.Optional(TEXT("require_concrete"), TEXT("boolean"), TEXT("Treat abstract/deprecated classes as errors."), TEXT("false"))
			.Optional(TEXT("require_value_setting"), TEXT("boolean"), TEXT("Require the class to derive from UGameSettingValue."), TEXT("false"))
			.Optional(TEXT("require_collection"), TEXT("boolean"), TEXT("Require the class to derive from UGameSettingCollection."), TEXT("false"))
			.Build(),
		TEXT("Validation"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("settings"), TEXT("validate_data_source_bindings"),
		TEXT("Validate GameSettings dynamic getter/setter path shapes read-only; reports static limits because FCachedPropertyPath resolves against a LocalPlayer at runtime."),
		FMonolithActionHandler::CreateStatic(&ValidateDataSourceBindings),
		FParamSchemaBuilder()
			.Optional(TEXT("getter_path"), TEXT("string"), TEXT("Dotted dynamic getter property path."))
			.Optional(TEXT("setter_path"), TEXT("string"), TEXT("Dotted dynamic setter property path."))
			.Optional(TEXT("dynamic_paths"), TEXT("array"), TEXT("Additional dotted dynamic property paths to validate."))
			.Optional(TEXT("require_getter"), TEXT("boolean"), TEXT("Require getter_path or at least one dynamic_paths entry."), TEXT("true"))
			.Optional(TEXT("require_setter"), TEXT("boolean"), TEXT("Require setter_path."), TEXT("false"))
			.Build(),
		TEXT("Validation"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("settings"), TEXT("validate_visual_data"),
		TEXT("Validate a UGameSettingVisualData asset/class contract and report entry-widget/detail-extension map counts read-only."),
		FMonolithActionHandler::CreateStatic(&ValidateVisualData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UGameSettingVisualData asset/object path to inspect."))
			.Optional(TEXT("require_entry_widgets"), TEXT("boolean"), TEXT("Require at least one EntryWidgetForClass or EntryWidgetForName row."), TEXT("false"))
			.Optional(TEXT("require_detail_extensions"), TEXT("boolean"), TEXT("Require at least one detail-extension row."), TEXT("false"))
			.Build(),
		TEXT("Validation"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("settings"), TEXT("validate_player_mappable_input_settings"),
		TEXT("Validate Enhanced Input player-mappable config/context semantics used by GameSettings/Lyra key-binding screens, read-only."),
		FMonolithActionHandler::CreateStatic(&ValidatePlayerMappableInputSettings),
		FParamSchemaBuilder()
			.Optional(TEXT("config_path"), TEXT("string"), TEXT("Optional UPlayerMappableInputConfig asset/object path."))
			.Optional(TEXT("config_paths"), TEXT("array"), TEXT("Additional UPlayerMappableInputConfig asset/object paths."))
			.Optional(TEXT("context_path"), TEXT("string"), TEXT("Optional UInputMappingContext asset/object path."))
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Additional UInputMappingContext asset/object paths."))
			.Optional(TEXT("require_config_name"), TEXT("boolean"), TEXT("Require PlayerMappableInputConfig.ConfigName."), TEXT("true"))
			.Optional(TEXT("require_config_display_name"), TEXT("boolean"), TEXT("Require PlayerMappableInputConfig display name."), TEXT("true"))
			.Optional(TEXT("require_contexts"), TEXT("boolean"), TEXT("Require config assets to reference at least one InputMappingContext."), TEXT("true"))
			.Optional(TEXT("require_mappable_keys"), TEXT("boolean"), TEXT("Require at least one mappable mapping per inspected target."), TEXT("true"))
			.Optional(TEXT("require_unique_mapping_names"), TEXT("boolean"), TEXT("Require unique player-mappable mapping names per default/profile scope."), TEXT("true"))
			.Optional(TEXT("require_mapping_display_names"), TEXT("boolean"), TEXT("Require display names on player-mappable rows."), TEXT("true"))
			.Optional(TEXT("require_valid_keys"), TEXT("boolean"), TEXT("Require player-mappable rows to use valid keys."), TEXT("true"))
			.Optional(TEXT("require_actions"), TEXT("boolean"), TEXT("Require player-mappable rows to reference an InputAction."), TEXT("true"))
			.Optional(TEXT("include_mapping_profile_overrides"), TEXT("boolean"), TEXT("Include UInputMappingContext profile override mappings."), TEXT("true"))
			.Build(),
		TEXT("Validation"),
		MonolithGameSettings::ExplicitReadOnlyPolicy());
}

FMonolithActionResult FMonolithGameSettingsActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	TArray<TSharedPtr<FJsonValue>> Plugins;
	Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameSettings"))));

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameSettings"))));

	UClass* GameSettingClass = LoadClassPath(TEXT("/Script/GameSettings.GameSetting"));
	UClass* RegistryClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingRegistry"));
	UClass* ScreenClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingScreen"));
	UClass* VisualDataClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingVisualData"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetBoolField(TEXT("game_settings_available"), GameSettingClass != nullptr && RegistryClass != nullptr);
	Result->SetBoolField(TEXT("registry_available"), RegistryClass != nullptr);
	Result->SetBoolField(TEXT("screen_available"), ScreenClass != nullptr);
	Result->SetBoolField(TEXT("visual_data_available"), VisualDataClass != nullptr);
	Result->SetArrayField(TEXT("plugins"), Plugins);
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("classes"), KnownClassSummaries());
	Result->SetArrayField(TEXT("registry_contract"), ContractRows());
	Result->SetStringField(TEXT("data_source_dynamic_contract"), TEXT("FGameSettingDataSourceDynamic is a native non-UObject class; this namespace validates supplied dynamic path shapes and reports runtime-resolution limits without instantiating a LocalPlayer."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameSettingsActions::DescribeRegistryTree(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	const FString ScreenClassPath = ReadOptionalStringParam(Params, TEXT("screen_class"));
	const FString RegistryClassPath = ReadOptionalStringParam(Params, TEXT("registry_class"));
	const FString SettingClassPath = ReadOptionalStringParam(Params, TEXT("setting_class"));

	UClass* ScreenBaseClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingScreen"));
	UClass* RegistryBaseClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingRegistry"));
	UClass* SettingBaseClass = LoadClassPath(TEXT("/Script/GameSettings.GameSetting"));
	UClass* CollectionBaseClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingCollection"));

	UClass* ScreenClass = ScreenClassPath.IsEmpty() ? ScreenBaseClass : LoadClassPath(ScreenClassPath);
	UClass* RegistryClass = RegistryClassPath.IsEmpty() ? RegistryBaseClass : LoadClassPath(RegistryClassPath);
	UClass* SettingClass = SettingClassPath.IsEmpty() ? SettingBaseClass : LoadClassPath(SettingClassPath);

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(ScreenClass, TEXT("NavigateToSetting"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(ScreenClass, TEXT("NavigateToSettings"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(ScreenClass, TEXT("GetSettingCollection"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SettingClass, TEXT("GetDevName"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SettingClass, TEXT("GetDisplayName"))));
	Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(SettingClass, TEXT("GetChildSettings"))));

	TArray<TSharedPtr<FJsonValue>> RegistryProperties;
	RegistryProperties.Add(MakeShared<FJsonValueObject>(PropertySummary(RegistryClass, TEXT("TopLevelSettings"))));
	RegistryProperties.Add(MakeShared<FJsonValueObject>(PropertySummary(RegistryClass, TEXT("RegisteredSettings"))));
	RegistryProperties.Add(MakeShared<FJsonValueObject>(PropertySummary(RegistryClass, TEXT("OwningLocalPlayer"))));

	TArray<TSharedPtr<FJsonValue>> CollectionProperties;
	CollectionProperties.Add(MakeShared<FJsonValueObject>(PropertySummary(CollectionBaseClass, TEXT("Settings"))));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetObjectField(TEXT("screen_class"), ClassSummary(ScreenClassPath.IsEmpty() ? TEXT("/Script/GameSettings.GameSettingScreen") : ScreenClassPath, ScreenClass, ScreenBaseClass));
	Result->SetObjectField(TEXT("registry_class"), ClassSummary(RegistryClassPath.IsEmpty() ? TEXT("/Script/GameSettings.GameSettingRegistry") : RegistryClassPath, RegistryClass, RegistryBaseClass));
	Result->SetObjectField(TEXT("setting_class"), ClassSummary(SettingClassPath.IsEmpty() ? TEXT("/Script/GameSettings.GameSetting") : SettingClassPath, SettingClass, SettingBaseClass));
	Result->SetObjectField(TEXT("collection_class"), ClassSummary(TEXT("/Script/GameSettings.GameSettingCollection"), CollectionBaseClass, SettingBaseClass));
	Result->SetArrayField(TEXT("reflected_functions"), Functions);
	Result->SetArrayField(TEXT("registry_properties"), RegistryProperties);
	Result->SetArrayField(TEXT("collection_properties"), CollectionProperties);
	Result->SetArrayField(TEXT("registry_contract"), ContractRows());
	Result->SetStringField(TEXT("tree_instance_status"), TEXT("not_instantiated; action describes reflected class contract only"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameSettingsActions::ValidateSettingClassContract(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	FString SettingClassPath;
	FString Error;
	if (!ReadRequiredStringParam(Params, TEXT("setting_class"), SettingClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	const bool bRequireConcrete = ReadOptionalBoolParam(Params, TEXT("require_concrete"), false);
	const bool bRequireValueSetting = ReadOptionalBoolParam(Params, TEXT("require_value_setting"), false);
	const bool bRequireCollection = ReadOptionalBoolParam(Params, TEXT("require_collection"), false);

	UClass* GameSettingClass = LoadClassPath(TEXT("/Script/GameSettings.GameSetting"));
	UClass* ValueSettingClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingValue"));
	UClass* CollectionClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingCollection"));
	UClass* SettingClass = LoadClassPath(SettingClassPath);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;

	AddCheck(Checks, bOk, TEXT("setting_class_loaded"), SettingClass != nullptr, TEXT("error"), SettingClassPath);
	AddCheck(Checks, bOk, TEXT("setting_class_is_game_setting"), SettingClass && GameSettingClass && SettingClass->IsChildOf(GameSettingClass), TEXT("error"), ObjectPath(SettingClass));
	AddCheck(Checks, bOk, TEXT("setting_class_not_deprecated"), SettingClass && !SettingClass->HasAnyClassFlags(CLASS_Deprecated), TEXT("error"), ObjectPath(SettingClass));
	AddCheck(Checks, bOk, TEXT("setting_class_concrete"), !bRequireConcrete || (SettingClass && !SettingClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)), bRequireConcrete ? TEXT("error") : TEXT("info"), ObjectPath(SettingClass));
	AddCheck(Checks, bOk, TEXT("setting_class_value_setting"), !bRequireValueSetting || (SettingClass && ValueSettingClass && SettingClass->IsChildOf(ValueSettingClass)), bRequireValueSetting ? TEXT("error") : TEXT("info"), ObjectPath(SettingClass));
	AddCheck(Checks, bOk, TEXT("setting_class_collection"), !bRequireCollection || (SettingClass && CollectionClass && SettingClass->IsChildOf(CollectionClass)), bRequireCollection ? TEXT("error") : TEXT("info"), ObjectPath(SettingClass));
	AddCheck(Checks, bOk, TEXT("get_dev_name_reflected"), SettingClass && SettingClass->FindFunctionByName(FName(TEXT("GetDevName"))) != nullptr, TEXT("error"), TEXT("UGameSetting::GetDevName must be callable for authoring diagnostics"));

	if (!SettingClass)
	{
		AddIssue(Issues, TEXT("error"), TEXT("setting_class_not_found"), FString::Printf(TEXT("Class '%s' could not be loaded."), *SettingClassPath));
	}
	else if (!GameSettingClass || !SettingClass->IsChildOf(GameSettingClass))
	{
		AddIssue(Issues, TEXT("error"), TEXT("setting_class_wrong_parent"), FString::Printf(TEXT("Class '%s' is not a UGameSetting subclass."), *ObjectPath(SettingClass)));
	}
	else if (bRequireConcrete && SettingClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		AddIssue(Issues, TEXT("error"), TEXT("setting_class_not_concrete"), FString::Printf(TEXT("Class '%s' is abstract or deprecated."), *ObjectPath(SettingClass)));
	}
	if (bRequireValueSetting && SettingClass && ValueSettingClass && !SettingClass->IsChildOf(ValueSettingClass))
	{
		AddIssue(Issues, TEXT("error"), TEXT("setting_class_not_value_setting"), FString::Printf(TEXT("Class '%s' is not a UGameSettingValue subclass."), *ObjectPath(SettingClass)));
	}
	if (bRequireCollection && SettingClass && CollectionClass && !SettingClass->IsChildOf(CollectionClass))
	{
		AddIssue(Issues, TEXT("error"), TEXT("setting_class_not_collection"), FString::Printf(TEXT("Class '%s' is not a UGameSettingCollection subclass."), *ObjectPath(SettingClass)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("require_concrete"), bRequireConcrete);
	Result->SetBoolField(TEXT("require_value_setting"), bRequireValueSetting);
	Result->SetBoolField(TEXT("require_collection"), bRequireCollection);
	Result->SetObjectField(TEXT("setting_class"), ClassSummary(SettingClassPath, SettingClass, GameSettingClass));
	Result->SetObjectField(TEXT("value_setting_base"), ClassSummary(TEXT("/Script/GameSettings.GameSettingValue"), ValueSettingClass, GameSettingClass));
	Result->SetObjectField(TEXT("collection_base"), ClassSummary(TEXT("/Script/GameSettings.GameSettingCollection"), CollectionClass, GameSettingClass));
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameSettingsActions::ValidateDataSourceBindings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	const FString GetterPath = ReadOptionalStringParam(Params, TEXT("getter_path"));
	const FString SetterPath = ReadOptionalStringParam(Params, TEXT("setter_path"));
	const bool bRequireGetter = ReadOptionalBoolParam(Params, TEXT("require_getter"), true);
	const bool bRequireSetter = ReadOptionalBoolParam(Params, TEXT("require_setter"), false);

	TArray<FString> DynamicPaths;
	FString Error;
	if (!ReadOptionalStringArrayParam(Params, TEXT("dynamic_paths"), DynamicPaths, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> Bindings;

	const bool bHasGetterSource = !GetterPath.IsEmpty() || DynamicPaths.Num() > 0;
	AddCheck(Checks, bOk, TEXT("getter_path_supplied"), !bRequireGetter || bHasGetterSource, bRequireGetter ? TEXT("error") : TEXT("info"), bHasGetterSource ? TEXT("present") : TEXT("missing"));
	if (bRequireGetter && !bHasGetterSource)
	{
		AddIssue(Issues, TEXT("error"), TEXT("getter_path_missing"), TEXT("A getter_path or at least one dynamic_paths entry is required."));
	}

	if (!GetterPath.IsEmpty())
	{
		Bindings.Add(MakeShared<FJsonValueObject>(ValidateDynamicPath(TEXT("getter"), GetterPath, bRequireGetter, Checks, Issues, bOk)));
	}
	if (!SetterPath.IsEmpty() || bRequireSetter)
	{
		Bindings.Add(MakeShared<FJsonValueObject>(ValidateDynamicPath(TEXT("setter"), SetterPath, bRequireSetter, Checks, Issues, bOk)));
	}
	for (int32 Index = 0; Index < DynamicPaths.Num(); ++Index)
	{
		Bindings.Add(MakeShared<FJsonValueObject>(ValidateDynamicPath(FString::Printf(TEXT("dynamic_%d"), Index), DynamicPaths[Index], true, Checks, Issues, bOk)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("require_getter"), bRequireGetter);
	Result->SetBoolField(TEXT("require_setter"), bRequireSetter);
	Result->SetArrayField(TEXT("bindings"), Bindings);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetStringField(TEXT("resolution_status"), TEXT("static_shape_only; FGameSettingDataSourceDynamic resolves FCachedPropertyPath against ULocalPlayer at runtime"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameSettingsActions::ValidateVisualData(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	FString AssetPath;
	FString Error;
	if (!ReadRequiredStringParam(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	const bool bRequireEntryWidgets = ReadOptionalBoolParam(Params, TEXT("require_entry_widgets"), false);
	const bool bRequireDetailExtensions = ReadOptionalBoolParam(Params, TEXT("require_detail_extensions"), false);

	UObject* Object = LoadAnyObjectPath(AssetPath);
	UClass* VisualDataClass = LoadClassPath(TEXT("/Script/GameSettings.GameSettingVisualData"));
	UClass* ObjectClass = Object ? Object->GetClass() : nullptr;
	const bool bIsVisualData = ObjectClass && VisualDataClass && ObjectClass->IsChildOf(VisualDataClass);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Issues;

	AddCheck(Checks, bOk, TEXT("visual_data_loaded"), Object != nullptr, TEXT("error"), AssetPath);
	AddCheck(Checks, bOk, TEXT("visual_data_type"), bIsVisualData, TEXT("error"), ObjectPath(ObjectClass));

	TArray<TSharedPtr<FJsonValue>> Properties;
	const UObject* VisualDataObject = bIsVisualData ? Object : nullptr;
	Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(VisualDataClass, TEXT("EntryWidgetForClass"), VisualDataObject)));
	Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(VisualDataClass, TEXT("EntryWidgetForName"), VisualDataObject)));
	Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(VisualDataClass, TEXT("ExtensionsForClasses"), VisualDataObject)));
	Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(VisualDataClass, TEXT("ExtensionsForName"), VisualDataObject)));

	auto MapCount = [Object, VisualDataClass, bIsVisualData](const TCHAR* PropertyName) -> int32
	{
		if (!bIsVisualData || !Object || !VisualDataClass)
		{
			return 0;
		}
		FMapProperty* MapProperty = FindFProperty<FMapProperty>(VisualDataClass, FName(PropertyName));
		if (!MapProperty)
		{
			return 0;
		}
		FScriptMapHelper Helper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	};

	const int32 EntryWidgetCount = MapCount(TEXT("EntryWidgetForClass")) + MapCount(TEXT("EntryWidgetForName"));
	const int32 DetailExtensionCount = MapCount(TEXT("ExtensionsForClasses")) + MapCount(TEXT("ExtensionsForName"));

	AddCheck(Checks, bOk, TEXT("entry_widgets_present"), !bRequireEntryWidgets || EntryWidgetCount > 0, bRequireEntryWidgets ? TEXT("error") : TEXT("info"), FString::Printf(TEXT("entry_widget_count=%d"), EntryWidgetCount));
	AddCheck(Checks, bOk, TEXT("detail_extensions_present"), !bRequireDetailExtensions || DetailExtensionCount > 0, bRequireDetailExtensions ? TEXT("error") : TEXT("info"), FString::Printf(TEXT("detail_extension_count=%d"), DetailExtensionCount));

	if (!Object)
	{
		AddIssue(Issues, TEXT("error"), TEXT("visual_data_not_found"), FString::Printf(TEXT("Object '%s' could not be loaded."), *AssetPath));
	}
	else if (!bIsVisualData)
	{
		AddIssue(Issues, TEXT("error"), TEXT("visual_data_wrong_type"), FString::Printf(TEXT("Object '%s' is a %s, not UGameSettingVisualData."), *Object->GetPathName(), ObjectClass ? *ObjectClass->GetPathName() : TEXT("<no class>")));
	}
	if (bRequireEntryWidgets && EntryWidgetCount == 0)
	{
		AddIssue(Issues, TEXT("error"), TEXT("entry_widgets_missing"), TEXT("EntryWidgetForClass and EntryWidgetForName are both empty."));
	}
	if (bRequireDetailExtensions && DetailExtensionCount == 0)
	{
		AddIssue(Issues, TEXT("error"), TEXT("detail_extensions_missing"), TEXT("ExtensionsForClasses and ExtensionsForName are both empty."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), ObjectPath(Object));
	Result->SetObjectField(TEXT("class"), ClassSummary(ObjectClass ? ObjectClass->GetPathName() : AssetPath, ObjectClass, VisualDataClass));
	Result->SetNumberField(TEXT("entry_widget_count"), EntryWidgetCount);
	Result->SetNumberField(TEXT("detail_extension_count"), DetailExtensionCount);
	Result->SetArrayField(TEXT("properties"), Properties);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameSettingsActions::ValidatePlayerMappableInputSettings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameSettings;

	TArray<FString> ConfigPaths;
	TArray<FString> ContextPaths;
	FString Error;
	if (!ReadOptionalStringArrayParam(Params, TEXT("config_paths"), ConfigPaths, Error)
		|| !ReadOptionalStringArrayParam(Params, TEXT("context_paths"), ContextPaths, Error)
		|| !AddOptionalStringParamToArray(Params, TEXT("config_path"), ConfigPaths, Error)
		|| !AddOptionalStringParamToArray(Params, TEXT("context_path"), ContextPaths, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams);
	}

	if (ConfigPaths.Num() == 0 && ContextPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Provide config_path/config_paths and/or context_path/context_paths."), ErrInvalidParams);
	}

	FPlayerMappableValidationOptions Options;
	Options.bRequireConfigName = ReadOptionalBoolParam(Params, TEXT("require_config_name"), true);
	Options.bRequireConfigDisplayName = ReadOptionalBoolParam(Params, TEXT("require_config_display_name"), true);
	Options.bRequireContexts = ReadOptionalBoolParam(Params, TEXT("require_contexts"), true);
	Options.bRequireMappableKeys = ReadOptionalBoolParam(Params, TEXT("require_mappable_keys"), true);
	Options.bRequireUniqueMappingNames = ReadOptionalBoolParam(Params, TEXT("require_unique_mapping_names"), true);
	Options.bRequireMappingDisplayNames = ReadOptionalBoolParam(Params, TEXT("require_mapping_display_names"), true);
	Options.bRequireValidKeys = ReadOptionalBoolParam(Params, TEXT("require_valid_keys"), true);
	Options.bRequireActions = ReadOptionalBoolParam(Params, TEXT("require_actions"), true);
	Options.bIncludeMappingProfileOverrides = ReadOptionalBoolParam(Params, TEXT("include_mapping_profile_overrides"), true);

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> ConfigResults;
	TArray<TSharedPtr<FJsonValue>> ContextResults;
	TArray<TSharedPtr<FJsonValue>> Issues;

	for (const FString& ConfigPath : ConfigPaths)
	{
		UObject* Object = LoadAnyObjectPath(ConfigPath);
		UPlayerMappableInputConfig* Config = Cast<UPlayerMappableInputConfig>(Object);
		ConfigResults.Add(MakeShared<FJsonValueObject>(ValidatePlayerMappableConfigObject(ConfigPath, Config, Options, Issues, bOk)));
	}

	for (const FString& ContextPath : ContextPaths)
	{
		UObject* Object = LoadAnyObjectPath(ContextPath);
		UInputMappingContext* Context = Cast<UInputMappingContext>(Object);
		ContextResults.Add(MakeShared<FJsonValueObject>(ValidateStandaloneInputMappingContext(ContextPath, Context, Options, Issues, bOk)));
	}

	TSharedPtr<FJsonObject> OptionsJson = MakeShared<FJsonObject>();
	OptionsJson->SetBoolField(TEXT("require_config_name"), Options.bRequireConfigName);
	OptionsJson->SetBoolField(TEXT("require_config_display_name"), Options.bRequireConfigDisplayName);
	OptionsJson->SetBoolField(TEXT("require_contexts"), Options.bRequireContexts);
	OptionsJson->SetBoolField(TEXT("require_mappable_keys"), Options.bRequireMappableKeys);
	OptionsJson->SetBoolField(TEXT("require_unique_mapping_names"), Options.bRequireUniqueMappingNames);
	OptionsJson->SetBoolField(TEXT("require_mapping_display_names"), Options.bRequireMappingDisplayNames);
	OptionsJson->SetBoolField(TEXT("require_valid_keys"), Options.bRequireValidKeys);
	OptionsJson->SetBoolField(TEXT("require_actions"), Options.bRequireActions);
	OptionsJson->SetBoolField(TEXT("include_mapping_profile_overrides"), Options.bIncludeMappingProfileOverrides);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("settings"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("uses_game_settings_compile_dependency"), false);
	Result->SetBoolField(TEXT("uses_lyra_compile_dependency"), false);
	Result->SetBoolField(TEXT("uses_enhanced_input_compile_dependency"), true);
	Result->SetObjectField(TEXT("options"), OptionsJson);
	Result->SetNumberField(TEXT("config_count"), ConfigResults.Num());
	Result->SetNumberField(TEXT("context_count"), ContextResults.Num());
	Result->SetArrayField(TEXT("configs"), ConfigResults);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetStringField(TEXT("lyra_settings_contract"), TEXT("Lyra key-binding settings are generated from player-mappable mappings; mapping names must be non-empty and unique for save slots, display names must be present for UI rows, and duplicate names are skipped by the registry."));
	return FMonolithActionResult::Success(Result);
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS
