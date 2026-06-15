#include "MonolithAudioAssetActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

// Audio asset types
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"  // F18: USoundWave for create_test_wave
#include "Memory/SharedBuffer.h"  // F18: FSharedBuffer for FEditorAudioBulkData::UpdatePayload (UE 5.4+)

// Factories (AudioEditor module)
#include "Factories/SoundAttenuationFactory.h"
#include "Factories/SoundClassFactory.h"
#include "Factories/SoundMixFactory.h"
#include "Factories/SoundConcurrencyFactory.h"
#include "Factories/SoundSubmixFactory.h"

// Asset registry & utilities
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "MonolithPackagePathValidator.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Helpers
// ============================================================================

namespace
{
	struct FMonolithAudioEQSettingsDraft
	{
		double RootTime = 0.0;
		float FrequencyCenter0 = 600.0f;
		float Gain0 = 1.0f;
		float Bandwidth0 = 1.0f;
		float FrequencyCenter1 = 1000.0f;
		float Gain1 = 1.0f;
		float Bandwidth1 = 1.0f;
		float FrequencyCenter2 = 2000.0f;
		float Gain2 = 1.0f;
		float Bandwidth2 = 1.0f;
		float FrequencyCenter3 = 10000.0f;
		float Gain3 = 1.0f;
		float Bandwidth3 = 1.0f;
	};

	bool ReadAudioEQNumberField(const TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName, double& OutValue, FString& OutError)
	{
		if (!Json->HasField(FieldName))
		{
			return true;
		}

		if (!Json->TryGetNumberField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a number"), FieldName);
			return false;
		}

		return true;
	}

	bool JsonToAudioEQSettingsDraft(const TSharedPtr<FJsonObject>& Json, FMonolithAudioEQSettingsDraft& OutDraft, FString& OutError)
	{
		if (!Json.IsValid())
		{
			OutError = TEXT("eq_settings must be an object");
			return false;
		}

		double DVal = 0.0;
		if (!ReadAudioEQNumberField(Json, TEXT("RootTime"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("RootTime"))) OutDraft.RootTime = DVal;
		if (!ReadAudioEQNumberField(Json, TEXT("FrequencyCenter0"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("FrequencyCenter0"))) OutDraft.FrequencyCenter0 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Gain0"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Gain0"))) OutDraft.Gain0 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Bandwidth0"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Bandwidth0"))) OutDraft.Bandwidth0 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("FrequencyCenter1"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("FrequencyCenter1"))) OutDraft.FrequencyCenter1 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Gain1"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Gain1"))) OutDraft.Gain1 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Bandwidth1"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Bandwidth1"))) OutDraft.Bandwidth1 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("FrequencyCenter2"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("FrequencyCenter2"))) OutDraft.FrequencyCenter2 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Gain2"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Gain2"))) OutDraft.Gain2 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Bandwidth2"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Bandwidth2"))) OutDraft.Bandwidth2 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("FrequencyCenter3"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("FrequencyCenter3"))) OutDraft.FrequencyCenter3 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Gain3"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Gain3"))) OutDraft.Gain3 = static_cast<float>(DVal);
		if (!ReadAudioEQNumberField(Json, TEXT("Bandwidth3"), DVal, OutError)) return false;
		if (Json->HasField(TEXT("Bandwidth3"))) OutDraft.Bandwidth3 = static_cast<float>(DVal);

		return true;
	}

	void ApplyAudioEQSettingsDraft(const FMonolithAudioEQSettingsDraft& Draft, FAudioEQEffect& OutSettings)
	{
		OutSettings.RootTime = Draft.RootTime;
		OutSettings.FrequencyCenter0 = Draft.FrequencyCenter0;
		OutSettings.Gain0 = Draft.Gain0;
		OutSettings.Bandwidth0 = Draft.Bandwidth0;
		OutSettings.FrequencyCenter1 = Draft.FrequencyCenter1;
		OutSettings.Gain1 = Draft.Gain1;
		OutSettings.Bandwidth1 = Draft.Bandwidth1;
		OutSettings.FrequencyCenter2 = Draft.FrequencyCenter2;
		OutSettings.Gain2 = Draft.Gain2;
		OutSettings.Bandwidth2 = Draft.Bandwidth2;
		OutSettings.FrequencyCenter3 = Draft.FrequencyCenter3;
		OutSettings.Gain3 = Draft.Gain3;
		OutSettings.Bandwidth3 = Draft.Bandwidth3;
	}
}

bool FMonolithAudioAssetActions::SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash <= 0)
	{
		return false;
	}
	OutPackagePath = AssetPath.Left(LastSlash);
	OutAssetName = AssetPath.Mid(LastSlash + 1);
	return !OutAssetName.IsEmpty();
}

template<typename T>
T* FMonolithAudioAssetActions::LoadAudioAsset(const FString& AssetPath, FString& OutError)
{
	// Normalize short paths: /Game/Foo/Bar -> /Game/Foo/Bar.Bar
	// FSoftObjectPath requires the Package.AssetName format to resolve.
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.Contains(TEXT(".")))
	{
		int32 LastSlash;
		if (NormalizedPath.FindLastChar('/', LastSlash) && LastSlash >= 0)
		{
			FString AssetName = NormalizedPath.Mid(LastSlash + 1);
			if (!AssetName.IsEmpty())
			{
				NormalizedPath = NormalizedPath + TEXT(".") + AssetName;
			}
		}
	}

	// Registry-first loading (per CLAUDE.md lessons)
	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizedPath));
	if (AssetData.IsValid())
	{
		UObject* Loaded = AssetData.GetAsset();
		T* Typed = Cast<T>(Loaded);
		if (!Typed)
		{
			OutError = FString::Printf(TEXT("Asset at '%s' is not a %s (found %s)"),
				*AssetPath, *T::StaticClass()->GetName(),
				Loaded ? *Loaded->GetClass()->GetName() : TEXT("null"));
		}
		return Typed;
	}

	// StaticLoadObject fallback (handles both short and long paths)
	UObject* Loaded = StaticLoadObject(T::StaticClass(), nullptr, *AssetPath);
	if (!Loaded)
	{
		OutError = FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath);
		return nullptr;
	}
	T* Typed = Cast<T>(Loaded);
	if (!Typed)
	{
		OutError = FString::Printf(TEXT("Asset at '%s' is not a %s"), *AssetPath, *T::StaticClass()->GetName());
	}
	return Typed;
}

template<typename TFactory, typename TAsset>
TAsset* FMonolithAudioAssetActions::CreateAudioAsset(const FString& AssetPath, FString& OutError)
{
	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		OutError = TEXT("Invalid asset path — must contain at least one '/' (e.g. /Game/Audio/SA_MyAttenuation)");
		return nullptr;
	}

	// Defensive: reject malformed paths (e.g. "//Game/...") before StaticLoadObject/CreatePackage can assert.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
	{
		OutError = ValidationError;
		return nullptr;
	}

	// Check if asset already exists
	UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (Existing)
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath);
		return nullptr;
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		OutError = FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath);
		return nullptr;
	}

	TFactory* Factory = NewObject<TFactory>();
	UObject* NewObj = Factory->FactoryCreateNew(
		TAsset::StaticClass(), Pkg, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);

	TAsset* Asset = Cast<TAsset>(NewObj);
	if (!Asset)
	{
		OutError = TEXT("Factory failed to create asset");
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Asset);
	Pkg->MarkPackageDirty();

	// Save to disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, Asset, *PackageFilename, SaveArgs);

	return Asset;
}

// ============================================================================
// Reflection: Struct <-> JSON
// ============================================================================

TSharedPtr<FJsonObject> FMonolithAudioAssetActions::StructToJson(const UStruct* StructDef, const void* StructData)
{
	auto Json = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> PropIt(StructDef); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructData);
		const FString PropName = Prop->GetName();

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			Json->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		}
		else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			Json->SetNumberField(PropName, FloatProp->GetPropertyValue(ValuePtr));
		}
		else if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			Json->SetNumberField(PropName, DoubleProp->GetPropertyValue(ValuePtr));
		}
		else if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			Json->SetNumberField(PropName, static_cast<double>(IntProp->GetPropertyValue(ValuePtr)));
		}
		else if (const FUInt32Property* UIntProp = CastField<FUInt32Property>(Prop))
		{
			Json->SetNumberField(PropName, static_cast<double>(UIntProp->GetPropertyValue(ValuePtr)));
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			Json->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			Json->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const UEnum* Enum = EnumProp->GetEnum();
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			int64 Val = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			FString EnumStr = Enum ? Enum->GetNameStringByValue(Val) : FString::FromInt(Val);
			Json->SetStringField(PropName, EnumStr);
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				uint8 Val = ByteProp->GetPropertyValue(ValuePtr);
				Json->SetStringField(PropName, ByteProp->Enum->GetNameStringByValue(Val));
			}
			else
			{
				Json->SetNumberField(PropName, static_cast<double>(ByteProp->GetPropertyValue(ValuePtr)));
			}
		}
		else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
			Json->SetStringField(PropName, Obj ? Obj->GetPathName() : TEXT("None"));
		}
		else if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			// Recurse into nested structs
			TSharedPtr<FJsonObject> NestedJson = StructToJson(StructProp->Struct, ValuePtr);
			Json->SetObjectField(PropName, NestedJson);
		}
		else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			// Serialize arrays of structs, objects, or primitives
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			JsonArray.Reserve(ArrayHelper.Num());

			for (int32 i = 0; i < ArrayHelper.Num(); ++i)
			{
				const void* ElemPtr = ArrayHelper.GetRawPtr(i);
				FProperty* InnerProp = ArrayProp->Inner;

				if (const FStructProperty* InnerStruct = CastField<FStructProperty>(InnerProp))
				{
					TSharedPtr<FJsonObject> ElemJson = StructToJson(InnerStruct->Struct, ElemPtr);
					JsonArray.Add(MakeShared<FJsonValueObject>(ElemJson));
				}
				else if (const FObjectPropertyBase* InnerObj = CastField<FObjectPropertyBase>(InnerProp))
				{
					UObject* Obj = InnerObj->GetObjectPropertyValue(ElemPtr);
					JsonArray.Add(MakeShared<FJsonValueString>(Obj ? Obj->GetPathName() : TEXT("None")));
				}
				else if (const FFloatProperty* InnerFloat = CastField<FFloatProperty>(InnerProp))
				{
					JsonArray.Add(MakeShared<FJsonValueNumber>(InnerFloat->GetPropertyValue(ElemPtr)));
				}
				else if (const FIntProperty* InnerInt = CastField<FIntProperty>(InnerProp))
				{
					JsonArray.Add(MakeShared<FJsonValueNumber>(static_cast<double>(InnerInt->GetPropertyValue(ElemPtr))));
				}
				else if (const FStrProperty* InnerStr = CastField<FStrProperty>(InnerProp))
				{
					JsonArray.Add(MakeShared<FJsonValueString>(InnerStr->GetPropertyValue(ElemPtr)));
				}
				else
				{
					// Fallback: export as string
					FString ExportStr;
					InnerProp->ExportTextItem_Direct(ExportStr, ElemPtr, nullptr, nullptr, PPF_None);
					JsonArray.Add(MakeShared<FJsonValueString>(ExportStr));
				}
			}
			Json->SetArrayField(PropName, JsonArray);
		}
		// Skip anything we can't serialize (delegates, maps, etc.)
	}

	return Json;
}

bool FMonolithAudioAssetActions::JsonToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* StructDef, void* StructData, FString& OutError)
{
	if (!Json.IsValid())
	{
		OutError = TEXT("JSON object is null");
		return false;
	}

	for (const auto& Pair : Json->Values)
	{
		const FString& FieldName = Pair.Key;
		const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

		FProperty* Prop = StructDef->FindPropertyByName(FName(*FieldName));
		if (!Prop)
		{
			// Skip unknown fields silently — allows forward-compatible partial updates
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructData);

		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			bool bVal;
			if (JsonVal->TryGetBool(bVal))
			{
				BoolProp->SetPropertyValue(ValuePtr, bVal);
			}
		}
		else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			double DVal;
			if (JsonVal->TryGetNumber(DVal))
			{
				FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(DVal));
			}
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			double DVal;
			if (JsonVal->TryGetNumber(DVal))
			{
				DoubleProp->SetPropertyValue(ValuePtr, DVal);
			}
		}
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			double DVal;
			if (JsonVal->TryGetNumber(DVal))
			{
				IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(DVal));
			}
		}
		else if (FUInt32Property* UIntProp = CastField<FUInt32Property>(Prop))
		{
			double DVal;
			if (JsonVal->TryGetNumber(DVal))
			{
				UIntProp->SetPropertyValue(ValuePtr, static_cast<uint32>(DVal));
			}
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			FString SVal;
			if (JsonVal->TryGetString(SVal))
			{
				StrProp->SetPropertyValue(ValuePtr, SVal);
			}
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			FString SVal;
			if (JsonVal->TryGetString(SVal))
			{
				NameProp->SetPropertyValue(ValuePtr, FName(*SVal));
			}
		}
		else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FString SVal;
			if (JsonVal->TryGetString(SVal))
			{
				const UEnum* Enum = EnumProp->GetEnum();
				int64 EnumVal = Enum ? Enum->GetValueByNameString(SVal) : INDEX_NONE;
				if (EnumVal != INDEX_NONE)
				{
					FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
					UnderlyingProp->SetIntPropertyValue(ValuePtr, EnumVal);
				}
				else
				{
					OutError = FString::Printf(TEXT("Unknown enum value '%s' for property '%s'"), *SVal, *FieldName);
					return false;
				}
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				FString SVal;
				if (JsonVal->TryGetString(SVal))
				{
					int64 EnumVal = ByteProp->Enum->GetValueByNameString(SVal);
					if (EnumVal != INDEX_NONE)
					{
						ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumVal));
					}
					else
					{
						OutError = FString::Printf(TEXT("Unknown enum value '%s' for property '%s'"), *SVal, *FieldName);
						return false;
					}
				}
			}
			else
			{
				double DVal;
				if (JsonVal->TryGetNumber(DVal))
				{
					ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(DVal));
				}
			}
		}
		else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			FString SVal;
			if (JsonVal->TryGetString(SVal))
			{
				if (SVal == TEXT("None") || SVal.IsEmpty())
				{
					ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
				}
				else
				{
					UObject* Loaded = StaticLoadObject(ObjProp->PropertyClass, nullptr, *SVal);
					if (Loaded)
					{
						ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
					}
					else
					{
						OutError = FString::Printf(TEXT("Could not load object '%s' for property '%s'"), *SVal, *FieldName);
						return false;
					}
				}
			}
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			const TSharedPtr<FJsonObject>* NestedObj = nullptr;
			if (JsonVal->TryGetObject(NestedObj) && NestedObj && NestedObj->IsValid())
			{
				if (!JsonToStruct(*NestedObj, StructProp->Struct, ValuePtr, OutError))
				{
					return false;
				}
			}
		}
		// Arrays and other complex types: skip silently for now (partial update)
	}

	return true;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithAudioAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- Sound Attenuation ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_attenuation"),
		TEXT("Create a new USoundAttenuation asset with optional initial settings"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateSoundAttenuation),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/SA_MyAttenuation)"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("FSoundAttenuationSettings fields to set on creation"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_attenuation_settings"),
		TEXT("Get all FSoundAttenuationSettings fields from a USoundAttenuation asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::GetAttenuationSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundAttenuation"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_attenuation_settings"),
		TEXT("Set FSoundAttenuationSettings fields on a USoundAttenuation (partial update)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::SetAttenuationSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundAttenuation"))
			.Required(TEXT("settings"), TEXT("object"), TEXT("FSoundAttenuationSettings fields to update"))
			.Build());

	// --- Sound Class ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_class"),
		TEXT("Create a new USoundClass asset with optional parent and properties"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateSoundClass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/SC_Ambient)"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Asset path of parent USoundClass for hierarchy"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("FSoundClassProperties fields to set on creation"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_class_properties"),
		TEXT("Get FSoundClassProperties + parent/children from a USoundClass asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::GetSoundClassProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundClass"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_sound_class_properties"),
		TEXT("Set FSoundClassProperties fields on a USoundClass (partial update). Optionally reparent."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::SetSoundClassProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundClass"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("FSoundClassProperties fields to update"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Asset path of new parent USoundClass (empty to clear)"))
			.Build());

	// --- Sound Mix ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_mix"),
		TEXT("Create a new USoundMix asset with optional EQ settings and class adjusters"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateSoundMix),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/Mix_Combat)"))
			.Optional(TEXT("eq_settings"), TEXT("object"), TEXT("FAudioEQEffect fields (4-band EQ)"))
			.Optional(TEXT("class_effects"), TEXT("array"), TEXT("Array of FSoundClassAdjuster objects"))
			.Optional(TEXT("initial_delay"), TEXT("number"), TEXT("Delay before mix takes effect"))
			.Optional(TEXT("fade_in_time"), TEXT("number"), TEXT("Fade-in duration"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Mix duration (-1 for infinite)"))
			.Optional(TEXT("fade_out_time"), TEXT("number"), TEXT("Fade-out duration"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_mix_settings"),
		TEXT("Get EQ bands, class adjusters, and timing from a USoundMix asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::GetSoundMixSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundMix"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_sound_mix_settings"),
		TEXT("Set EQ settings, class adjusters, or timing on a USoundMix (partial update)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::SetSoundMixSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundMix"))
			.Optional(TEXT("eq_settings"), TEXT("object"), TEXT("FAudioEQEffect fields to update"))
			.Optional(TEXT("class_effects"), TEXT("array"), TEXT("Array of FSoundClassAdjuster objects (replaces existing)"))
			.Optional(TEXT("initial_delay"), TEXT("number"), TEXT("Delay before mix takes effect"))
			.Optional(TEXT("fade_in_time"), TEXT("number"), TEXT("Fade-in duration"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Mix duration (-1 for infinite)"))
			.Optional(TEXT("fade_out_time"), TEXT("number"), TEXT("Fade-out duration"))
			.Build());

	// --- Sound Concurrency ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_concurrency"),
		TEXT("Create a new USoundConcurrency asset with optional settings"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateSoundConcurrency),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/Conc_Default)"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("FSoundConcurrencySettings fields"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_concurrency_settings"),
		TEXT("Get FSoundConcurrencySettings fields from a USoundConcurrency asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::GetConcurrencySettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundConcurrency"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_concurrency_settings"),
		TEXT("Set FSoundConcurrencySettings fields on a USoundConcurrency (partial update)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::SetConcurrencySettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundConcurrency"))
			.Required(TEXT("settings"), TEXT("object"), TEXT("FSoundConcurrencySettings fields to update"))
			.Build());

	// --- Sound Submix ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_submix"),
		TEXT("Create a new USoundSubmix asset with optional parent and effect chain"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateSoundSubmix),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/Submix_Reverb)"))
			.Optional(TEXT("parent_submix"), TEXT("string"), TEXT("Asset path of parent USoundSubmix"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_submix_properties"),
		TEXT("Get effect chain, volume, parent/children from a USoundSubmix asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::GetSubmixProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundSubmix"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_submix_properties"),
		TEXT("Set properties on a USoundSubmix (partial update)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::SetSubmixProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundSubmix"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("USoundSubmix properties to update"))
			.Build());

	// --- Test fixtures (F18) ---
	Registry.RegisterAction(TEXT("audio"), TEXT("create_test_wave"),
		TEXT("Procedurally synthesize a 16-bit mono sine-tone USoundWave (no asset deps). Useful for tests requiring a disposable wave."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioAssetActions::CreateTestWave),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Destination asset path under /Game/ (e.g. /Game/Tests/Monolith/Audio/SW_Test_Sine_440)"), { TEXT("path") })
			.Optional(TEXT("frequency_hz"), TEXT("number"), TEXT("Sine frequency in Hz (20.0 to 20000.0, default 440.0)"))
			.Optional(TEXT("duration_seconds"), TEXT("number"), TEXT("Clip length in seconds (0.05 to 5.0, default 0.5)"))
			.Optional(TEXT("sample_rate"), TEXT("integer"), TEXT("Sample rate in Hz; allowlist {22050, 44100, 48000} (default 44100)"))
			.Optional(TEXT("amplitude"), TEXT("number"), TEXT("Peak amplitude in (0.0, 1.0] (default 0.5)"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("audio"), TEXT("create_sound_attenuation"),
		{ TEXT("3d falloff"), TEXT("distance attenuation"), TEXT("spatialization settings"), TEXT("occlusion and falloff"), TEXT("how loud by distance") },
		{ TEXT("new_attenuation"), TEXT("make_attenuation"), TEXT("create_attenuation_asset") },
		{ TEXT("create a sound attenuation asset for 3d falloff"), TEXT("make an attenuation setting so sounds get quieter with distance") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("audio"), TEXT("create_sound_class"),
		{ TEXT("audio category"), TEXT("bus grouping"), TEXT("music sfx voice group"), TEXT("volume group"), TEXT("class hierarchy") },
		{ TEXT("new_sound_class"), TEXT("make_sound_class"), TEXT("create_audio_class") },
		{ TEXT("create a SoundClass for music"), TEXT("make an audio category group for sfx volume control") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("audio"), TEXT("create_sound_mix"),
		{ TEXT("ducking"), TEXT("eq settings"), TEXT("class volume adjusters"), TEXT("snapshot mix"), TEXT("lower music when dialogue plays") },
		{ TEXT("new_sound_mix"), TEXT("make_sound_mix"), TEXT("create_audio_mix") },
		{ TEXT("create a SoundMix that ducks music during dialogue"), TEXT("make a sound mix with eq and class adjusters") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("audio"), TEXT("create_sound_submix"),
		{ TEXT("effects bus"), TEXT("reverb send"), TEXT("submix effect chain"), TEXT("master bus routing"), TEXT("audio routing graph") },
		{ TEXT("new_submix"), TEXT("make_submix"), TEXT("create_audio_bus") },
		{ TEXT("create a sound submix with a reverb effect chain"), TEXT("make an effects bus to route sounds through") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("audio"), TEXT("create_sound_concurrency"),
		{ TEXT("voice limiting"), TEXT("max instances"), TEXT("stop oldest sound"), TEXT("prevent audio spam"), TEXT("polyphony cap") },
		{ TEXT("new_concurrency"), TEXT("make_concurrency"), TEXT("create_voice_limit") },
		{ TEXT("create a concurrency asset that caps a sound to 4 instances"), TEXT("limit how many copies of a sound can play at once") });
}

// ============================================================================
// Sound Attenuation
// ============================================================================

FMonolithActionResult FMonolithAudioAssetActions::CreateSoundAttenuation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundAttenuation* Asset = CreateAudioAsset<USoundAttenuationFactory, USoundAttenuation>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Apply optional initial settings
	const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
	if (Params->TryGetObjectField(TEXT("settings"), SettingsJson) && SettingsJson && SettingsJson->IsValid())
	{
		FString SetError;
		if (!JsonToStruct(*SettingsJson, FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation, SetError))
		{
			UE_LOG(LogMonolith, Warning, TEXT("create_sound_attenuation: partial settings error: %s"), *SetError);
		}
		Asset->GetPackage()->MarkPackageDirty();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::GetAttenuationSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundAttenuation* Asset = LoadAudioAsset<USoundAttenuation>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::SetAttenuationSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsJson) || !SettingsJson || !SettingsJson->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"));
	}

	FString Error;
	USoundAttenuation* Asset = LoadAudioAsset<USoundAttenuation>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	Asset->Modify();
	if (!JsonToStruct(*SettingsJson, FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	Asset->PostEditChange();
	Asset->GetPackage()->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Sound Class
// ============================================================================

FMonolithActionResult FMonolithAudioAssetActions::CreateSoundClass(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundClass* Asset = CreateAudioAsset<USoundClassFactory, USoundClass>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Set parent class if specified
	FString ParentClassPath;
	if (Params->TryGetStringField(TEXT("parent_class"), ParentClassPath) && !ParentClassPath.IsEmpty())
	{
		FString LoadError;
		USoundClass* Parent = LoadAudioAsset<USoundClass>(ParentClassPath, LoadError);
		if (Parent)
		{
#if WITH_EDITOR
			Asset->SetParentClass(Parent);
#endif
		}
		else
		{
			UE_LOG(LogMonolith, Warning, TEXT("create_sound_class: could not load parent_class '%s': %s"), *ParentClassPath, *LoadError);
		}
	}

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsJson = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson) && PropsJson && PropsJson->IsValid())
	{
		FString SetError;
		if (!JsonToStruct(*PropsJson, FSoundClassProperties::StaticStruct(), &Asset->Properties, SetError))
		{
			UE_LOG(LogMonolith, Warning, TEXT("create_sound_class: partial properties error: %s"), *SetError);
		}
		Asset->GetPackage()->MarkPackageDirty();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_class"), Asset->ParentClass ? Asset->ParentClass->GetPathName() : TEXT("None"));
	Result->SetObjectField(TEXT("properties"), StructToJson(FSoundClassProperties::StaticStruct(), &Asset->Properties));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::GetSoundClassProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundClass* Asset = LoadAudioAsset<USoundClass>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_class"), Asset->ParentClass ? Asset->ParentClass->GetPathName() : TEXT("None"));

	// Child classes
	TArray<TSharedPtr<FJsonValue>> ChildArray;
	ChildArray.Reserve(Asset->ChildClasses.Num());
	for (USoundClass* Child : Asset->ChildClasses)
	{
		if (Child)
		{
			ChildArray.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("child_classes"), ChildArray);

	Result->SetObjectField(TEXT("properties"), StructToJson(FSoundClassProperties::StaticStruct(), &Asset->Properties));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::SetSoundClassProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundClass* Asset = LoadAudioAsset<USoundClass>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	Asset->Modify();

	// Reparent if requested
	FString ParentClassPath;
	if (Params->TryGetStringField(TEXT("parent_class"), ParentClassPath))
	{
		if (ParentClassPath.IsEmpty() || ParentClassPath == TEXT("None"))
		{
#if WITH_EDITOR
			Asset->SetParentClass(nullptr);
#endif
		}
		else
		{
			FString LoadError;
			USoundClass* Parent = LoadAudioAsset<USoundClass>(ParentClassPath, LoadError);
			if (Parent)
			{
#if WITH_EDITOR
				Asset->SetParentClass(Parent);
#endif
			}
			else
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load parent_class '%s': %s"), *ParentClassPath, *LoadError));
			}
		}
	}

	// Apply properties
	const TSharedPtr<FJsonObject>* PropsJson = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson) && PropsJson && PropsJson->IsValid())
	{
		if (!JsonToStruct(*PropsJson, FSoundClassProperties::StaticStruct(), &Asset->Properties, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	Asset->PostEditChange();
	Asset->GetPackage()->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_class"), Asset->ParentClass ? Asset->ParentClass->GetPathName() : TEXT("None"));
	Result->SetObjectField(TEXT("properties"), StructToJson(FSoundClassProperties::StaticStruct(), &Asset->Properties));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Sound Mix
// ============================================================================

static TSharedPtr<FJsonObject> SoundClassAdjusterToJson(const FSoundClassAdjuster& Adjuster)
{
	auto Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("SoundClass"), Adjuster.SoundClassObject ? Adjuster.SoundClassObject->GetPathName() : TEXT("None"));
	Json->SetNumberField(TEXT("VolumeAdjuster"), Adjuster.VolumeAdjuster);
	Json->SetNumberField(TEXT("PitchAdjuster"), Adjuster.PitchAdjuster);
	Json->SetNumberField(TEXT("LowPassFilterFrequency"), Adjuster.LowPassFilterFrequency);
	Json->SetBoolField(TEXT("bApplyToChildren"), Adjuster.bApplyToChildren);
	return Json;
}

static bool JsonToSoundClassAdjuster(const TSharedPtr<FJsonObject>& Json, FSoundClassAdjuster& OutAdjuster, FString& OutError)
{
	if (!Json.IsValid())
	{
		OutError = TEXT("Null class adjuster object");
		return false;
	}

	FString ClassPath;
	if (Json->TryGetStringField(TEXT("SoundClass"), ClassPath) && !ClassPath.IsEmpty() && ClassPath != TEXT("None"))
	{
		UObject* Loaded = StaticLoadObject(USoundClass::StaticClass(), nullptr, *ClassPath);
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Could not load SoundClass '%s'"), *ClassPath);
			return false;
		}
		OutAdjuster.SoundClassObject = Cast<USoundClass>(Loaded);
	}

	double DVal;
	if (Json->HasField(TEXT("VolumeAdjuster")))
	{
		if (!Json->TryGetNumberField(TEXT("VolumeAdjuster"), DVal))
		{
			OutError = TEXT("Malformed parameter: VolumeAdjuster must be a number");
			return false;
		}
		OutAdjuster.VolumeAdjuster = static_cast<float>(DVal);
	}
	if (Json->HasField(TEXT("PitchAdjuster")))
	{
		if (!Json->TryGetNumberField(TEXT("PitchAdjuster"), DVal))
		{
			OutError = TEXT("Malformed parameter: PitchAdjuster must be a number");
			return false;
		}
		OutAdjuster.PitchAdjuster = static_cast<float>(DVal);
	}
	if (Json->HasField(TEXT("LowPassFilterFrequency")))
	{
		if (!Json->TryGetNumberField(TEXT("LowPassFilterFrequency"), DVal))
		{
			OutError = TEXT("Malformed parameter: LowPassFilterFrequency must be a number");
			return false;
		}
		OutAdjuster.LowPassFilterFrequency = static_cast<float>(DVal);
	}

	bool bVal;
	if (Json->HasField(TEXT("bApplyToChildren")))
	{
		if (!Json->TryGetBoolField(TEXT("bApplyToChildren"), bVal))
		{
			OutError = TEXT("Malformed parameter: bApplyToChildren must be a boolean");
			return false;
		}
		OutAdjuster.bApplyToChildren = bVal;
	}

	return true;
}

FMonolithActionResult FMonolithAudioAssetActions::CreateSoundMix(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// 1. Pre-validate timing fields
	bool bHasInitialDelay = false, bHasFadeIn = false, bHasDuration = false, bHasFadeOut = false;
	double InitialDelayVal = 0, FadeInVal = 0, DurationVal = 0, FadeOutVal = 0;
	if (Params->HasField(TEXT("initial_delay")))
	{
		if (!Params->TryGetNumberField(TEXT("initial_delay"), InitialDelayVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: initial_delay must be a number"));
		bHasInitialDelay = true;
	}
	if (Params->HasField(TEXT("fade_in_time")))
	{
		if (!Params->TryGetNumberField(TEXT("fade_in_time"), FadeInVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: fade_in_time must be a number"));
		bHasFadeIn = true;
	}
	if (Params->HasField(TEXT("duration")))
	{
		if (!Params->TryGetNumberField(TEXT("duration"), DurationVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: duration must be a number"));
		bHasDuration = true;
	}
	if (Params->HasField(TEXT("fade_out_time")))
	{
		if (!Params->TryGetNumberField(TEXT("fade_out_time"), FadeOutVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: fade_out_time must be a number"));
		bHasFadeOut = true;
	}

	// 2. Pre-validate class effects
	bool bHasEffects = false;
	TArray<FSoundClassAdjuster> ParsedEffects;
	const TArray<TSharedPtr<FJsonValue>>* ClassEffectsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("class_effects"), ClassEffectsArr) && ClassEffectsArr)
	{
		bHasEffects = true;
		for (const auto& Val : *ClassEffectsArr)
		{
			const TSharedPtr<FJsonObject>* AdjusterObj = nullptr;
			if (Val->TryGetObject(AdjusterObj) && AdjusterObj)
			{
				FSoundClassAdjuster Adjuster;
				FString AdjError;
				if (!JsonToSoundClassAdjuster(*AdjusterObj, Adjuster, AdjError))
				{
					return FMonolithActionResult::Error(FString::Printf(TEXT("class_effects error: %s"), *AdjError));
				}
				ParsedEffects.Add(Adjuster);
			}
		}
	}

	// 3. Pre-validate EQ settings
	bool bHasEq = false;
	FMonolithAudioEQSettingsDraft ParsedEq;
	const TSharedPtr<FJsonObject>* EqJson = nullptr;
	if (Params->HasField(TEXT("eq_settings")))
	{
		if (!Params->TryGetObjectField(TEXT("eq_settings"), EqJson) || !EqJson || !EqJson->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: eq_settings must be an object"));
		}

		bHasEq = true;
		FString SetError;
		if (!JsonToAudioEQSettingsDraft(*EqJson, ParsedEq, SetError))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("eq_settings error: %s"), *SetError));
		}
	}

	// 4. Create and mutate asset only after all validation passes
	FString Error;
	USoundMix* Asset = CreateAudioAsset<USoundMixFactory, USoundMix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (bHasEq)
	{
		ApplyAudioEQSettingsDraft(ParsedEq, Asset->EQSettings);
	}

	if (bHasEffects)
	{
		Asset->SoundClassEffects = ParsedEffects;
	}

	if (bHasInitialDelay) Asset->InitialDelay = static_cast<float>(InitialDelayVal);
	if (bHasFadeIn) Asset->FadeInTime = static_cast<float>(FadeInVal);
	if (bHasDuration) Asset->Duration = static_cast<float>(DurationVal);
	if (bHasFadeOut) Asset->FadeOutTime = static_cast<float>(FadeOutVal);

	Asset->GetPackage()->MarkPackageDirty();

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("eq_settings"), StructToJson(FAudioEQEffect::StaticStruct(), &Asset->EQSettings));

	TArray<TSharedPtr<FJsonValue>> AdjustersArr;
	AdjustersArr.Reserve(Asset->SoundClassEffects.Num());
	for (const FSoundClassAdjuster& Adj : Asset->SoundClassEffects)
	{
		AdjustersArr.Add(MakeShared<FJsonValueObject>(SoundClassAdjusterToJson(Adj)));
	}
	Result->SetArrayField(TEXT("class_effects"), AdjustersArr);

	Result->SetNumberField(TEXT("initial_delay"), Asset->InitialDelay);
	Result->SetNumberField(TEXT("fade_in_time"), Asset->FadeInTime);
	Result->SetNumberField(TEXT("duration"), Asset->Duration);
	Result->SetNumberField(TEXT("fade_out_time"), Asset->FadeOutTime);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::GetSoundMixSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundMix* Asset = LoadAudioAsset<USoundMix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("eq_settings"), StructToJson(FAudioEQEffect::StaticStruct(), &Asset->EQSettings));

	TArray<TSharedPtr<FJsonValue>> AdjustersArr;
	AdjustersArr.Reserve(Asset->SoundClassEffects.Num());
	for (const FSoundClassAdjuster& Adj : Asset->SoundClassEffects)
	{
		AdjustersArr.Add(MakeShared<FJsonValueObject>(SoundClassAdjusterToJson(Adj)));
	}
	Result->SetArrayField(TEXT("class_effects"), AdjustersArr);

	Result->SetNumberField(TEXT("initial_delay"), Asset->InitialDelay);
	Result->SetNumberField(TEXT("fade_in_time"), Asset->FadeInTime);
	Result->SetNumberField(TEXT("duration"), Asset->Duration);
	Result->SetNumberField(TEXT("fade_out_time"), Asset->FadeOutTime);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::SetSoundMixSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundMix* Asset = LoadAudioAsset<USoundMix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	// 1. Pre-validate timing fields
	bool bHasInitialDelay = false, bHasFadeIn = false, bHasDuration = false, bHasFadeOut = false;
	double InitialDelayVal = 0, FadeInVal = 0, DurationVal = 0, FadeOutVal = 0;
	if (Params->HasField(TEXT("initial_delay")))
	{
		if (!Params->TryGetNumberField(TEXT("initial_delay"), InitialDelayVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: initial_delay must be a number"));
		bHasInitialDelay = true;
	}
	if (Params->HasField(TEXT("fade_in_time")))
	{
		if (!Params->TryGetNumberField(TEXT("fade_in_time"), FadeInVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: fade_in_time must be a number"));
		bHasFadeIn = true;
	}
	if (Params->HasField(TEXT("duration")))
	{
		if (!Params->TryGetNumberField(TEXT("duration"), DurationVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: duration must be a number"));
		bHasDuration = true;
	}
	if (Params->HasField(TEXT("fade_out_time")))
	{
		if (!Params->TryGetNumberField(TEXT("fade_out_time"), FadeOutVal)) return FMonolithActionResult::Error(TEXT("Malformed parameter: fade_out_time must be a number"));
		bHasFadeOut = true;
	}

	// 2. Pre-validate class effects
	bool bHasEffects = false;
	TArray<FSoundClassAdjuster> ParsedEffects;
	const TArray<TSharedPtr<FJsonValue>>* ClassEffectsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("class_effects"), ClassEffectsArr) && ClassEffectsArr)
	{
		bHasEffects = true;
		for (const auto& Val : *ClassEffectsArr)
		{
			const TSharedPtr<FJsonObject>* AdjusterObj = nullptr;
			if (Val->TryGetObject(AdjusterObj) && AdjusterObj)
			{
				FSoundClassAdjuster Adjuster;
				FString AdjError;
				if (!JsonToSoundClassAdjuster(*AdjusterObj, Adjuster, AdjError))
				{
					return FMonolithActionResult::Error(FString::Printf(TEXT("class_effects error: %s"), *AdjError));
				}
				ParsedEffects.Add(Adjuster);
			}
		}
	}

	// 3. Pre-validate EQ settings
	bool bHasEq = false;
	FMonolithAudioEQSettingsDraft ParsedEq;
	const TSharedPtr<FJsonObject>* EqJson = nullptr;
	if (Params->HasField(TEXT("eq_settings")))
	{
		if (!Params->TryGetObjectField(TEXT("eq_settings"), EqJson) || !EqJson || !EqJson->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Malformed parameter: eq_settings must be an object"));
		}

		bHasEq = true;
		FString SetError;
		if (!JsonToAudioEQSettingsDraft(*EqJson, ParsedEq, SetError))
		{
			return FMonolithActionResult::Error(SetError);
		}
	}

	// 4. Mutate asset only after all validation passes
	Asset->Modify();

	if (bHasEq)
	{
		ApplyAudioEQSettingsDraft(ParsedEq, Asset->EQSettings);
	}

	if (bHasEffects)
	{
		Asset->SoundClassEffects = ParsedEffects;
	}

	if (bHasInitialDelay) Asset->InitialDelay = static_cast<float>(InitialDelayVal);
	if (bHasFadeIn) Asset->FadeInTime = static_cast<float>(FadeInVal);
	if (bHasDuration) Asset->Duration = static_cast<float>(DurationVal);
	if (bHasFadeOut) Asset->FadeOutTime = static_cast<float>(FadeOutVal);

	Asset->PostEditChange();
	Asset->GetPackage()->MarkPackageDirty();

	// Return updated state
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("eq_settings"), StructToJson(FAudioEQEffect::StaticStruct(), &Asset->EQSettings));

	TArray<TSharedPtr<FJsonValue>> AdjustersArr;
	AdjustersArr.Reserve(Asset->SoundClassEffects.Num());
	for (const FSoundClassAdjuster& Adj : Asset->SoundClassEffects)
	{
		AdjustersArr.Add(MakeShared<FJsonValueObject>(SoundClassAdjusterToJson(Adj)));
	}
	Result->SetArrayField(TEXT("class_effects"), AdjustersArr);

	Result->SetNumberField(TEXT("initial_delay"), Asset->InitialDelay);
	Result->SetNumberField(TEXT("fade_in_time"), Asset->FadeInTime);
	Result->SetNumberField(TEXT("duration"), Asset->Duration);
	Result->SetNumberField(TEXT("fade_out_time"), Asset->FadeOutTime);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Sound Concurrency
// ============================================================================

FMonolithActionResult FMonolithAudioAssetActions::CreateSoundConcurrency(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundConcurrency* Asset = CreateAudioAsset<USoundConcurrencyFactory, USoundConcurrency>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Apply optional settings
	const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
	if (Params->TryGetObjectField(TEXT("settings"), SettingsJson) && SettingsJson && SettingsJson->IsValid())
	{
		FString SetError;
		if (!JsonToStruct(*SettingsJson, FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency, SetError))
		{
			UE_LOG(LogMonolith, Warning, TEXT("create_sound_concurrency: settings error: %s"), *SetError);
		}
		Asset->GetPackage()->MarkPackageDirty();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::GetConcurrencySettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundConcurrency* Asset = LoadAudioAsset<USoundConcurrency>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::SetConcurrencySettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsJson) || !SettingsJson || !SettingsJson->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"));
	}

	FString Error;
	USoundConcurrency* Asset = LoadAudioAsset<USoundConcurrency>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	Asset->Modify();
	if (!JsonToStruct(*SettingsJson, FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	Asset->PostEditChange();
	Asset->GetPackage()->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetObjectField(TEXT("settings"), StructToJson(FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Sound Submix
// ============================================================================

FMonolithActionResult FMonolithAudioAssetActions::CreateSoundSubmix(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundSubmix* Asset = CreateAudioAsset<USoundSubmixFactory, USoundSubmix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Set parent submix if specified
	FString ParentPath;
	if (Params->TryGetStringField(TEXT("parent_submix"), ParentPath) && !ParentPath.IsEmpty())
	{
		FString LoadError;
		USoundSubmix* Parent = LoadAudioAsset<USoundSubmix>(ParentPath, LoadError);
		if (Parent)
		{
			Asset->ParentSubmix = Parent;
			// Add this as child of parent
			if (!Parent->ChildSubmixes.Contains(Asset))
			{
				Parent->ChildSubmixes.Add(Asset);
				Parent->GetPackage()->MarkPackageDirty();
			}
		}
		else
		{
			UE_LOG(LogMonolith, Warning, TEXT("create_sound_submix: could not load parent_submix '%s': %s"), *ParentPath, *LoadError);
		}
	}

	Asset->GetPackage()->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_submix"), Asset->ParentSubmix ? Asset->ParentSubmix->GetPathName() : TEXT("None"));

	// Effect chain
	TArray<TSharedPtr<FJsonValue>> EffectChainArr;
	EffectChainArr.Reserve(Asset->SubmixEffectChain.Num());
	for (const auto& Effect : Asset->SubmixEffectChain)
	{
		EffectChainArr.Add(MakeShared<FJsonValueString>(Effect ? Effect->GetPathName() : TEXT("None")));
	}
	Result->SetArrayField(TEXT("effect_chain"), EffectChainArr);

	// Children
	TArray<TSharedPtr<FJsonValue>> ChildrenArr;
	ChildrenArr.Reserve(Asset->ChildSubmixes.Num());
	for (const auto& Child : Asset->ChildSubmixes)
	{
		if (Child)
		{
			ChildrenArr.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("child_submixes"), ChildrenArr);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::GetSubmixProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundSubmix* Asset = LoadAudioAsset<USoundSubmix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_submix"), Asset->ParentSubmix ? Asset->ParentSubmix->GetPathName() : TEXT("None"));

	// Effect chain
	TArray<TSharedPtr<FJsonValue>> EffectChainArr;
	EffectChainArr.Reserve(Asset->SubmixEffectChain.Num());
	for (const auto& Effect : Asset->SubmixEffectChain)
	{
		EffectChainArr.Add(MakeShared<FJsonValueString>(Effect ? Effect->GetPathName() : TEXT("None")));
	}
	Result->SetArrayField(TEXT("effect_chain"), EffectChainArr);

	// Children
	TArray<TSharedPtr<FJsonValue>> ChildrenArr;
	ChildrenArr.Reserve(Asset->ChildSubmixes.Num());
	for (const auto& Child : Asset->ChildSubmixes)
	{
		if (Child)
		{
			ChildrenArr.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("child_submixes"), ChildrenArr);

	// Output volume
	Result->SetNumberField(TEXT("output_volume_db"), Asset->OutputVolumeModulation.Value);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioAssetActions::SetSubmixProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TSharedPtr<FJsonObject>* PropsJson = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsJson) || !PropsJson || !PropsJson->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("properties object is required"));
	}

	FString Error;
	USoundSubmix* Asset = LoadAudioAsset<USoundSubmix>(AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	Asset->Modify();

	// Parent submix
	FString ParentPath;
	if ((*PropsJson)->TryGetStringField(TEXT("parent_submix"), ParentPath))
	{
		if (ParentPath.IsEmpty() || ParentPath == TEXT("None"))
		{
			// Remove from current parent's children
			if (Asset->ParentSubmix)
			{
				Asset->ParentSubmix->ChildSubmixes.Remove(Asset);
				Asset->ParentSubmix->GetPackage()->MarkPackageDirty();
			}
			Asset->ParentSubmix = nullptr;
		}
		else
		{
			FString LoadError;
			USoundSubmix* NewParent = LoadAudioAsset<USoundSubmix>(ParentPath, LoadError);
			if (NewParent)
			{
				// Remove from old parent
				if (Asset->ParentSubmix && Asset->ParentSubmix != NewParent)
				{
					Asset->ParentSubmix->ChildSubmixes.Remove(Asset);
					Asset->ParentSubmix->GetPackage()->MarkPackageDirty();
				}
				Asset->ParentSubmix = NewParent;
				if (!NewParent->ChildSubmixes.Contains(Asset))
				{
					NewParent->ChildSubmixes.Add(Asset);
					NewParent->GetPackage()->MarkPackageDirty();
				}
			}
			else
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load parent_submix '%s': %s"), *ParentPath, *LoadError));
			}
		}
	}

	// Output volume (modulation destination — set the Value field).
	// Accept BOTH `output_volume_db` (canonical) and `output_volume` (legacy synonym).
	// K2 alias rewriter is top-level only and does NOT recurse into the nested
	// `properties` object, so we accept both inline here.
	bool bRequestedOutputVolume = false;
	double RequestedOutputVolume = 0.0;
	bool bUsedLegacyKey = false;
	bool bCollision = false;

	{
		const bool bHasCanonical = (*PropsJson)->HasField(TEXT("output_volume_db"));
		const bool bHasLegacy = (*PropsJson)->HasField(TEXT("output_volume"));
		if (bHasCanonical && bHasLegacy)
		{
			bCollision = true;
		}
		else if (bHasCanonical)
		{
			if (!(*PropsJson)->TryGetNumberField(TEXT("output_volume_db"), RequestedOutputVolume))
			{
				return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'output_volume_db'. Expected number."));
			}
			bRequestedOutputVolume = true;
		}
		else if (bHasLegacy)
		{
			if (!(*PropsJson)->TryGetNumberField(TEXT("output_volume"), RequestedOutputVolume))
			{
				return FMonolithActionResult::Error(TEXT("Invalid type for parameter 'output_volume'. Expected number."));
			}
			bRequestedOutputVolume = true;
			bUsedLegacyKey = true;
		}
	}

	if (bCollision)
	{
		return FMonolithActionResult::Error(
			TEXT("Param collision in properties: both 'output_volume_db' (canonical) and 'output_volume' (legacy) supplied. Use only one."));
	}

	if (bRequestedOutputVolume)
	{
		Asset->OutputVolumeModulation.Value = static_cast<float>(RequestedOutputVolume);
	}

	Asset->PostEditChange();
	Asset->GetPackage()->MarkPackageDirty();

	// Return updated state
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Result->SetStringField(TEXT("parent_submix"), Asset->ParentSubmix ? Asset->ParentSubmix->GetPathName() : TEXT("None"));

	TArray<TSharedPtr<FJsonValue>> EffectChainArr;
	EffectChainArr.Reserve(Asset->SubmixEffectChain.Num());
	for (const auto& Effect : Asset->SubmixEffectChain)
	{
		EffectChainArr.Add(MakeShared<FJsonValueString>(Effect ? Effect->GetPathName() : TEXT("None")));
	}
	Result->SetArrayField(TEXT("effect_chain"), EffectChainArr);

	TArray<TSharedPtr<FJsonValue>> ChildrenArr;
	ChildrenArr.Reserve(Asset->ChildSubmixes.Num());
	for (const auto& Child : Asset->ChildSubmixes)
	{
		if (Child)
		{
			ChildrenArr.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("child_submixes"), ChildrenArr);

	const float ActualOutputVolume = Asset->OutputVolumeModulation.Value;
	Result->SetNumberField(TEXT("output_volume_db"), ActualOutputVolume);

	// verified_value smoking gun — readback after write so callers can confirm.
	if (bRequestedOutputVolume)
	{
		TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> OutputEntry = MakeShared<FJsonObject>();
		OutputEntry->SetNumberField(TEXT("requested"), RequestedOutputVolume);
		OutputEntry->SetNumberField(TEXT("actual"), ActualOutputVolume);
		OutputEntry->SetBoolField(TEXT("match"), FMath::IsNearlyEqual(static_cast<float>(RequestedOutputVolume), ActualOutputVolume));
		Verified->SetObjectField(TEXT("output_volume_db"), OutputEntry);
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}

	if (bUsedLegacyKey)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		WarnArr.Add(MakeShared<FJsonValueString>(
			TEXT("Deprecated: use 'output_volume_db' instead of 'output_volume' in the properties object.")));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Test fixtures (F18) — create_test_wave
// ============================================================================
//
// Procedurally synthesizes a mono 16-bit PCM sine-tone USoundWave. Zero on-disk
// audio dependencies — entirely deterministic, reproducible across runs. Used
// by J3 TC3.19 (USoundWave direct binding) and any future test that needs a
// disposable wave fixture.
//
// Recipe (per Docs/Research/2026-04-26-j-misc-drift-findings.md §B.3):
//   1. Validate path under /Game/ and all numeric params.
//   2. Generate int16 PCM samples with linear fade-in/out (avoid click artifact).
//   3. Build full RIFF/WAVE blob in memory (44-byte header + PCM payload).
//   4. NewObject<USoundWave> in destination package.
//   5. Set NumChannels / SetSampleRate / Duration / TotalSamples (UE 5.7 public API).
//   6. RawData.Lock(LOCK_READ_WRITE) -> Realloc -> Memcpy -> Unlock (canonical
//      FByteBulkData write pattern matching engine SoundFactory::FactoryCreateBinary).
//   7. InvalidateCompressedData(true) so the cooker re-cooks if needed.
//   8. SavePackage + AssetRegistry::AssetCreated.

namespace
{
	/** Append a little-endian 32-bit value to a byte buffer. */
	FORCEINLINE void AppendLE32(TArray<uint8>& Buf, uint32 Value)
	{
		Buf.Add(static_cast<uint8>(Value & 0xFF));
		Buf.Add(static_cast<uint8>((Value >> 8) & 0xFF));
		Buf.Add(static_cast<uint8>((Value >> 16) & 0xFF));
		Buf.Add(static_cast<uint8>((Value >> 24) & 0xFF));
	}

	/** Append a little-endian 16-bit value to a byte buffer. */
	FORCEINLINE void AppendLE16(TArray<uint8>& Buf, uint16 Value)
	{
		Buf.Add(static_cast<uint8>(Value & 0xFF));
		Buf.Add(static_cast<uint8>((Value >> 8) & 0xFF));
	}

	/** Append a 4-char ASCII tag (no null terminator) to a byte buffer. */
	FORCEINLINE void AppendTag(TArray<uint8>& Buf, const char (&Tag)[5])
	{
		Buf.Add(static_cast<uint8>(Tag[0]));
		Buf.Add(static_cast<uint8>(Tag[1]));
		Buf.Add(static_cast<uint8>(Tag[2]));
		Buf.Add(static_cast<uint8>(Tag[3]));
	}
}

FMonolithActionResult FMonolithAudioAssetActions::CreateTestWave(const TSharedPtr<FJsonObject>& Params)
{
	// ---- 1. Validate path ---------------------------------------------------
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		Params->TryGetStringField(TEXT("path"), AssetPath); // Fallback for backward compatibility
	}
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("asset_path must be under /Game/ (got '%s')"), *AssetPath));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path — must contain at least one '/' (e.g. /Game/Tests/Monolith/Audio/SW_Test_Sine_440)"));
	}

	// Defensive: reject malformed paths (e.g. "//Game/...") before StaticLoadObject/CreatePackage can assert.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	// ---- 2. Validate numeric params ----------------------------------------
	double FrequencyHz = 440.0;
	double DurationSeconds = 0.5;
	int32 SampleRate = 44100;
	double Amplitude = 0.5;

	double TmpD = 0.0;
	if (Params->HasField(TEXT("frequency_hz")))
	{
		if (!Params->TryGetNumberField(TEXT("frequency_hz"), TmpD)) return FMonolithActionResult::Error(TEXT("Malformed parameter: frequency_hz must be a number"));
		FrequencyHz = TmpD;
	}
	if (Params->HasField(TEXT("duration_seconds")))
	{
		if (!Params->TryGetNumberField(TEXT("duration_seconds"), TmpD)) return FMonolithActionResult::Error(TEXT("Malformed parameter: duration_seconds must be a number"));
		DurationSeconds = TmpD;
	}
	if (Params->HasField(TEXT("amplitude")))
	{
		if (!Params->TryGetNumberField(TEXT("amplitude"), TmpD)) return FMonolithActionResult::Error(TEXT("Malformed parameter: amplitude must be a number"));
		Amplitude = TmpD;
	}
	if (Params->HasField(TEXT("sample_rate")))
	{
		if (!Params->TryGetNumberField(TEXT("sample_rate"), TmpD)) return FMonolithActionResult::Error(TEXT("Malformed parameter: sample_rate must be a number"));
		SampleRate = static_cast<int32>(TmpD);
	}

	if (FrequencyHz < 20.0 || FrequencyHz > 20000.0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("frequency_hz must be in [20.0, 20000.0] (got %.4f)"), FrequencyHz));
	}
	if (DurationSeconds < 0.05 || DurationSeconds > 5.0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("duration_seconds must be in [0.05, 5.0] (got %.4f)"), DurationSeconds));
	}
	if (Amplitude <= 0.0 || Amplitude > 1.0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("amplitude must be in (0.0, 1.0] (got %.4f)"), Amplitude));
	}
	if (SampleRate != 22050 && SampleRate != 44100 && SampleRate != 48000)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("sample_rate must be one of {22050, 44100, 48000} (got %d)"), SampleRate));
	}

	// ---- 3. Pre-existing-asset guard ---------------------------------------
	UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s' (delete first if regenerating)"), *AssetPath));
	}

	// ---- 4. Generate PCM16 sample buffer -----------------------------------
	const int32 NumChannelsMono = 1;
	const int32 BitsPerSample = 16;
	const int32 BytesPerSample = BitsPerSample / 8;

	const int64 TotalSampleCount = static_cast<int64>(FMath::FloorToInt(DurationSeconds * static_cast<double>(SampleRate)));
	if (TotalSampleCount <= 0)
	{
		return FMonolithActionResult::Error(TEXT("Computed sample count is non-positive — sample_rate × duration_seconds collapsed to zero"));
	}

	const float ActualDurationSeconds = static_cast<float>(TotalSampleCount) / static_cast<float>(SampleRate);

	TArray<int16> Pcm;
	Pcm.SetNumUninitialized(static_cast<int32>(TotalSampleCount));

	const double TwoPiF = 2.0 * PI * FrequencyHz;
	const double InvSampleRate = 1.0 / static_cast<double>(SampleRate);
	const double Scale = Amplitude * 32767.0;

	const int64 FadeSamples = FMath::Min<int64>(256, TotalSampleCount / 2);
	const int64 FadeOutStart = TotalSampleCount - FadeSamples;

	for (int64 i = 0; i < TotalSampleCount; ++i)
	{
		const double t = static_cast<double>(i) * InvSampleRate;
		double Sample = Scale * FMath::Sin(TwoPiF * t);

		// Linear fade-in / fade-out (256 samples each end) to avoid click.
		if (FadeSamples > 0)
		{
			if (i < FadeSamples)
			{
				Sample *= static_cast<double>(i) / static_cast<double>(FadeSamples);
			}
			else if (i >= FadeOutStart)
			{
				const double Fade = static_cast<double>(TotalSampleCount - 1 - i) / static_cast<double>(FadeSamples);
				Sample *= FMath::Max(0.0, Fade);
			}
		}

		// Clamp into int16 range with rounding.
		const double Rounded = FMath::Clamp(Sample, -32768.0, 32767.0);
		Pcm[static_cast<int32>(i)] = static_cast<int16>(FMath::RoundToInt(Rounded));
	}

	// ---- 5. Build full RIFF/WAVE byte blob ---------------------------------
	// Standard 44-byte canonical header + PCM data. Layout:
	//   "RIFF" <chunk_size:le32> "WAVE"
	//   "fmt " <subchunk_size:le32=16> <fmt:le16=1=PCM> <channels:le16>
	//   <sample_rate:le32> <byte_rate:le32> <block_align:le16> <bits_per_sample:le16>
	//   "data" <pcm_size:le32> <pcm bytes...>

	const uint32 PcmByteCount = static_cast<uint32>(TotalSampleCount * BytesPerSample);
	const uint32 ByteRate = static_cast<uint32>(SampleRate * NumChannelsMono * BytesPerSample);
	const uint16 BlockAlign = static_cast<uint16>(NumChannelsMono * BytesPerSample);
	const uint32 ChunkSize = 36 + PcmByteCount; // 4 ("WAVE") + 24 (fmt subchunk total) + 8 (data hdr) + payload

	TArray<uint8> WavBlob;
	WavBlob.Reserve(44 + PcmByteCount);

	AppendTag(WavBlob, "RIFF");
	AppendLE32(WavBlob, ChunkSize);
	AppendTag(WavBlob, "WAVE");

	AppendTag(WavBlob, "fmt ");
	AppendLE32(WavBlob, 16);                               // PCM subchunk size
	AppendLE16(WavBlob, 1);                                // AudioFormat = 1 (linear PCM)
	AppendLE16(WavBlob, static_cast<uint16>(NumChannelsMono));
	AppendLE32(WavBlob, static_cast<uint32>(SampleRate));
	AppendLE32(WavBlob, ByteRate);
	AppendLE16(WavBlob, BlockAlign);
	AppendLE16(WavBlob, static_cast<uint16>(BitsPerSample));

	AppendTag(WavBlob, "data");
	AppendLE32(WavBlob, PcmByteCount);

	const int32 PayloadStart = WavBlob.Num();
	WavBlob.AddUninitialized(static_cast<int32>(PcmByteCount));
	FMemory::Memcpy(WavBlob.GetData() + PayloadStart, Pcm.GetData(), PcmByteCount);

	// ---- 6. Create package + USoundWave ------------------------------------
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	USoundWave* Wave = NewObject<USoundWave>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Wave)
	{
		return FMonolithActionResult::Error(TEXT("NewObject<USoundWave> returned null"));
	}

	// ---- 7. Set USoundWave properties (UE 5.7 public API) ------------------
	Wave->NumChannels = NumChannelsMono;
#if WITH_EDITOR
	Wave->SetSampleRate(static_cast<uint32>(SampleRate));
#else
	// Headless / non-editor — fall back to direct field set (test fixtures should not run in shipping anyway).
	Wave->SampleRate = SampleRate;
#endif
	Wave->Duration = ActualDurationSeconds;
	Wave->TotalSamples = static_cast<int32>(TotalSampleCount);
	Wave->SoundGroup = SOUNDGROUP_Default;

	// ---- 8. Write WAV blob into RawData (FEditorAudioBulkData, UE 5.7) ------
	// UE 5.4+ replaced the legacy FByteBulkData Lock/Realloc/Unlock idiom with
	// FEditorAudioBulkData::UpdatePayload(FSharedBuffer, UObject* Owner).
	// FSharedBuffer::Clone copies the bytes — caller-owned WavBlob can drop afterwards.
#if WITH_EDITOR
	FSharedBuffer WaveBuffer = FSharedBuffer::Clone(WavBlob.GetData(), WavBlob.Num());
	Wave->RawData.UpdatePayload(WaveBuffer, Wave);

	// Force the cooker to re-cook from the new RawData.
	Wave->InvalidateCompressedData(true, false);
#endif

	// ---- 9. Asset Registry + save -----------------------------------------
	FAssetRegistryModule::AssetCreated(Wave);
	Pkg->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Pkg, Wave, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to save package to '%s'"), *PackageFilename));
	}

	// ---- 10. Result --------------------------------------------------------
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Wave->GetPathName());
	Result->SetNumberField(TEXT("samples_written"), static_cast<double>(TotalSampleCount));
	Result->SetNumberField(TEXT("duration_actual_seconds"), ActualDurationSeconds);
	Result->SetNumberField(TEXT("frequency_hz"), FrequencyHz);
	Result->SetNumberField(TEXT("sample_rate"), SampleRate);
	Result->SetNumberField(TEXT("amplitude"), Amplitude);
	return FMonolithActionResult::Success(Result);
}
