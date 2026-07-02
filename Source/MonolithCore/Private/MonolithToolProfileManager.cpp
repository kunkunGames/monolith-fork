#include "MonolithToolProfileManager.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithJsonUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FString GetProfileStorePath()
{
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith")))
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("Monolith") / TEXT("tool_profiles.json");
	}
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT("Monolith"), TEXT("tool_profiles.json"));
}

TArray<TSharedPtr<FJsonValue>> StringSetToJsonArray(const TSet<FString>& Values)
{
	TArray<FString> Sorted;
	Sorted.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Sorted.Add(Value);
	}
	Sorted.Sort();

	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Sorted.Num());
	for (const FString& Value : Sorted)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

void ReadStringSet(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSet<FString>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Obj.IsValid() || !Obj->TryGetArrayField(Field, Array) || !Array)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
		{
			OutValues.Add(Text);
		}
	}
}

void ReadStringMap(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TMap<FString, FString>& OutValues)
{
	const TSharedPtr<FJsonObject>* MapObj = nullptr;
	if (!Obj.IsValid() || !Obj->TryGetObjectField(Field, MapObj) || !MapObj)
	{
		return;
	}

	for (const auto& Pair : FMonolithJsonUtils::GetFields(*MapObj))
	{
		FString Text;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(Text))
		{
			OutValues.Add(Pair.Key, Text);
		}
	}
}

TSharedPtr<FJsonObject> StringMapToJsonObject(const TMap<FString, FString>& Values)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Values.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		Obj->SetStringField(Key, Values[Key]);
	}
	return Obj;
}

bool IsValidProfileMode(const FString& Mode)
{
	return Mode == TEXT("denylist") || Mode == TEXT("allowlist");
}
}

FMonolithToolProfileManager& FMonolithToolProfileManager::Get()
{
	static FMonolithToolProfileManager Instance;
	return Instance;
}

void FMonolithToolProfileManager::EnsureLoaded()
{
	FScopeLock Scope(&Lock);
	if (!bLoaded)
	{
		Load_NoLock();
		bLoaded = true;
	}
}

void FMonolithToolProfileManager::AddDefaultProfile_NoLock()
{
	if (Profiles.Contains(TEXT("default")))
	{
		return;
	}

	FMonolithToolProfile DefaultProfile;
	DefaultProfile.Id = TEXT("default");
	DefaultProfile.DisplayName = TEXT("Default");
	DefaultProfile.Description = TEXT("All registered Monolith actions are visible and executable.");
	DefaultProfile.Mode = TEXT("denylist");
	DefaultProfile.bBuiltIn = true;
	Profiles.Add(DefaultProfile.Id, MoveTemp(DefaultProfile));
}

void FMonolithToolProfileManager::Load_NoLock()
{
	Profiles.Empty();
	ActiveProfileId = TEXT("default");

	FString Body;
	if (FFileHelper::LoadFileToString(Body, *GetProfileStorePath()))
	{
		TSharedPtr<FJsonObject> Root = FMonolithJsonUtils::Parse(Body);
		if (Root.IsValid())
		{
			Root->TryGetStringField(TEXT("active_profile"), ActiveProfileId);
			const TArray<TSharedPtr<FJsonValue>>* ProfilesArray = nullptr;
			if (Root->TryGetArrayField(TEXT("profiles"), ProfilesArray) && ProfilesArray)
			{
				for (const TSharedPtr<FJsonValue>& Value : *ProfilesArray)
				{
					if (!Value.IsValid() || Value->Type != EJson::Object)
					{
						continue;
					}

					TSharedPtr<FJsonObject> Obj = Value->AsObject();
					FMonolithToolProfile Profile;
					Obj->TryGetStringField(TEXT("id"), Profile.Id);
					if (Profile.Id.IsEmpty())
					{
						continue;
					}
					Obj->TryGetStringField(TEXT("display_name"), Profile.DisplayName);
					Obj->TryGetStringField(TEXT("description"), Profile.Description);
					Obj->TryGetStringField(TEXT("mode"), Profile.Mode);
					Obj->TryGetStringField(TEXT("custom_instructions"), Profile.CustomInstructions);
					Obj->TryGetBoolField(TEXT("built_in"), Profile.bBuiltIn);
					if (!IsValidProfileMode(Profile.Mode))
					{
						Profile.Mode = TEXT("denylist");
					}
					ReadStringSet(Obj, TEXT("enabled_namespaces"), Profile.EnabledNamespaces);
					ReadStringSet(Obj, TEXT("enabled_actions"), Profile.EnabledActions);
					ReadStringSet(Obj, TEXT("disabled_namespaces"), Profile.DisabledNamespaces);
					ReadStringSet(Obj, TEXT("disabled_actions"), Profile.DisabledActions);
					ReadStringMap(Obj, TEXT("description_overrides"), Profile.DescriptionOverrides);
					Profiles.Add(Profile.Id, MoveTemp(Profile));
				}
			}
		}
	}

	AddDefaultProfile_NoLock();
	if (!Profiles.Contains(ActiveProfileId))
	{
		ActiveProfileId = TEXT("default");
	}
}

bool FMonolithToolProfileManager::Save_NoLock(FString& OutError) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("active_profile"), ActiveProfileId);

	TArray<TSharedPtr<FJsonValue>> ProfilesArray;
	ProfilesArray.Reserve(Profiles.Num());

	TArray<FString> Keys;
	Profiles.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		ProfilesArray.Add(MakeShared<FJsonValueObject>(ProfileToJson(Profiles.FindChecked(Key))));
	}
	Root->SetArrayField(TEXT("profiles"), ProfilesArray);

	const FString StorePath = GetProfileStorePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(StorePath), true);
	if (!FFileHelper::SaveStringToFile(FMonolithJsonUtils::Serialize(Root), *StorePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write tool profile store: %s"), *StorePath);
		return false;
	}
	return true;
}

FString FMonolithToolProfileManager::MakeActionId(const FString& Namespace, const FString& Action)
{
	return Namespace + TEXT(".") + Action;
}

bool FMonolithToolProfileManager::IsProfileManagementAction(const FString& Namespace, const FString& Action)
{
	if (Namespace != TEXT("monolith"))
	{
		return false;
	}

	return Action == TEXT("discover")
		|| Action == TEXT("status")
		|| Action == TEXT("list_tool_profiles")
		|| Action == TEXT("get_tool_profile")
		|| Action == TEXT("get_effective_discovery")
		|| Action == TEXT("get_action_metadata_coverage")
		|| Action == TEXT("validate_tool_profile");
}

FString FMonolithToolProfileManager::GetActiveProfileId()
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	return ActiveProfileId;
}

int64 FMonolithToolProfileManager::GetToolListRevision() const
{
	FScopeLock Scope(&Lock);
	return ToolListRevision;
}

void FMonolithToolProfileManager::BumpToolListRevision_NoLock()
{
	// Monotonic; callers hold Lock. A change that affects the advertised tool
	// surface (visibility or description) advances the revision so the MCP
	// server can advertise tools.listChanged and track the list version.
	++ToolListRevision;
}

bool FMonolithToolProfileManager::SetActiveProfile(const FString& ProfileId, FString& OutError)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	if (!Profiles.Contains(ProfileId))
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}
	ActiveProfileId = ProfileId;
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

TArray<FMonolithToolProfile> FMonolithToolProfileManager::ListProfiles()
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	TArray<FMonolithToolProfile> Result;
	Profiles.GenerateValueArray(Result);
	Result.Sort([](const FMonolithToolProfile& A, const FMonolithToolProfile& B) { return A.Id < B.Id; });
	return Result;
}

TOptional<FMonolithToolProfile> FMonolithToolProfileManager::GetProfile(const FString& ProfileId)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	if (const FMonolithToolProfile* Profile = Profiles.Find(ProfileId))
	{
		return *Profile;
	}
	return TOptional<FMonolithToolProfile>();
}

TSharedPtr<FJsonObject> FMonolithToolProfileManager::ProfileToJson(const FMonolithToolProfile& Profile) const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("id"), Profile.Id);
	Obj->SetStringField(TEXT("display_name"), Profile.DisplayName);
	Obj->SetStringField(TEXT("description"), Profile.Description);
	Obj->SetStringField(TEXT("mode"), Profile.Mode);
	Obj->SetStringField(TEXT("custom_instructions"), Profile.CustomInstructions);
	Obj->SetBoolField(TEXT("built_in"), Profile.bBuiltIn);
	Obj->SetArrayField(TEXT("enabled_namespaces"), StringSetToJsonArray(Profile.EnabledNamespaces));
	Obj->SetArrayField(TEXT("enabled_actions"), StringSetToJsonArray(Profile.EnabledActions));
	Obj->SetArrayField(TEXT("disabled_namespaces"), StringSetToJsonArray(Profile.DisabledNamespaces));
	Obj->SetArrayField(TEXT("disabled_actions"), StringSetToJsonArray(Profile.DisabledActions));
	Obj->SetObjectField(TEXT("description_overrides"), StringMapToJsonObject(Profile.DescriptionOverrides));
	return Obj;
}

bool FMonolithToolProfileManager::UpsertProfile(const FMonolithToolProfile& Profile, bool bCreateOnly, FString& OutError)
{
	if (Profile.Id.IsEmpty())
	{
		OutError = TEXT("profile_id is required");
		return false;
	}
	if (!IsValidProfileMode(Profile.Mode))
	{
		OutError = TEXT("profile mode must be 'denylist' or 'allowlist'");
		return false;
	}

	EnsureLoaded();
	FScopeLock Scope(&Lock);
	if (bCreateOnly && Profiles.Contains(Profile.Id))
	{
		OutError = FString::Printf(TEXT("Tool profile already exists: %s"), *Profile.Id);
		return false;
	}
	if (const FMonolithToolProfile* Existing = Profiles.Find(Profile.Id))
	{
		if (Existing->bBuiltIn && Profile.Id == TEXT("default"))
		{
			OutError = TEXT("The built-in default profile cannot be overwritten");
			return false;
		}
	}

	Profiles.Add(Profile.Id, Profile);
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

bool FMonolithToolProfileManager::DeleteProfile(const FString& ProfileId, FString& OutError)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	FMonolithToolProfile* Profile = Profiles.Find(ProfileId);
	if (!Profile)
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}
	if (Profile->bBuiltIn)
	{
		OutError = TEXT("Built-in tool profiles cannot be deleted");
		return false;
	}
	if (ActiveProfileId == ProfileId)
	{
		OutError = TEXT("Cannot delete the active tool profile");
		return false;
	}
	Profiles.Remove(ProfileId);
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

bool FMonolithToolProfileManager::SetActionEnabled(const FString& ProfileId, const FString& ActionId, bool bEnabled, FString& OutError)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	FMonolithToolProfile* Profile = Profiles.Find(ProfileId);
	if (!Profile)
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}
	if (Profile->bBuiltIn)
	{
		OutError = TEXT("Built-in tool profiles cannot be edited");
		return false;
	}

	if (Profile->Mode == TEXT("allowlist"))
	{
		if (bEnabled)
		{
			Profile->EnabledActions.Add(ActionId);
			Profile->DisabledActions.Remove(ActionId);
		}
		else
		{
			Profile->EnabledActions.Remove(ActionId);
			Profile->DisabledActions.Add(ActionId);
		}
	}
	else
	{
		if (bEnabled)
		{
			Profile->DisabledActions.Remove(ActionId);
			Profile->EnabledActions.Add(ActionId);
		}
		else
		{
			Profile->EnabledActions.Remove(ActionId);
			Profile->DisabledActions.Add(ActionId);
		}
	}
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

bool FMonolithToolProfileManager::SetNamespaceEnabled(const FString& ProfileId, const FString& Namespace, bool bEnabled, FString& OutError)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	FMonolithToolProfile* Profile = Profiles.Find(ProfileId);
	if (!Profile)
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}
	if (Profile->bBuiltIn)
	{
		OutError = TEXT("Built-in tool profiles cannot be edited");
		return false;
	}

	if (Profile->Mode == TEXT("allowlist"))
	{
		if (bEnabled)
		{
			Profile->EnabledNamespaces.Add(Namespace);
		}
		else
		{
			Profile->EnabledNamespaces.Remove(Namespace);
		}
	}
	else
	{
		if (bEnabled)
		{
			Profile->DisabledNamespaces.Remove(Namespace);
		}
		else
		{
			Profile->DisabledNamespaces.Add(Namespace);
		}
	}
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

bool FMonolithToolProfileManager::SetDescriptionOverride(const FString& ProfileId, const FString& ActionId, const FString& Description, FString& OutError)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);
	FMonolithToolProfile* Profile = Profiles.Find(ProfileId);
	if (!Profile)
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}
	if (Profile->bBuiltIn)
	{
		OutError = TEXT("Built-in tool profiles cannot be edited");
		return false;
	}

	if (Description.IsEmpty())
	{
		Profile->DescriptionOverrides.Remove(ActionId);
	}
	else
	{
		Profile->DescriptionOverrides.Add(ActionId, Description);
	}
	if (!Save_NoLock(OutError))
	{
		return false;
	}
	BumpToolListRevision_NoLock();
	return true;
}

bool FMonolithToolProfileManager::IsActionAllowed(const FString& Namespace, const FString& Action)
{
	if (IsProfileManagementAction(Namespace, Action))
	{
		return true;
	}

	EnsureLoaded();
	FScopeLock Scope(&Lock);
	const FMonolithToolProfile* Profile = Profiles.Find(ActiveProfileId);
	if (!Profile)
	{
		return true;
	}

	const FString ActionId = MakeActionId(Namespace, Action);
	if (Profile->EnabledActions.Contains(ActionId))
	{
		return true;
	}
	if (Profile->DisabledActions.Contains(ActionId))
	{
		return false;
	}

	if (Profile->Mode == TEXT("allowlist"))
	{
		return Profile->EnabledNamespaces.Contains(Namespace);
	}

	return !Profile->DisabledNamespaces.Contains(Namespace);
}

FMonolithActionInfo FMonolithToolProfileManager::ApplyDescriptionOverride(const FMonolithActionInfo& Info)
{
	EnsureLoaded();
	FScopeLock Scope(&Lock);

	FMonolithActionInfo Effective = Info;
	if (const FMonolithToolProfile* Profile = Profiles.Find(ActiveProfileId))
	{
		if (const FString* Override = Profile->DescriptionOverrides.Find(MakeActionId(Info.Namespace, Info.Action)))
		{
			Effective.Description = *Override;
		}
	}
	return Effective;
}

bool FMonolithToolProfileManager::ValidateProfile(const FString& ProfileId, TArray<FString>& OutUnknownNamespaces, TArray<FString>& OutUnknownActions, FString& OutError)
{
	TOptional<FMonolithToolProfile> MaybeProfile = GetProfile(ProfileId);
	if (!MaybeProfile.IsSet())
	{
		OutError = FString::Printf(TEXT("Unknown tool profile: %s"), *ProfileId);
		return false;
	}

	const FMonolithToolProfile& Profile = MaybeProfile.GetValue();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	auto CheckNamespace = [&Registry, &OutUnknownNamespaces](const FString& Namespace)
	{
		if (!Registry.HasNamespace(Namespace))
		{
			OutUnknownNamespaces.AddUnique(Namespace);
		}
	};

	auto CheckAction = [&Registry, &OutUnknownActions](const FString& ActionId)
	{
		FString Namespace;
		FString Action;
		if (!ActionId.Split(TEXT("."), &Namespace, &Action) || Namespace.IsEmpty() || Action.IsEmpty() || !Registry.HasAction(Namespace, Action))
		{
			OutUnknownActions.AddUnique(ActionId);
		}
	};

	for (const FString& Namespace : Profile.EnabledNamespaces) { CheckNamespace(Namespace); }
	for (const FString& Namespace : Profile.DisabledNamespaces) { CheckNamespace(Namespace); }
	for (const FString& ActionId : Profile.EnabledActions) { CheckAction(ActionId); }
	for (const FString& ActionId : Profile.DisabledActions) { CheckAction(ActionId); }
	for (const auto& Pair : Profile.DescriptionOverrides) { CheckAction(Pair.Key); }

	OutUnknownNamespaces.Sort();
	OutUnknownActions.Sort();
	return true;
}
