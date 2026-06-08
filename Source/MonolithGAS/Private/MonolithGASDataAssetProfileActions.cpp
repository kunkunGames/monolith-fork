#include "MonolithGASDataAssetProfileActions.h"

#include "MonolithParamSchema.h"

#include "Abilities/GameplayAbility.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "Misc/FileHelper.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{

struct FGASProfileRoleSpec
{
	FString Role;
	TArray<FString> Candidates;
	bool bDefaultRequired = false;
	bool bRecommended = false;
};

static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Arr.Add(MakeShared<FJsonValueString>(Value));
	}
	return Arr;
}

static TArray<FGASProfileRoleSpec> MakeDefaultRoleSpecs()
{
	return {
		{ TEXT("ability_class"),
			{ TEXT("AbilityClass"), TEXT("GameplayAbilityClass"), TEXT("GrantedAbilityClass") },
			true, false },
		{ TEXT("damage_effect_class"),
			{ TEXT("DamageEffectClass"), TEXT("DamageGameplayEffectClass") },
			false, true },
		{ TEXT("cooldown_effect_class"),
			{ TEXT("CooldownEffectClass"), TEXT("CooldownGameplayEffectClass") },
			false, true },
		{ TEXT("gameplay_cue_tag"),
			{ TEXT("GameplayCueTag"), TEXT("CueTag"), TEXT("ActivationCueTag") },
			false, true },
		{ TEXT("cooldown_tag"),
			{ TEXT("CooldownTag"), TEXT("AbilityCooldownTag") },
			false, true },
		{ TEXT("event_tag"),
			{ TEXT("GameplayEventTag"), TEXT("EventTag"), TEXT("ActivationEventTag") },
			false, false },
		{ TEXT("input_tag"),
			{ TEXT("InputTag"), TEXT("AbilityInputTag") },
			true, false },
		{ TEXT("input_action"),
			{ TEXT("InputAction"), TEXT("AbilityInputAction") },
			true, false },
		{ TEXT("activation_policy"),
			{ TEXT("ActivationPolicy"), TEXT("AbilityActivationPolicy") },
			true, false },
		{ TEXT("channel_duration"),
			{ TEXT("ChannelDuration"), TEXT("ChargeDuration"), TEXT("HoldDuration") },
			false, false }
	};
}

static void ParseCandidateValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutCandidates)
{
	if (!Value.IsValid())
	{
		return;
	}

	FString Candidate;
	if (Value->TryGetString(Candidate))
	{
		Candidate.TrimStartAndEndInline();
		if (!Candidate.IsEmpty())
		{
			OutCandidates.Add(Candidate);
		}
		return;
	}

	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
		{
			ParseCandidateValue(Entry, OutCandidates);
		}
		return;
	}

	const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
	if (Value->TryGetObject(ObjPtr) && ObjPtr && ObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		if (Obj->TryGetStringField(TEXT("property"), Candidate) || Obj->TryGetStringField(TEXT("field"), Candidate))
		{
			Candidate.TrimStartAndEndInline();
			if (!Candidate.IsEmpty())
			{
				OutCandidates.Add(Candidate);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* CandidateArray = nullptr;
		if (Obj->TryGetArrayField(TEXT("candidates"), CandidateArray) && CandidateArray)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *CandidateArray)
			{
				ParseCandidateValue(Entry, OutCandidates);
			}
		}
	}
}

static TArray<FGASProfileRoleSpec> BuildRoleSpecs(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FGASProfileRoleSpec> Specs = MakeDefaultRoleSpecs();

	const TSharedPtr<FJsonObject>* ProfileObjPtr = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("profile"), ProfileObjPtr) || !ProfileObjPtr || !ProfileObjPtr->IsValid())
	{
		return Specs;
	}

	const TSharedPtr<FJsonObject>& ProfileObj = *ProfileObjPtr;
	for (FGASProfileRoleSpec& Spec : Specs)
	{
		TSharedPtr<FJsonValue> RoleValue = ProfileObj->TryGetField(Spec.Role);
		if (!RoleValue.IsValid())
		{
			continue;
		}

		TArray<FString> OverrideCandidates;
		ParseCandidateValue(RoleValue, OverrideCandidates);
		if (OverrideCandidates.Num() > 0)
		{
			Spec.Candidates = OverrideCandidates;
		}
	}

	return Specs;
}

static TSet<FString> BuildRequiredRoles(const TSharedPtr<FJsonObject>& Params, const TArray<FGASProfileRoleSpec>& RoleSpecs)
{
	TSet<FString> RequiredRoles;
	const TArray<TSharedPtr<FJsonValue>>* RequiredArray = nullptr;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("required_roles"), RequiredArray) && RequiredArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RequiredArray)
		{
			FString Role;
			if (Value.IsValid() && Value->TryGetString(Role))
			{
				Role.TrimStartAndEndInline();
				if (!Role.IsEmpty())
				{
					RequiredRoles.Add(Role);
				}
			}
		}
		return RequiredRoles;
	}

	for (const FGASProfileRoleSpec& Spec : RoleSpecs)
	{
		if (Spec.bDefaultRequired)
		{
			RequiredRoles.Add(Spec.Role);
		}
	}
	return RequiredRoles;
}

static FProperty* FindProfileProperty(UClass* Class, const TArray<FString>& Candidates)
{
	if (!Class)
	{
		return nullptr;
	}

	for (const FString& Candidate : Candidates)
	{
		if (FProperty* Prop = Class->FindPropertyByName(FName(*Candidate)))
		{
			return Prop;
		}
	}

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop)
		{
			continue;
		}
		for (const FString& Candidate : Candidates)
		{
			if (Prop->GetName().Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return Prop;
			}
		}
	}

	return nullptr;
}

static UObject* ResolveProfileTarget(UObject* LoadedAsset, bool& bOutBlueprintCDO)
{
	bOutBlueprintCDO = false;
	if (UBlueprint* BP = Cast<UBlueprint>(LoadedAsset))
	{
		if (BP->GeneratedClass)
		{
			bOutBlueprintCDO = true;
			return BP->GeneratedClass->GetDefaultObject();
		}
		return nullptr;
	}
	return LoadedAsset;
}

static FString ExportPropertyValue(UObject* Target, FProperty* Prop)
{
	if (!Target || !Prop)
	{
		return FString();
	}

	FString Exported;
	Prop->ExportText_InContainer(0, Exported, Target, Target, Target, PPF_None);
	Exported.TrimStartAndEndInline();
	return Exported;
}

static bool RoleExpectsGameplayAbility(const FString& Role)
{
	return Role == TEXT("ability_class");
}

static bool RoleExpectsGameplayEffect(const FString& Role)
{
	return Role == TEXT("damage_effect_class") || Role == TEXT("cooldown_effect_class");
}

static bool RoleExpectsInputAction(const FString& Role)
{
	return Role == TEXT("input_action");
}

static bool RoleExpectsTag(const FString& Role)
{
	return Role.Contains(TEXT("tag"));
}

static bool IsExpectedClassForRole(const FString& Role, UClass* ClassValue)
{
	if (!ClassValue)
	{
		return false;
	}
	if (RoleExpectsGameplayAbility(Role))
	{
		return ClassValue->IsChildOf(UGameplayAbility::StaticClass());
	}
	if (RoleExpectsGameplayEffect(Role))
	{
		return ClassValue->IsChildOf(UGameplayEffect::StaticClass());
	}
	if (RoleExpectsInputAction(Role))
	{
		return ClassValue->IsChildOf(UInputAction::StaticClass());
	}
	return true;
}

static void SetExpectedTypeFields(const FString& Role, const TSharedPtr<FJsonObject>& FieldObj)
{
	if (!FieldObj.IsValid())
	{
		return;
	}
	if (RoleExpectsGameplayAbility(Role))
	{
		FieldObj->SetStringField(TEXT("expected_type"), TEXT("UGameplayAbility class"));
	}
	else if (RoleExpectsGameplayEffect(Role))
	{
		FieldObj->SetStringField(TEXT("expected_type"), TEXT("UGameplayEffect class"));
	}
	else if (RoleExpectsInputAction(Role))
	{
		FieldObj->SetStringField(TEXT("expected_type"), TEXT("UInputAction asset"));
	}
	else if (RoleExpectsTag(Role))
	{
		FieldObj->SetStringField(TEXT("expected_type"), TEXT("FGameplayTag"));
	}
}

static void SerializeResolvedObjectForRole(const FString& Role, UObject* ValueObj, const TSharedPtr<FJsonObject>& FieldObj)
{
	if (!FieldObj.IsValid())
	{
		return;
	}

	FieldObj->SetBoolField(TEXT("is_set"), ValueObj != nullptr);
	if (!ValueObj)
	{
		return;
	}

	FieldObj->SetStringField(TEXT("value"), ValueObj->GetPathName());
	FieldObj->SetStringField(TEXT("resolved_class"), ValueObj->GetClass()->GetPathName());
	FieldObj->SetBoolField(TEXT("resolved"), true);

	if (UClass* ClassValue = Cast<UClass>(ValueObj))
	{
		FieldObj->SetStringField(TEXT("value_class"), ClassValue->GetPathName());
		FieldObj->SetBoolField(TEXT("type_valid"), IsExpectedClassForRole(Role, ClassValue));
	}
	else if (RoleExpectsInputAction(Role))
	{
		FieldObj->SetBoolField(TEXT("type_valid"), ValueObj->IsA(UInputAction::StaticClass()));
	}
}

static void SerializeSoftObjectForRole(const FString& Role, const FSoftObjectPath& Path, const TSharedPtr<FJsonObject>& FieldObj)
{
	if (!FieldObj.IsValid())
	{
		return;
	}

	const FString PathString = Path.ToString();
	FieldObj->SetBoolField(TEXT("is_set"), !PathString.IsEmpty());
	if (PathString.IsEmpty())
	{
		return;
	}

	FieldObj->SetStringField(TEXT("value"), PathString);
	UObject* Resolved = Path.ResolveObject();
	if (!Resolved)
	{
		Resolved = Path.TryLoad();
	}
	FieldObj->SetBoolField(TEXT("resolved"), Resolved != nullptr);
	if (Resolved)
	{
		FieldObj->SetStringField(TEXT("resolved_class"), Resolved->GetClass()->GetPathName());
		if (UClass* ClassValue = Cast<UClass>(Resolved))
		{
			FieldObj->SetStringField(TEXT("value_class"), ClassValue->GetPathName());
			FieldObj->SetBoolField(TEXT("type_valid"), IsExpectedClassForRole(Role, ClassValue));
		}
		else if (RoleExpectsInputAction(Role))
		{
			FieldObj->SetBoolField(TEXT("type_valid"), Resolved->IsA(UInputAction::StaticClass()));
		}
	}
}

static TSharedPtr<FJsonObject> SerializeRoleField(UObject* Target, const FGASProfileRoleSpec& Spec, FProperty* Prop)
{
	TSharedPtr<FJsonObject> FieldObj = MakeShared<FJsonObject>();
	FieldObj->SetStringField(TEXT("role"), Spec.Role);
	FieldObj->SetArrayField(TEXT("candidate_properties"), StringArrayToJson(Spec.Candidates));
	FieldObj->SetBoolField(TEXT("found"), Prop != nullptr);
	FieldObj->SetBoolField(TEXT("recommended"), Spec.bRecommended);
	SetExpectedTypeFields(Spec.Role, FieldObj);

	if (!Target || !Prop)
	{
		FieldObj->SetBoolField(TEXT("is_set"), false);
		return FieldObj;
	}

	FieldObj->SetStringField(TEXT("property_name"), Prop->GetName());
	FieldObj->SetStringField(TEXT("property_type"), Prop->GetCPPType());
	FieldObj->SetStringField(TEXT("property_class"), Prop->GetClass()->GetName());

	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UObject* ValueObj = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(Target));
		SerializeResolvedObjectForRole(Spec.Role, ValueObj, FieldObj);
		return FieldObj;
	}

	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		FSoftObjectPtr* SoftPtr = SoftClassProp->ContainerPtrToValuePtr<FSoftObjectPtr>(Target);
		SerializeSoftObjectForRole(Spec.Role, SoftPtr ? SoftPtr->ToSoftObjectPath() : FSoftObjectPath(), FieldObj);
		return FieldObj;
	}

	if (FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Prop))
	{
		FSoftObjectPtr* SoftPtr = SoftObjectProp->ContainerPtrToValuePtr<FSoftObjectPtr>(Target);
		SerializeSoftObjectForRole(Spec.Role, SoftPtr ? SoftPtr->ToSoftObjectPath() : FSoftObjectPath(), FieldObj);
		return FieldObj;
	}

	if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* ValueObj = ObjectProp->GetObjectPropertyValue(ObjectProp->ContainerPtrToValuePtr<void>(Target));
		SerializeResolvedObjectForRole(Spec.Role, ValueObj, FieldObj);
		return FieldObj;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct == FGameplayTag::StaticStruct())
		{
			const FGameplayTag* Tag = StructProp->ContainerPtrToValuePtr<FGameplayTag>(Target);
			const bool bValid = Tag && Tag->IsValid();
			FieldObj->SetBoolField(TEXT("is_set"), bValid);
			FieldObj->SetBoolField(TEXT("tag_valid"), bValid);
			FieldObj->SetStringField(TEXT("value"), bValid ? Tag->ToString() : TEXT(""));
			return FieldObj;
		}
		if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
		{
			const FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Target);
			TArray<TSharedPtr<FJsonValue>> Tags;
			if (Container)
			{
				for (const FGameplayTag& Tag : *Container)
				{
					Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
			}
			FieldObj->SetBoolField(TEXT("is_set"), Tags.Num() > 0);
			FieldObj->SetNumberField(TEXT("tag_count"), Tags.Num());
			FieldObj->SetArrayField(TEXT("value"), Tags);
			return FieldObj;
		}
	}

	FString Exported = ExportPropertyValue(Target, Prop);
	FieldObj->SetStringField(TEXT("value"), Exported);
	FieldObj->SetBoolField(TEXT("is_set"), !Exported.IsEmpty() && Exported != TEXT("None") && Exported != TEXT("()"));

	if (RoleExpectsTag(Spec.Role) && !Exported.IsEmpty())
	{
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Exported), false);
		FieldObj->SetBoolField(TEXT("tag_valid"), Tag.IsValid());
	}

	if (FNumericProperty* NumericProp = CastField<FNumericProperty>(Prop))
	{
		const void* ValuePtr = NumericProp->ContainerPtrToValuePtr<void>(Target);
		if (NumericProp->IsFloatingPoint())
		{
			FieldObj->SetNumberField(TEXT("number"), NumericProp->GetFloatingPointPropertyValue(ValuePtr));
		}
		else if (NumericProp->IsInteger())
		{
			FieldObj->SetNumberField(TEXT("number"), static_cast<double>(NumericProp->GetSignedIntPropertyValue(ValuePtr)));
		}
	}

	return FieldObj;
}

static bool IsNumericGASPropertyName(const FString& Name)
{
	return Name.Contains(TEXT("Damage"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Cooldown"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Magnitude"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Duration"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Knockback"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Radius"), ESearchCase::IgnoreCase)
		|| Name.Contains(TEXT("Multiplier"), ESearchCase::IgnoreCase);
}

static TArray<TSharedPtr<FJsonValue>> CollectNumericGASFields(UObject* Target)
{
	TArray<TSharedPtr<FJsonValue>> Fields;
	if (!Target)
	{
		return Fields;
	}

	for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		FNumericProperty* NumericProp = CastField<FNumericProperty>(Prop);
		if (!Prop || !NumericProp || !IsNumericGASPropertyName(Prop->GetName()))
		{
			continue;
		}

		const void* ValuePtr = NumericProp->ContainerPtrToValuePtr<void>(Target);
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("property_name"), Prop->GetName());
		Field->SetStringField(TEXT("property_type"), Prop->GetCPPType());
		if (NumericProp->IsFloatingPoint())
		{
			Field->SetNumberField(TEXT("value"), NumericProp->GetFloatingPointPropertyValue(ValuePtr));
		}
		else if (NumericProp->IsInteger())
		{
			Field->SetNumberField(TEXT("value"), static_cast<double>(NumericProp->GetSignedIntPropertyValue(ValuePtr)));
		}
		Fields.Add(MakeShared<FJsonValueObject>(Field));
	}

	return Fields;
}

struct FGASProfileDescribeResult
{
	TSharedPtr<FJsonObject> Json;
	TMap<FString, TSharedPtr<FJsonObject>> RoleFields;
};

static FGASProfileDescribeResult DescribeProfileAsset(
	const FString& AssetPath,
	UObject* LoadedAsset,
	const TArray<FGASProfileRoleSpec>& RoleSpecs)
{
	FGASProfileDescribeResult Result;
	Result.Json = MakeShared<FJsonObject>();
	Result.Json->SetStringField(TEXT("asset_path"), AssetPath);

	bool bBlueprintCDO = false;
	UObject* Target = ResolveProfileTarget(LoadedAsset, bBlueprintCDO);
	Result.Json->SetBoolField(TEXT("is_blueprint_cdo"), bBlueprintCDO);
	Result.Json->SetBoolField(TEXT("loaded"), LoadedAsset != nullptr);
	Result.Json->SetBoolField(TEXT("described"), Target != nullptr);

	if (!LoadedAsset || !Target)
	{
		return Result;
	}

	Result.Json->SetStringField(TEXT("asset_name"), LoadedAsset->GetName());
	Result.Json->SetStringField(TEXT("asset_class"), LoadedAsset->GetClass()->GetPathName());
	Result.Json->SetStringField(TEXT("target_object"), Target->GetPathName());
	Result.Json->SetStringField(TEXT("target_class"), Target->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> RoleArray;
	TSharedPtr<FJsonObject> RoleMap = MakeShared<FJsonObject>();
	for (const FGASProfileRoleSpec& Spec : RoleSpecs)
	{
		FProperty* Prop = FindProfileProperty(Target->GetClass(), Spec.Candidates);
		TSharedPtr<FJsonObject> Field = SerializeRoleField(Target, Spec, Prop);
		Result.RoleFields.Add(Spec.Role, Field);
		RoleArray.Add(MakeShared<FJsonValueObject>(Field));
		RoleMap->SetObjectField(Spec.Role, Field);
	}

	Result.Json->SetArrayField(TEXT("roles"), RoleArray);
	Result.Json->SetObjectField(TEXT("role_fields"), RoleMap);
	Result.Json->SetArrayField(TEXT("numeric_gas_fields"), CollectNumericGASFields(Target));
	return Result;
}

static void AddIssue(
	TArray<TSharedPtr<FJsonValue>>& Issues,
	const FString& Severity,
	const FString& Code,
	const FString& Message,
	const FString& AssetPath,
	const FString& Role = FString())
{
	TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
	Issue->SetStringField(TEXT("severity"), Severity);
	Issue->SetStringField(TEXT("code"), Code);
	Issue->SetStringField(TEXT("message"), Message);
	if (!AssetPath.IsEmpty())
	{
		Issue->SetStringField(TEXT("asset_path"), AssetPath);
	}
	if (!Role.IsEmpty())
	{
		Issue->SetStringField(TEXT("role"), Role);
	}
	Issues.Add(MakeShared<FJsonValueObject>(Issue));
}

static bool FieldBool(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool DefaultValue = false)
{
	bool bValue = DefaultValue;
	if (Obj.IsValid())
	{
		Obj->TryGetBoolField(FieldName, bValue);
	}
	return bValue;
}

static FString FieldString(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	FString Value;
	if (Obj.IsValid())
	{
		Obj->TryGetStringField(FieldName, Value);
	}
	return Value;
}

static bool PolicyRequiresRelease(const FString& PolicyValue)
{
	return PolicyValue.Contains(TEXT("Release"), ESearchCase::IgnoreCase)
		|| PolicyValue.Contains(TEXT("Hold"), ESearchCase::IgnoreCase)
		|| PolicyValue.Contains(TEXT("Channel"), ESearchCase::IgnoreCase);
}

static void ValidateRoleField(
	const FString& AssetPath,
	const FGASProfileRoleSpec& Spec,
	const TSet<FString>& RequiredRoles,
	const TSharedPtr<FJsonObject>& Field,
	TArray<TSharedPtr<FJsonValue>>& Issues)
{
	const bool bRequired = RequiredRoles.Contains(Spec.Role);
	const bool bFound = FieldBool(Field, TEXT("found"));
	const bool bSet = FieldBool(Field, TEXT("is_set"));
	if (!bFound)
	{
		AddIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"),
			bRequired ? TEXT("missing_required_role") : TEXT("missing_recommended_role"),
			FString::Printf(TEXT("No property was found for GAS profile role '%s'."), *Spec.Role),
			AssetPath, Spec.Role);
		return;
	}

	if (!bSet && (bRequired || Spec.bRecommended))
	{
		AddIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"),
			bRequired ? TEXT("empty_required_role") : TEXT("empty_recommended_role"),
			FString::Printf(TEXT("Property '%s' for GAS profile role '%s' is empty."),
				*FieldString(Field, TEXT("property_name")), *Spec.Role),
			AssetPath, Spec.Role);
	}

	if (RoleExpectsTag(Spec.Role) && bSet && !FieldBool(Field, TEXT("tag_valid"), true))
	{
		AddIssue(Issues, TEXT("error"), TEXT("invalid_gameplay_tag"),
			FString::Printf(TEXT("Property '%s' contains an unregistered gameplay tag."),
				*FieldString(Field, TEXT("property_name"))),
			AssetPath, Spec.Role);
	}

	if ((RoleExpectsGameplayAbility(Spec.Role) || RoleExpectsGameplayEffect(Spec.Role) || RoleExpectsInputAction(Spec.Role))
		&& bSet && Field->HasField(TEXT("type_valid")) && !FieldBool(Field, TEXT("type_valid"), true))
	{
		AddIssue(Issues, TEXT("error"), TEXT("invalid_reference_type"),
			FString::Printf(TEXT("Property '%s' does not resolve to expected %s."),
				*FieldString(Field, TEXT("property_name")), *FieldString(Field, TEXT("expected_type"))),
			AssetPath, Spec.Role);
	}

	if ((RoleExpectsGameplayAbility(Spec.Role) || RoleExpectsGameplayEffect(Spec.Role) || RoleExpectsInputAction(Spec.Role))
		&& bSet && Field->HasField(TEXT("resolved")) && !FieldBool(Field, TEXT("resolved"), true))
	{
		AddIssue(Issues, TEXT("error"), TEXT("broken_asset_reference"),
			FString::Printf(TEXT("Property '%s' could not resolve referenced asset '%s'."),
				*FieldString(Field, TEXT("property_name")), *FieldString(Field, TEXT("value"))),
			AssetPath, Spec.Role);
	}
}

static FString NormalizePackagePath(FString Path)
{
	Path.TrimStartAndEndInline();
	if (Path.IsEmpty())
	{
		return TEXT("/Game");
	}
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
	}
	while (Path.EndsWith(TEXT("/")) && Path.Len() > 1)
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

static void CollectDataAssetCandidates(const FString& PathFilter, int32 MaxAssets, TArray<FAssetData>& OutAssets)
{
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(*NormalizePackagePath(PathFilter)));
	AssetRegistry.GetAssets(Filter, OutAssets);

	OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	if (MaxAssets > 0 && OutAssets.Num() > MaxAssets)
	{
		OutAssets.SetNum(MaxAssets);
	}
}

static int32 CountSubstring(const FString& Haystack, const FString& Needle)
{
	if (Needle.IsEmpty())
	{
		return 0;
	}

	int32 Count = 0;
	int32 Index = 0;
	while (Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Index) != INDEX_NONE)
	{
		Index = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Index) + Needle.Len();
		++Count;
	}
	return Count;
}

static TSharedPtr<FJsonObject> BuildProjectInputSourceScan()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	Result->SetStringField(TEXT("source_root"), SourceRoot);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> DeprecatedOccurrences;

	if (!FPaths::DirectoryExists(SourceRoot))
	{
		Result->SetBoolField(TEXT("available"), false);
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Project Source directory was not found.")));
		Result->SetArrayField(TEXT("warnings"), Warnings);
		return Result;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.h"), true, false);
	IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.cpp"), true, false);
	Files.Sort();

	int32 DeprecatedDynamicTags = 0;
	int32 DynamicSpecSourceTags = 0;
	int32 StartedBindings = 0;
	int32 CompletedBindings = 0;
	int32 CanceledBindings = 0;
	int32 AbilitySpecPressed = 0;
	int32 AbilitySpecReleased = 0;
	int32 ExactTagMatches = 0;
	int32 TryActivateByTag = 0;

	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			continue;
		}

		const int32 FileDeprecated = CountSubstring(Text, TEXT("DynamicAbilityTags"));
		DeprecatedDynamicTags += FileDeprecated;
		DynamicSpecSourceTags += CountSubstring(Text, TEXT("GetDynamicSpecSourceTags"));
		StartedBindings += CountSubstring(Text, TEXT("ETriggerEvent::Started"));
		CompletedBindings += CountSubstring(Text, TEXT("ETriggerEvent::Completed"));
		CanceledBindings += CountSubstring(Text, TEXT("ETriggerEvent::Canceled"));
		AbilitySpecPressed += CountSubstring(Text, TEXT("AbilitySpecInputPressed"));
		AbilitySpecReleased += CountSubstring(Text, TEXT("AbilitySpecInputReleased"));
		ExactTagMatches += CountSubstring(Text, TEXT("HasTagExact"));
		TryActivateByTag += CountSubstring(Text, TEXT("TryActivateAbilitiesByTag"));
		TryActivateByTag += CountSubstring(Text, TEXT("TryActivateAbilitiesByInputTag"));

		if (FileDeprecated > 0 && DeprecatedOccurrences.Num() < 25)
		{
			TSharedPtr<FJsonObject> Occurrence = MakeShared<FJsonObject>();
			Occurrence->SetStringField(TEXT("file"), File);
			Occurrence->SetNumberField(TEXT("count"), FileDeprecated);
			DeprecatedOccurrences.Add(MakeShared<FJsonValueObject>(Occurrence));
		}
	}

	Result->SetBoolField(TEXT("available"), true);
	Result->SetNumberField(TEXT("file_count"), Files.Num());
	Result->SetNumberField(TEXT("deprecated_dynamic_ability_tags_count"), DeprecatedDynamicTags);
	Result->SetNumberField(TEXT("dynamic_spec_source_tags_count"), DynamicSpecSourceTags);
	Result->SetNumberField(TEXT("started_binding_count"), StartedBindings);
	Result->SetNumberField(TEXT("completed_binding_count"), CompletedBindings);
	Result->SetNumberField(TEXT("canceled_binding_count"), CanceledBindings);
	Result->SetNumberField(TEXT("ability_spec_input_pressed_count"), AbilitySpecPressed);
	Result->SetNumberField(TEXT("ability_spec_input_released_count"), AbilitySpecReleased);
	Result->SetNumberField(TEXT("exact_tag_match_count"), ExactTagMatches);
	Result->SetNumberField(TEXT("try_activate_by_tag_count"), TryActivateByTag);
	Result->SetArrayField(TEXT("deprecated_dynamic_ability_tags_occurrences"), DeprecatedOccurrences);

	if (DeprecatedDynamicTags > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("Project source references DynamicAbilityTags directly; use FGameplayAbilitySpec::GetDynamicSpecSourceTags() for UE 5.7+ compatibility.")));
	}
	if (DynamicSpecSourceTags == 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("Project source scan did not find GetDynamicSpecSourceTags(); tag-based ability input may not be using the UE 5.7-safe path.")));
	}
	Result->SetArrayField(TEXT("warnings"), Warnings);
	return Result;
}

static double NumberField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	double Value = 0.0;
	if (Obj.IsValid())
	{
		Obj->TryGetNumberField(FieldName, Value);
	}
	return Value;
}

struct FPreparedProfileWrite
{
	FString Role;
	FProperty* Property = nullptr;
	TSharedPtr<FJsonValue> Value;
	TSharedPtr<FJsonObject> Report;
};

static bool JsonValueToImportString(const TSharedPtr<FJsonValue>& Value, FString& OutString)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return false;
	}
	if (Value->TryGetString(OutString))
	{
		return true;
	}
	double Number = 0.0;
	if (Value->TryGetNumber(Number))
	{
		OutString = FString::SanitizeFloat(Number);
		return true;
	}
	bool bBool = false;
	if (Value->TryGetBool(bBool))
	{
		OutString = bBool ? TEXT("true") : TEXT("false");
		return true;
	}
	return false;
}

static UClass* ResolveClassForRole(const FString& Role, const FString& ClassPath, FString& OutError)
{
	UClass* ClassValue = LoadClass<UObject>(nullptr, *ClassPath);
	if (!ClassValue)
	{
		FString LoadError;
		UObject* Asset = MonolithGAS::LoadAssetFromPath(ClassPath, LoadError);
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			ClassValue = BP->GeneratedClass;
		}
		else
		{
			ClassValue = Cast<UClass>(Asset);
		}
	}

	if (!ClassValue)
	{
		OutError = FString::Printf(TEXT("Class reference could not be resolved: %s"), *ClassPath);
		return nullptr;
	}

	if (!IsExpectedClassForRole(Role, ClassValue))
	{
		OutError = FString::Printf(TEXT("Class '%s' is not valid for role '%s'."), *ClassValue->GetPathName(), *Role);
		return nullptr;
	}

	return ClassValue;
}

static UObject* ResolveObjectForRole(const FString& Role, const FString& ObjectPath, FString& OutError)
{
	FString LoadError;
	UObject* ObjectValue = MonolithGAS::LoadAssetFromPath(ObjectPath, LoadError);
	if (!ObjectValue)
	{
		OutError = LoadError.IsEmpty()
			? FString::Printf(TEXT("Object reference could not be resolved: %s"), *ObjectPath)
			: LoadError;
		return nullptr;
	}

	if (RoleExpectsInputAction(Role) && !ObjectValue->IsA(UInputAction::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Object '%s' is not a UInputAction for role '%s'."), *ObjectValue->GetPathName(), *Role);
		return nullptr;
	}

	return ObjectValue;
}

static bool ApplyRoleValue(
	UObject* Target,
	const FString& Role,
	FProperty* Prop,
	const TSharedPtr<FJsonValue>& Value,
	bool bDryRun,
	FString& OutError)
{
	if (!Target || !Prop)
	{
		OutError = TEXT("Missing target object or property.");
		return false;
	}

	FString ImportValue;
	if (!JsonValueToImportString(Value, ImportValue))
	{
		OutError = FString::Printf(TEXT("Field for role '%s' must be a string, number, or boolean."), *Role);
		return false;
	}
	ImportValue.TrimStartAndEndInline();

	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UClass* ClassValue = ImportValue.IsEmpty() || ImportValue == TEXT("None")
			? nullptr
			: ResolveClassForRole(Role, ImportValue, OutError);
		if (!ClassValue && !(ImportValue.IsEmpty() || ImportValue == TEXT("None")))
		{
			return false;
		}
		if (!bDryRun)
		{
			ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(Target), ClassValue);
		}
		return true;
	}

	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		if (!ImportValue.IsEmpty() && ImportValue != TEXT("None"))
		{
			FString ClassError;
			if (!ResolveClassForRole(Role, ImportValue, ClassError))
			{
				OutError = ClassError;
				return false;
			}
		}
		if (!bDryRun)
		{
			FSoftObjectPtr* SoftPtr = SoftClassProp->ContainerPtrToValuePtr<FSoftObjectPtr>(Target);
			if (SoftPtr)
			{
				*SoftPtr = (ImportValue.IsEmpty() || ImportValue == TEXT("None")) ? FSoftObjectPath() : FSoftObjectPath(ImportValue);
			}
		}
		return true;
	}

	if (FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Prop))
	{
		if (RoleExpectsInputAction(Role) && !ImportValue.IsEmpty() && ImportValue != TEXT("None"))
		{
			FString ObjectError;
			if (!ResolveObjectForRole(Role, ImportValue, ObjectError))
			{
				OutError = ObjectError;
				return false;
			}
		}
		if (!bDryRun)
		{
			FSoftObjectPtr* SoftPtr = SoftObjectProp->ContainerPtrToValuePtr<FSoftObjectPtr>(Target);
			if (SoftPtr)
			{
				*SoftPtr = (ImportValue.IsEmpty() || ImportValue == TEXT("None")) ? FSoftObjectPath() : FSoftObjectPath(ImportValue);
			}
		}
		return true;
	}

	if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* ObjectValue = ImportValue.IsEmpty() || ImportValue == TEXT("None")
			? nullptr
			: ResolveObjectForRole(Role, ImportValue, OutError);
		if (!ObjectValue && !(ImportValue.IsEmpty() || ImportValue == TEXT("None")))
		{
			return false;
		}
		if (!bDryRun)
		{
			ObjectProp->SetObjectPropertyValue(ObjectProp->ContainerPtrToValuePtr<void>(Target), ObjectValue);
		}
		return true;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct == FGameplayTag::StaticStruct())
		{
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*ImportValue), false);
			if (!ImportValue.IsEmpty() && ImportValue != TEXT("None") && !Tag.IsValid())
			{
				OutError = FString::Printf(TEXT("Gameplay tag is not registered: %s"), *ImportValue);
				return false;
			}
			if (!bDryRun)
			{
				FGameplayTag* TagPtr = StructProp->ContainerPtrToValuePtr<FGameplayTag>(Target);
				if (TagPtr)
				{
					*TagPtr = Tag;
				}
			}
			return true;
		}
	}

	if (!bDryRun)
	{
		const TCHAR* ImportEnd = Prop->ImportText_InContainer(*ImportValue, Target, Target, PPF_None);
		if (!ImportEnd)
		{
			OutError = FString::Printf(TEXT("Failed to import '%s' into property '%s'."), *ImportValue, *Prop->GetName());
			return false;
		}
	}
	else
	{
		UObject* Duplicate = StaticDuplicateObject(Target, GetTransientPackage());
		if (!Duplicate)
		{
			OutError = TEXT("Dry-run duplicate object creation failed.");
			return false;
		}
		const TCHAR* ImportEnd = Prop->ImportText_InContainer(*ImportValue, Duplicate, Duplicate, PPF_None);
		if (!ImportEnd)
		{
			OutError = FString::Printf(TEXT("Failed to import '%s' into property '%s'."), *ImportValue, *Prop->GetName());
			return false;
		}
	}
	return true;
}

static bool SaveProfileAssetIfRequested(UObject* Asset, bool bSave, bool& bSaved, FString& OutError)
{
	bSaved = false;
	if (!bSave)
	{
		return true;
	}

	if (!Asset)
	{
		OutError = TEXT("Cannot save a null asset.");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Asset has no outer package.");
		return false;
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("SavePackage failed for '%s'."), *PackageFilename);
		return false;
	}
	return true;
}

} // namespace

void FMonolithGASDataAssetProfileActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("describe_data_asset_gas_profile"),
		TEXT("Describe a DataAsset-driven GAS skill profile by mapping semantic roles to reflected UPROPERTY fields without mutating the asset."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeDataAssetGASProfile),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataAsset or Blueprint asset path to inspect"))
			.Optional(TEXT("profile"), TEXT("object"), TEXT("Optional role -> property name/candidates map overriding default GAS profile candidates"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_data_asset_gas_profile"),
		TEXT("Validate DataAsset-driven GAS skill profiles: ability/effect/cue/input/policy fields, tag validity, asset references, duplicate input pairs, and release-path source support."),
		FMonolithActionHandler::CreateStatic(&HandleValidateDataAssetGASProfile),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Content path to scan for DataAssets"), TEXT("/Game"))
			.Optional(TEXT("profile"), TEXT("object"), TEXT("Optional role -> property name/candidates map overriding default GAS profile candidates"))
			.Optional(TEXT("include_content"), TEXT("boolean"), TEXT("Reserved for parity with project search; validation currently scans DataAsset assets only"), TEXT("true"))
			.Optional(TEXT("include_source_scan"), TEXT("boolean"), TEXT("Scan project Source for input release/cancel and DynamicAbilityTags evidence"), TEXT("true"))
			.Optional(TEXT("required_roles"), TEXT("array"), TEXT("Optional role names to treat as required; default is ability_class, input_tag, input_action, activation_policy"))
			.Optional(TEXT("max_assets"), TEXT("number"), TEXT("Maximum DataAsset assets to load and validate (default 200)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_data_asset_gas_fields"),
		TEXT("Set DataAsset-driven GAS profile fields through a role/profile map. Defaults to dry_run=true; real writes are transacted and strict by default."),
		FMonolithActionHandler::CreateStatic(&HandleSetDataAssetGASFields),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataAsset or Blueprint asset path to edit"))
			.Required(TEXT("fields"), TEXT("object"), TEXT("Role -> value map, e.g. {input_tag:'Input.Ability.Primary'}"))
			.Optional(TEXT("profile"), TEXT("object"), TEXT("Optional role -> property name/candidates map overriding default GAS profile candidates"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview writes without mutating asset; defaults to true"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("If true, any invalid role/value aborts the whole write; defaults to true"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after successful non-dry-run write; defaults to false"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithGASDataAssetProfileActions::HandleDescribeDataAssetGASProfile(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrorResult))
	{
		return ErrorResult;
	}

	FString LoadError;
	UObject* LoadedAsset = MonolithGAS::LoadAssetFromPath(AssetPath, LoadError);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(LoadError.IsEmpty()
			? FString::Printf(TEXT("Asset not found: %s"), *AssetPath)
			: LoadError);
	}

	FGASProfileDescribeResult Description = DescribeProfileAsset(AssetPath, LoadedAsset, BuildRoleSpecs(Params));
	bool bDescribed = false;
	if (!Description.Json.IsValid() || !Description.Json->TryGetBoolField(TEXT("described"), bDescribed) || !bDescribed)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Asset '%s' could not be described as a UObject/DataAsset profile target."), *AssetPath));
	}

	Description.Json->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Description.Json);
}

FMonolithActionResult FMonolithGASDataAssetProfileActions::HandleValidateDataAssetGASProfile(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = TEXT("/Game");
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	PathFilter = NormalizePackagePath(PathFilter);

	bool bIncludeSourceScan = true;
	Params->TryGetBoolField(TEXT("include_source_scan"), bIncludeSourceScan);

	double MaxAssetsValue = 200.0;
	Params->TryGetNumberField(TEXT("max_assets"), MaxAssetsValue);
	const int32 MaxAssets = FMath::Clamp(FMath::FloorToInt(MaxAssetsValue), 1, 5000);

	const TArray<FGASProfileRoleSpec> RoleSpecs = BuildRoleSpecs(Params);
	const TSet<FString> RequiredRoles = BuildRequiredRoles(Params, RoleSpecs);

	TArray<FAssetData> Assets;
	CollectDataAssetCandidates(PathFilter, MaxAssets, Assets);

	TArray<TSharedPtr<FJsonValue>> Profiles;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TMap<FString, TArray<FString>> AssetsByInputPair;
	int32 InvalidProfileCount = 0;
	int32 ReleasePolicyProfileCount = 0;

	for (const FAssetData& AssetData : Assets)
	{
		const FString AssetPath = AssetData.GetObjectPathString();
		UObject* LoadedAsset = AssetData.GetAsset();
		FGASProfileDescribeResult Description = DescribeProfileAsset(AssetPath, LoadedAsset, RoleSpecs);
		bool bDescribed = false;
	if (!Description.Json.IsValid() || !Description.Json->TryGetBoolField(TEXT("described"), bDescribed) || !bDescribed)
		{
			AddIssue(Issues, TEXT("error"), TEXT("asset_load_failed"),
				TEXT("DataAsset candidate could not be loaded or described."), AssetPath);
			++InvalidProfileCount;
			continue;
		}

		const int32 IssueCountBefore = Issues.Num();
		for (const FGASProfileRoleSpec& Spec : RoleSpecs)
		{
			if (const TSharedPtr<FJsonObject>* Field = Description.RoleFields.Find(Spec.Role))
			{
				ValidateRoleField(AssetPath, Spec, RequiredRoles, *Field, Issues);
			}
		}

		const TSharedPtr<FJsonObject>* InputActionField = Description.RoleFields.Find(TEXT("input_action"));
		const TSharedPtr<FJsonObject>* InputTagField = Description.RoleFields.Find(TEXT("input_tag"));
		const bool bHasInputAction = InputActionField && FieldBool(*InputActionField, TEXT("is_set"));
		const bool bHasInputTag = InputTagField && FieldBool(*InputTagField, TEXT("is_set"));
		if (bHasInputAction != bHasInputTag)
		{
			AddIssue(Issues, TEXT("error"), TEXT("input_action_tag_mismatch"),
				TEXT("DataAsset defines only one side of the input_action/input_tag pair."),
				AssetPath);
		}
		if (bHasInputAction && bHasInputTag)
		{
			const FString PairKey = FieldString(*InputActionField, TEXT("value")) + TEXT("|") + FieldString(*InputTagField, TEXT("value"));
			AssetsByInputPair.FindOrAdd(PairKey).Add(AssetPath);
		}

		const TSharedPtr<FJsonObject>* PolicyField = Description.RoleFields.Find(TEXT("activation_policy"));
		const FString PolicyValue = PolicyField ? FieldString(*PolicyField, TEXT("value")) : FString();
		const bool bRequiresRelease = PolicyRequiresRelease(PolicyValue);
		if (bRequiresRelease)
		{
			++ReleasePolicyProfileCount;
			if (!bHasInputAction || !bHasInputTag)
			{
				AddIssue(Issues, TEXT("error"), TEXT("release_policy_missing_input_pair"),
					TEXT("Release/channel activation policy requires both input_action and input_tag."),
					AssetPath, TEXT("activation_policy"));
			}
		}

		TSharedPtr<FJsonObject> ReleaseContract = MakeShared<FJsonObject>();
		ReleaseContract->SetBoolField(TEXT("requires_release"), bRequiresRelease);
		ReleaseContract->SetStringField(TEXT("policy_value"), PolicyValue);
		ReleaseContract->SetArrayField(TEXT("required_events"),
			StringArrayToJson(bRequiresRelease
				? TArray<FString>{ TEXT("Started"), TEXT("Completed"), TEXT("Canceled") }
				: TArray<FString>{ TEXT("Started") }));
		Description.Json->SetObjectField(TEXT("release_contract"), ReleaseContract);

		const int32 ProfileIssueCount = Issues.Num() - IssueCountBefore;
		Description.Json->SetNumberField(TEXT("issue_count"), ProfileIssueCount);
		Description.Json->SetBoolField(TEXT("valid"), ProfileIssueCount == 0);
		if (ProfileIssueCount > 0)
		{
			++InvalidProfileCount;
		}
		Profiles.Add(MakeShared<FJsonValueObject>(Description.Json));
	}

	for (const TPair<FString, TArray<FString>>& Pair : AssetsByInputPair)
	{
		if (Pair.Value.Num() <= 1)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("code"), TEXT("duplicate_input_action_tag_pair"));
		Issue->SetStringField(TEXT("message"), TEXT("Multiple DataAssets define the same input_action/input_tag pair."));
		Issue->SetStringField(TEXT("pair"), Pair.Key);
		Issue->SetArrayField(TEXT("assets"), StringArrayToJson(Pair.Value));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		InvalidProfileCount = FMath::Max(InvalidProfileCount, 1);
	}

	TSharedPtr<FJsonObject> SourceScan;
	if (bIncludeSourceScan)
	{
		SourceScan = BuildProjectInputSourceScan();
		if (FieldBool(SourceScan, TEXT("available")))
		{
			if (NumberField(SourceScan, TEXT("deprecated_dynamic_ability_tags_count")) > 0.0)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("deprecated_dynamic_ability_tags"),
					TEXT("Project source references DynamicAbilityTags directly; prefer GetDynamicSpecSourceTags()."),
					FString());
			}

			if (ReleasePolicyProfileCount > 0)
			{
				if (NumberField(SourceScan, TEXT("started_binding_count")) == 0.0
					|| NumberField(SourceScan, TEXT("completed_binding_count")) == 0.0
					|| NumberField(SourceScan, TEXT("canceled_binding_count")) == 0.0
					|| NumberField(SourceScan, TEXT("ability_spec_input_released_count")) == 0.0)
				{
					AddIssue(Issues, TEXT("error"), TEXT("release_binding_source_evidence_missing"),
						TEXT("At least one release/channel DataAsset exists, but source scan did not find complete Started/Completed/Canceled plus AbilitySpecInputReleased evidence."),
						FString());
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path_filter"), PathFilter);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(TEXT("count"), Profiles.Num());
	Result->SetNumberField(TEXT("invalid_count"), InvalidProfileCount);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("release_policy_profile_count"), ReleasePolicyProfileCount);
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	Result->SetBoolField(TEXT("truncated"), Assets.Num() >= MaxAssets);
	Result->SetArrayField(TEXT("profiles"), Profiles);
	Result->SetArrayField(TEXT("issues"), Issues);
	if (SourceScan.IsValid())
	{
		Result->SetObjectField(TEXT("source_scan"), SourceScan);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASDataAssetProfileActions::HandleSetDataAssetGASFields(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrorResult))
	{
		return ErrorResult;
	}

	const TSharedPtr<FJsonObject>* FieldsObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("fields"), FieldsObjPtr) || !FieldsObjPtr || !FieldsObjPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: fields (object)"));
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bStrict = true;
	Params->TryGetBoolField(TEXT("strict"), bStrict);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString LoadError;
	UObject* LoadedAsset = MonolithGAS::LoadAssetFromPath(AssetPath, LoadError);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(LoadError.IsEmpty()
			? FString::Printf(TEXT("Asset not found: %s"), *AssetPath)
			: LoadError);
	}

	bool bBlueprintCDO = false;
	UObject* Target = ResolveProfileTarget(LoadedAsset, bBlueprintCDO);
	if (!Target)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Asset '%s' could not be resolved to a writable profile target."), *AssetPath));
	}

	const TArray<FGASProfileRoleSpec> RoleSpecs = BuildRoleSpecs(Params);
	TMap<FString, FGASProfileRoleSpec> SpecsByRole;
	for (const FGASProfileRoleSpec& Spec : RoleSpecs)
	{
		SpecsByRole.Add(Spec.Role, Spec);
	}

	TArray<FPreparedProfileWrite> PreparedWrites;
	TArray<TSharedPtr<FJsonValue>> WriteReports;
	TArray<TSharedPtr<FJsonValue>> Errors;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& FieldPair : (*FieldsObjPtr)->Values)
	{
		const FString Role = FieldPair.Key;
		const FGASProfileRoleSpec* Spec = SpecsByRole.Find(Role);
		TSharedPtr<FJsonObject> WriteReport = MakeShared<FJsonObject>();
		WriteReport->SetStringField(TEXT("role"), Role);
		WriteReport->SetBoolField(TEXT("dry_run"), bDryRun);

		if (!Spec)
		{
			WriteReport->SetBoolField(TEXT("ok"), false);
			FString ErrorMsg = FString::Printf(TEXT("Unknown GAS profile role \'%s\'."), *Role);
			WriteReport->SetStringField(TEXT("error"), ErrorMsg);
			WriteReports.Add(MakeShared<FJsonValueObject>(WriteReport));
			AddIssue(Errors, TEXT("error"), TEXT("unknown_role"), ErrorMsg, AssetPath, Role);
			continue;
		}

		FProperty* Prop = FindProfileProperty(Target->GetClass(), Spec->Candidates);
		if (!Prop)
		{
			WriteReport->SetBoolField(TEXT("ok"), false);
			FString ErrorMsg = FString::Printf(TEXT("No property found for GAS profile role \'%s\'."), *Role);
			WriteReport->SetStringField(TEXT("error"), ErrorMsg);
			WriteReport->SetArrayField(TEXT("candidate_properties"), StringArrayToJson(Spec->Candidates));
			WriteReports.Add(MakeShared<FJsonValueObject>(WriteReport));
			AddIssue(Errors, TEXT("error"), TEXT("missing_role_property"), ErrorMsg, AssetPath, Role);
			continue;
		}

		WriteReport->SetStringField(TEXT("property_name"), Prop->GetName());
		WriteReport->SetStringField(TEXT("property_type"), Prop->GetCPPType());
		WriteReport->SetStringField(TEXT("old_value"), ExportPropertyValue(Target, Prop));

		FString NewValueString;
		JsonValueToImportString(FieldPair.Value, NewValueString);
		WriteReport->SetStringField(TEXT("new_value"), NewValueString);

		FString ValidationError;
		if (!ApplyRoleValue(Target, Role, Prop, FieldPair.Value, true, ValidationError))
		{
			WriteReport->SetBoolField(TEXT("ok"), false);
			WriteReport->SetStringField(TEXT("error"), ValidationError);
			WriteReports.Add(MakeShared<FJsonValueObject>(WriteReport));
			AddIssue(Errors, TEXT("error"), TEXT("invalid_field_value"), ValidationError, AssetPath, Role);
			continue;
		}

		WriteReport->SetBoolField(TEXT("ok"), true);
		PreparedWrites.Add({ Role, Prop, FieldPair.Value, WriteReport });
		WriteReports.Add(MakeShared<FJsonValueObject>(WriteReport));
	}

	if (Errors.Num() > 0 && bStrict)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("strict"), bStrict);
		Result->SetBoolField(TEXT("committed"), false);
		Result->SetNumberField(TEXT("write_count"), PreparedWrites.Num());
		Result->SetNumberField(TEXT("error_count"), Errors.Num());
		Result->SetArrayField(TEXT("writes"), WriteReports);
		Result->SetArrayField(TEXT("errors"), Errors);
		Result->SetStringField(TEXT("message"), TEXT("Strict mode aborted before mutation because one or more fields are invalid."));
		return FMonolithActionResult::Success(Result);
	}

	bool bCommitted = false;
	bool bSaved = false;
	if (!bDryRun && PreparedWrites.Num() > 0)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("MonolithGAS", "SetDataAssetGASFields", "Set DataAsset GAS Fields"));
		Target->Modify();
		for (FPreparedProfileWrite& Prepared : PreparedWrites)
		{
			FString ApplyError;
			Target->PreEditChange(Prepared.Property);
			const bool bApplied = ApplyRoleValue(Target, Prepared.Role, Prepared.Property, Prepared.Value, false, ApplyError);
			FPropertyChangedEvent ChangeEvent(Prepared.Property, EPropertyChangeType::ValueSet);
			Target->PostEditChangeProperty(ChangeEvent);
			Prepared.Report->SetBoolField(TEXT("applied"), bApplied);
			if (!bApplied)
			{
				Prepared.Report->SetStringField(TEXT("error"), ApplyError);
				AddIssue(Errors, TEXT("error"), TEXT("apply_failed"), ApplyError, AssetPath, Prepared.Role);
			}
		}

		LoadedAsset->MarkPackageDirty();
		Target->MarkPackageDirty();
		bCommitted = Errors.Num() == 0 || !bStrict;

		FString SaveError;
		if (!SaveProfileAssetIfRequested(LoadedAsset, bSave, bSaved, SaveError))
		{
			AddIssue(Errors, TEXT("error"), TEXT("save_failed"), SaveError, AssetPath);
		}
	}

	FGASProfileDescribeResult Description = DescribeProfileAsset(AssetPath, LoadedAsset, RoleSpecs);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("strict"), bStrict);
	Result->SetBoolField(TEXT("save_requested"), bSave);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("committed"), bCommitted);
	Result->SetBoolField(TEXT("is_blueprint_cdo"), bBlueprintCDO);
	Result->SetNumberField(TEXT("write_count"), PreparedWrites.Num());
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetArrayField(TEXT("writes"), WriteReports);
	if (Errors.Num() > 0)
	{
		Result->SetArrayField(TEXT("errors"), Errors);
	}
	if (Description.Json.IsValid())
	{
		Result->SetObjectField(TEXT("profile_after"), Description.Json);
	}
	Result->SetStringField(TEXT("message"),
		bDryRun
			? TEXT("Dry-run completed; no asset was mutated.")
			: (bCommitted ? TEXT("DataAsset GAS fields updated.") : TEXT("No fields were committed.")));
	return FMonolithActionResult::Success(Result);
}
