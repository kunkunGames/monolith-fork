#include "MonolithAssetInspectionActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Modules/ModuleManager.h"
#include "UObject/MetaData.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/MonolithAssetInspectionTestHooks.h"
#endif

namespace
{
	struct FAssetEnricherDef
	{
		const TCHAR* Key;
		const TCHAR* DisplayName;
		TArray<const TCHAR*> ClassNames;
		bool bLargePayload = false;
	};

	const TArray<FAssetEnricherDef>& GetSupportedEnrichers()
	{
		static const TArray<FAssetEnricherDef> Defs = {
			{ TEXT("dialogue_voice"), TEXT("Dialogue Voice"), { TEXT("DialogueVoice") }, false },
			{ TEXT("dialogue_wave"), TEXT("Dialogue Wave"), { TEXT("DialogueWave") }, false },
			{ TEXT("foliage_type"), TEXT("Foliage Type"), { TEXT("FoliageType"), TEXT("FoliageType_InstancedStaticMesh"), TEXT("FoliageType_Actor") }, true },
			{ TEXT("landscape_grass_type"), TEXT("Landscape Grass Type"), { TEXT("LandscapeGrassType") }, true },
			{ TEXT("runtime_virtual_texture"), TEXT("Runtime Virtual Texture"), { TEXT("RuntimeVirtualTexture") }, false },
			{ TEXT("texture2d"), TEXT("Texture2D"), { TEXT("Texture2D") }, false },
			{ TEXT("physical_material"), TEXT("Physical Material"), { TEXT("PhysicalMaterial") }, false },
			{ TEXT("physics_asset"), TEXT("Physics Asset"), { TEXT("PhysicsAsset") }, true },
			{ TEXT("curve"), TEXT("Curve"), { TEXT("CurveFloat"), TEXT("CurveVector"), TEXT("CurveLinearColor"), TEXT("CurveBase") }, true },
			{ TEXT("curve_table"), TEXT("Curve Table"), { TEXT("CurveTable") }, true },
			{ TEXT("material_parameter_collection"), TEXT("Material Parameter Collection"), { TEXT("MaterialParameterCollection") }, false }
		};
		return Defs;
	}

	const UClass* GetNativeClass(const UObject* Asset)
	{
		if (!Asset)
		{
			return nullptr;
		}

		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			if (Blueprint->GeneratedClass)
			{
				return Blueprint->GeneratedClass;
			}
		}

		return Asset->GetClass();
	}

	bool ClassMatchesCandidate(const UClass* Class, const TCHAR* Candidate)
	{
		for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			if (Current->GetName().Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	const FAssetEnricherDef* FindEnricher(const UObject* Asset)
	{
		const UClass* Class = GetNativeClass(Asset);
		for (const FAssetEnricherDef& Def : GetSupportedEnrichers())
		{
			for (const TCHAR* Candidate : Def.ClassNames)
			{
				if (ClassMatchesCandidate(Class, Candidate))
				{
					return &Def;
				}
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> ObjectRefToJson(const UObject* Obj)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetBoolField(TEXT("loaded"), Obj != nullptr);
		if (Obj)
		{
			Ref->SetStringField(TEXT("name"), Obj->GetName());
			Ref->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
			Ref->SetStringField(TEXT("path"), Obj->GetPathName());
		}
		return Ref;
	}

	bool DoesSoftReferenceExist(const FSoftObjectPath& Path)
	{
		if (!Path.IsValid())
		{
			return false;
		}

		if (Path.ResolveObject())
		{
			return true;
		}

		const FString AssetPath = Path.GetAssetPathString();

		// /Script exports are native and created with their module. ResolveObject
		// above already covers the loaded case, so only an unloadable module is
		// indeterminate here rather than automatically valid.
		if (AssetPath.StartsWith(TEXT("/Script/"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		// A soft path naming a missing subobject, such as /Game/Pkg.Asset:Missing,
		// fails ResolveObject while its owning asset still has a registry row.
		// Treating the row as sufficient reported an unresolvable path as existing.
		if (!Path.GetSubPathString().IsEmpty())
		{
			return false;
		}

		// Only namespaces that genuinely cannot be queried are inherently valid.
		// Plugin and /Engine mounts are ordinary asset paths, so hard-coding them
		// as existing hid every missing reference outside /Game.
		return FMonolithAssetUtils::AssetExists(AssetPath);
	}

	TSharedPtr<FJsonObject> SoftObjectRefToJson(const FSoftObjectPath& Path)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("path"), Path.ToString());
		Ref->SetStringField(TEXT("asset_path"), Path.GetAssetPathString());
		Ref->SetBoolField(TEXT("valid"), Path.IsValid());
		Ref->SetBoolField(TEXT("exists"), DoesSoftReferenceExist(Path));
		return Ref;
	}

	FString ExportPropertyValue(const FProperty* Property, const void* ValuePtr)
	{
		if (!Property || !ValuePtr)
		{
			return FString();
		}

		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return Exported;
	}

	TSharedPtr<FJsonObject> SerializeStructFields(const UStruct* Struct, const void* StructMemory, int32 Depth, int32 ArrayLimit);

	TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Property, const void* ValuePtr, int32 Depth, int32 ArrayLimit)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}

		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			const int64 RawValue = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(EnumProp->GetEnum()
				? EnumProp->GetEnum()->GetNameStringByValue(RawValue)
				: FString::FromInt(RawValue));
		}

		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			const uint8 RawValue = ByteProp->GetPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(ByteProp->Enum
				? ByteProp->Enum->GetNameStringByValue(RawValue)
				: FString::FromInt(RawValue));
		}

		if (const FNumericProperty* NumericProp = CastField<FNumericProperty>(Property))
		{
			if (NumericProp->IsInteger())
			{
				const bool bUnsigned =
					CastField<FUInt16Property>(Property) ||
					CastField<FUInt32Property>(Property) ||
					CastField<FUInt64Property>(Property);
				return MakeShared<FJsonValueNumber>(bUnsigned
					? static_cast<double>(NumericProp->GetUnsignedIntPropertyValue(ValuePtr))
					: static_cast<double>(NumericProp->GetSignedIntPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(NumericProp->GetFloatingPointPropertyValue(ValuePtr));
		}

		if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}

		if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}

		if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
		}

		if (const FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Property))
		{
			return MakeShared<FJsonValueObject>(SoftObjectRefToJson(SoftObjectProp->GetPropertyValue(ValuePtr).ToSoftObjectPath()));
		}

		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
		{
			return MakeShared<FJsonValueObject>(ObjectRefToJson(ObjectProp->GetObjectPropertyValue(ValuePtr)));
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (Depth <= 0)
			{
				return MakeShared<FJsonValueString>(ExportPropertyValue(Property, ValuePtr));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("script_struct"), StructProp->Struct ? StructProp->Struct->GetName() : FString());
			Obj->SetObjectField(TEXT("fields"), SerializeStructFields(StructProp->Struct, ValuePtr, Depth - 1, ArrayLimit));
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			const int32 Count = Helper.Num();
			const int32 Limit = FMath::Clamp(ArrayLimit, 0, Count);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Limit);
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				Items.Add(PropertyValueToJson(ArrayProp->Inner, Helper.GetRawPtr(Index), Depth - 1, ArrayLimit));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("count"), Count);
			Obj->SetArrayField(TEXT("items"), Items);
			if (Limit < Count)
			{
				Obj->SetNumberField(TEXT("truncated_after"), Limit);
			}
			return MakeShared<FJsonValueObject>(Obj);
		}

		return MakeShared<FJsonValueString>(ExportPropertyValue(Property, ValuePtr));
	}

	TSharedPtr<FJsonObject> SerializeStructFields(const UStruct* Struct, const void* StructMemory, int32 Depth, int32 ArrayLimit)
	{
		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (!Struct || !StructMemory)
		{
			return Fields;
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructMemory);
			Fields->SetField(Property->GetName(), PropertyValueToJson(Property, ValuePtr, Depth, ArrayLimit));
		}

		return Fields;
	}

	void CollectReferencesFromProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const FString& Source,
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		int32 Depth,
		int32 ArrayLimit);

	void AddReference(TArray<TSharedPtr<FJsonValue>>& OutRefs, TSet<FString>& Seen, const FString& Source, const FString& Path, const FString& ClassName, bool bLoaded, bool bSoft)
	{
		if (Path.IsEmpty())
		{
			return;
		}

		const FString Key = Source + TEXT("|") + Path;
		if (Seen.Contains(Key))
		{
			return;
		}
		Seen.Add(Key);

		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("source"), Source);
		Ref->SetStringField(TEXT("path"), Path);
		Ref->SetStringField(TEXT("class"), ClassName);
		Ref->SetBoolField(TEXT("loaded"), bLoaded);
		Ref->SetBoolField(TEXT("soft"), bSoft);
		if (bSoft)
		{
			const FSoftObjectPath SoftPath(Path);
			const FString AssetPath = SoftPath.GetAssetPathString();
			Ref->SetStringField(TEXT("asset_path"), AssetPath);
			Ref->SetBoolField(TEXT("exists"), DoesSoftReferenceExist(SoftPath));
		}
		OutRefs.Add(MakeShared<FJsonValueObject>(Ref));
	}

	void CollectReferencesFromStruct(const UStruct* Struct, const void* StructMemory, const FString& Source, TArray<TSharedPtr<FJsonValue>>& OutRefs, TSet<FString>& Seen, int32 Depth, int32 ArrayLimit)
	{
		if (!Struct || !StructMemory || Depth < 0)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			CollectReferencesFromProperty(Property, Property->ContainerPtrToValuePtr<void>(StructMemory), Source, OutRefs, Seen, Depth, ArrayLimit);
		}
	}

	void CollectReferencesFromProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const FString& Source,
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		int32 Depth,
		int32 ArrayLimit)
	{
		if (!Property || !ValuePtr || Depth < 0)
		{
			return;
		}

		if (const FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath Path = SoftObjectProp->GetPropertyValue(ValuePtr).ToSoftObjectPath();
			if (Path.IsValid())
			{
				AddReference(OutRefs, Seen, Source + TEXT(".") + Property->GetName(), Path.ToString(), TEXT("soft_object"), false, true);
			}
			return;
		}

		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
		{
			if (const UObject* Obj = ObjectProp->GetObjectPropertyValue(ValuePtr))
			{
				AddReference(OutRefs, Seen, Source + TEXT(".") + Property->GetName(), Obj->GetPathName(), Obj->GetClass()->GetName(), true, false);
			}
			return;
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			CollectReferencesFromStruct(StructProp->Struct, ValuePtr, Source + TEXT(".") + Property->GetName(), OutRefs, Seen, Depth - 1, ArrayLimit);
			return;
		}

		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			const int32 Limit = FMath::Clamp(ArrayLimit, 0, Helper.Num());
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				CollectReferencesFromProperty(
					ArrayProp->Inner,
					Helper.GetRawPtr(Index),
					FString::Printf(TEXT("%s.%s[%d]"), *Source, *Property->GetName(), Index),
					OutRefs,
					Seen,
					Depth - 1,
					ArrayLimit);
			}
			return;
		}

		// TSet and TMap were never visited, so hard and soft references stored in
		// them were absent from inspect_asset(include_references=true) and could
		// not raise unresolved_soft_reference. They use the same depth and
		// item-count safeguards as the array case.
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProp, ValuePtr);
			int32 Visited = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (Visited >= ArrayLimit)
				{
					break;
				}
				++Visited;
				CollectReferencesFromProperty(
					SetProp->ElementProp,
					Helper.GetElementPtr(Index),
					FString::Printf(TEXT("%s.%s{%d}"), *Source, *Property->GetName(), Index),
					OutRefs,
					Seen,
					Depth - 1,
					ArrayLimit);
			}
			return;
		}

		if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProp, ValuePtr);
			int32 Visited = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (Visited >= ArrayLimit)
				{
					break;
				}
				++Visited;
				const FString EntrySource =
					FString::Printf(TEXT("%s.%s{%d}"), *Source, *Property->GetName(), Index);
				CollectReferencesFromProperty(
					MapProp->KeyProp,
					Helper.GetKeyPtr(Index),
					EntrySource + TEXT(".key"),
					OutRefs,
					Seen,
					Depth - 1,
					ArrayLimit);
				CollectReferencesFromProperty(
					MapProp->ValueProp,
					Helper.GetValuePtr(Index),
					EntrySource + TEXT(".value"),
					OutRefs,
					Seen,
					Depth - 1,
					ArrayLimit);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> CollectAssetReferences(const UObject* Asset, int32 ArrayLimit)
	{
		TArray<TSharedPtr<FJsonValue>> Refs;
		TSet<FString> Seen;
		CollectReferencesFromStruct(Asset ? Asset->GetClass() : nullptr, Asset, TEXT("asset"), Refs, Seen, 6, ArrayLimit);
		return Refs;
	}

	TSharedPtr<FJsonObject> BuildAssetRegistryDetails(const UObject* Asset)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		if (!Asset)
		{
			return Details;
		}

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FAssetData Data = AR.GetAssetByObjectPath(FSoftObjectPath(Asset->GetPathName()));
		if (Data.IsValid())
		{
			Details->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
			Details->SetStringField(TEXT("object_path"), Data.GetObjectPathString());
			Details->SetStringField(TEXT("asset_name"), Data.AssetName.ToString());
			Details->SetStringField(TEXT("asset_class_path"), Data.AssetClassPath.ToString());

			TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
			Data.TagsAndValues.ForEach([&Tags](TPair<FName, FAssetTagValueRef> Pair)
			{
				Tags->SetStringField(Pair.Key.ToString(), Pair.Value.GetValue());
			});
			Details->SetObjectField(TEXT("tags"), Tags);
		}
		return Details;
	}

	FString EnumValueName(const UEnum* Enum, int64 Value)
	{
		return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
	}

	FString GetGeneratedTextureRole(const UTexture2D* Texture)
	{
		if (!Texture)
		{
			return FString();
		}
#if WITH_METADATA
		FMetaData& MetaData = Texture->GetOutermost()->GetMetaData();
		if (const FString* Role = MetaData.FindValue(Texture, TEXT("Monolith.Generated.texture_role")))
		{
			return *Role;
		}
#endif
		return FString();
	}

	TSharedPtr<FJsonObject> BuildTextureDetails(const UTexture2D* Texture)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		if (!Texture)
		{
			return Details;
		}

		Details->SetNumberField(TEXT("width"), Texture->GetSizeX());
		Details->SetNumberField(TEXT("height"), Texture->GetSizeY());
#if WITH_EDITOR
		Details->SetNumberField(TEXT("source_width"), Texture->Source.GetSizeX());
		Details->SetNumberField(TEXT("source_height"), Texture->Source.GetSizeY());
		Details->SetNumberField(TEXT("source_mips"), Texture->Source.GetNumMips());
#endif
		Details->SetBoolField(TEXT("srgb"), Texture->SRGB != 0);
		Details->SetStringField(TEXT("compression_settings"), EnumValueName(StaticEnum<TextureCompressionSettings>(), Texture->CompressionSettings));
		Details->SetStringField(TEXT("mip_gen_settings"), EnumValueName(StaticEnum<TextureMipGenSettings>(), Texture->MipGenSettings));
		Details->SetStringField(TEXT("lod_group"), EnumValueName(StaticEnum<TextureGroup>(), Texture->LODGroup));
		Details->SetStringField(TEXT("address_x"), EnumValueName(StaticEnum<TextureAddress>(), Texture->AddressX));
		Details->SetStringField(TEXT("address_y"), EnumValueName(StaticEnum<TextureAddress>(), Texture->AddressY));
		const FString GeneratedRole = GetGeneratedTextureRole(Texture);
		if (!GeneratedRole.IsEmpty())
		{
			Details->SetStringField(TEXT("generated_texture_role"), GeneratedRole);
		}
		return Details;
	}

	TArray<TSharedPtr<FJsonValue>> BuildValidationWarnings(
		const UObject* Asset,
		const FAssetEnricherDef* Def,
		int32 ArrayLimit,
		const TArray<TSharedPtr<FJsonValue>>* PrecomputedReferences = nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
		{
			TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("code"), Code);
			Warning->SetStringField(TEXT("message"), Message);
			Warnings.Add(MakeShared<FJsonValueObject>(Warning));
		};

		if (!Asset)
		{
			AddWarning(TEXT("asset_missing"), TEXT("Asset could not be loaded."));
			return Warnings;
		}

		if (!Def)
		{
			AddWarning(TEXT("unsupported_type"), TEXT("No typed enricher is registered for this asset class."));
			return Warnings;
		}

		if (Def->bLargePayload)
		{
			AddWarning(TEXT("large_payload_capped"), FString::Printf(TEXT("Array properties are capped at %d items by default."), ArrayLimit));
		}

		const TArray<TSharedPtr<FJsonValue>> CollectedReferences =
			PrecomputedReferences ? TArray<TSharedPtr<FJsonValue>>() : CollectAssetReferences(Asset, ArrayLimit);
		const TArray<TSharedPtr<FJsonValue>>& References = PrecomputedReferences ? *PrecomputedReferences : CollectedReferences;
		for (const TSharedPtr<FJsonValue>& RefValue : References)
		{
			const TSharedPtr<FJsonObject>* RefObject = nullptr;
			if (!RefValue.IsValid() || !RefValue->TryGetObject(RefObject) || !RefObject || !RefObject->IsValid())
			{
				continue;
			}

			bool bSoft = false;
			bool bExists = true;
			(*RefObject)->TryGetBoolField(TEXT("soft"), bSoft);
			(*RefObject)->TryGetBoolField(TEXT("exists"), bExists);
			if (bSoft && !bExists)
			{
				FString Path;
				(*RefObject)->TryGetStringField(TEXT("path"), Path);
				AddWarning(TEXT("unresolved_soft_reference"), FString::Printf(TEXT("Soft reference does not resolve: %s"), *Path));
			}
		}

		if (const UTexture2D* Texture = Cast<UTexture2D>(Asset))
		{
			const FString Role = GetGeneratedTextureRole(Texture).ToLower();
			if (Role == TEXT("normal"))
			{
				if (Texture->SRGB)
				{
					AddWarning(TEXT("normal_srgb_enabled"), TEXT("Generated normal-map textures should have sRGB disabled."));
				}
				if (Texture->CompressionSettings != TC_Normalmap)
				{
					AddWarning(TEXT("normal_wrong_compression"), TEXT("Generated normal-map textures should use TC_Normalmap compression."));
				}
			}
			else if (Role == TEXT("orm_mask") || Role == TEXT("height"))
			{
				if (Texture->SRGB)
				{
					AddWarning(TEXT("data_texture_srgb_enabled"), TEXT("Generated mask/height textures should have sRGB disabled."));
				}
				if (Role == TEXT("orm_mask") && Texture->CompressionSettings != TC_Masks)
				{
					AddWarning(TEXT("mask_wrong_compression"), TEXT("Generated ORM/mask textures should use TC_Masks compression."));
				}
			}
			else if (Role == TEXT("world_tile"))
			{
				if (Texture->AddressX != TA_Wrap || Texture->AddressY != TA_Wrap)
				{
					AddWarning(TEXT("tile_not_wrapped"), TEXT("Generated world_tile textures should use wrap addressing."));
				}
			}
			else if (Role == TEXT("ui_icon") || Role == TEXT("sprite"))
			{
				if (Texture->MipGenSettings != TMGS_NoMipmaps)
				{
					AddWarning(TEXT("ui_mips_enabled"), TEXT("Generated UI/sprite textures should normally disable mipmaps."));
				}
			}
		}

		return Warnings;
	}

	bool HasBlockingValidationWarnings(const TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		for (const TSharedPtr<FJsonValue>& WarningValue : Warnings)
		{
			const TSharedPtr<FJsonObject>* WarningObject = nullptr;
			if (!WarningValue.IsValid() || !WarningValue->TryGetObject(WarningObject) || !WarningObject || !WarningObject->IsValid())
			{
				continue;
			}

			FString Code;
			(*WarningObject)->TryGetStringField(TEXT("code"), Code);
			if (!Code.Equals(TEXT("large_payload_capped"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	UObject* LoadAssetFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		Params->TryGetStringField(TEXT("asset_path"), OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}

		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(OutAssetPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Asset not found at '%s'"), *OutAssetPath);
		}
		return Asset;
	}

	TSharedPtr<FJsonObject> InspectLoadedAsset(UObject* Asset, const FString& AssetPath, bool bIncludeReferences, int32 ArrayLimit)
	{
		const FAssetEnricherDef* Def = FindEnricher(Asset);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : FString());
		Result->SetStringField(TEXT("class"), Asset ? Asset->GetClass()->GetName() : FString());
		Result->SetBoolField(TEXT("enriched"), Def != nullptr);
		Result->SetStringField(TEXT("enricher"), Def ? Def->Key : TEXT("generic"));
		Result->SetObjectField(TEXT("registry"), BuildAssetRegistryDetails(Asset));

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		if (Asset)
		{
			Details->SetObjectField(TEXT("properties"), SerializeStructFields(Asset->GetClass(), Asset, 2, ArrayLimit));
		}
		if (Def)
		{
			Details->SetStringField(TEXT("display_name"), Def->DisplayName);
			TArray<FString> MatchedClasses;
			for (const TCHAR* ClassName : Def->ClassNames)
			{
				MatchedClasses.Add(ClassName);
			}
			Details->SetArrayField(TEXT("supported_class_names"), StringArrayToJson(MatchedClasses));
		}
		if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
		{
			Details->SetObjectField(TEXT("texture"), BuildTextureDetails(Texture));
		}
		Result->SetObjectField(TEXT("details"), Details);

		TArray<TSharedPtr<FJsonValue>> References;
		const TArray<TSharedPtr<FJsonValue>>* ReferencesForWarnings = nullptr;
		if (bIncludeReferences)
		{
			References = CollectAssetReferences(Asset, ArrayLimit);
			ReferencesForWarnings = &References;
			Result->SetArrayField(TEXT("references"), References);
		}

		Result->SetArrayField(TEXT("warnings"), BuildValidationWarnings(Asset, Def, ArrayLimit, ReferencesForWarnings));
		return Result;
	}
}

void FMonolithAssetInspectionActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("list_supported_asset_enrichers"),
		TEXT("List read-only typed asset enrichers supported by Monolith."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetInspectionActions::ListSupportedAssetEnrichers),
		FParamSchemaBuilder().StrictComplexTypes().Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("inspect_asset"),
		TEXT("Inspect an asset with typed read-only enrichment when supported."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetInspectionActions::InspectAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Optional(TEXT("include_references"), TEXT("boolean"), TEXT("Include reflected object references"), TEXT("true"))
			.Optional(TEXT("array_limit"), TEXT("number"), TEXT("Maximum items per reflected array"), TEXT("32"))
			.StrictComplexTypes()
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("inspect_assets_batch"),
		TEXT("Inspect multiple assets with per-row success/error results."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetInspectionActions::InspectAssetsBatch),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Asset paths to inspect"))
			.Optional(TEXT("include_references"), TEXT("boolean"), TEXT("Include reflected object references"), TEXT("false"))
			.Optional(TEXT("array_limit"), TEXT("number"), TEXT("Maximum items per reflected array"), TEXT("16"))
			.StrictComplexTypes()
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("validate_typed_asset"),
		TEXT("Validate a typed asset and report warnings without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetInspectionActions::ValidateTypedAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to validate"))
			.Optional(TEXT("array_limit"), TEXT("number"), TEXT("Array cap used when evaluating large payload warnings"), TEXT("32"))
			.StrictComplexTypes()
			.Build());
}

FMonolithActionResult FMonolithAssetInspectionActions::ListSupportedAssetEnrichers(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetEnricherDef& Def : GetSupportedEnrichers())
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("key"), Def.Key);
		Item->SetStringField(TEXT("display_name"), Def.DisplayName);
		Item->SetBoolField(TEXT("large_payload"), Def.bLargePayload);

		TArray<FString> ClassNames;
		for (const TCHAR* ClassName : Def.ClassNames)
		{
			ClassNames.Add(ClassName);
		}
		Item->SetArrayField(TEXT("class_names"), StringArrayToJson(ClassNames));
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("enrichers"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetInspectionActions::InspectAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeReferences = true;
	if (Params->HasField(TEXT("include_references"))
		&& (!Params->HasTypedField<EJson::Boolean>(TEXT("include_references"))
			|| !Params->TryGetBoolField(TEXT("include_references"), bIncludeReferences)))
	{
		return FMonolithActionResult::Error(
			TEXT("Invalid parameter 'include_references': must be a boolean."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	double ArrayLimitNumber = 32.0;
	Params->TryGetNumberField(TEXT("array_limit"), ArrayLimitNumber);
	const int32 ArrayLimit = FMath::Clamp(static_cast<int32>(ArrayLimitNumber), 0, 256);

	return FMonolithActionResult::Success(InspectLoadedAsset(Asset, AssetPath, bIncludeReferences, ArrayLimit));
}

FMonolithActionResult FMonolithAssetInspectionActions::InspectAssetsBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathValues = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathValues) || !AssetPathValues)
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_paths'"));
	}

	bool bIncludeReferences = false;
	if (Params->HasField(TEXT("include_references"))
		&& (!Params->HasTypedField<EJson::Boolean>(TEXT("include_references"))
			|| !Params->TryGetBoolField(TEXT("include_references"), bIncludeReferences)))
	{
		return FMonolithActionResult::Error(
			TEXT("Invalid parameter 'include_references': must be a boolean."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	double ArrayLimitNumber = 16.0;
	Params->TryGetNumberField(TEXT("array_limit"), ArrayLimitNumber);
	const int32 ArrayLimit = FMath::Clamp(static_cast<int32>(ArrayLimitNumber), 0, 256);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(AssetPathValues->Num());

	for (const TSharedPtr<FJsonValue>& Value : *AssetPathValues)
	{
		FString InputPath;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Value.IsValid() || !Value->TryGetString(InputPath) || InputPath.IsEmpty())
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetStringField(TEXT("error"), TEXT("Each asset_paths item must be a non-empty string."));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		FString AssetPath = FMonolithAssetUtils::ResolveAssetPath(InputPath);
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (!Asset)
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetStringField(TEXT("asset_path"), AssetPath);
			Row->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		Row = InspectLoadedAsset(Asset, AssetPath, bIncludeReferences, ArrayLimit);
		Row->SetStringField(TEXT("status"), TEXT("ok"));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetInspectionActions::ValidateTypedAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	double ArrayLimitNumber = 32.0;
	Params->TryGetNumberField(TEXT("array_limit"), ArrayLimitNumber);
	const int32 ArrayLimit = FMath::Clamp(static_cast<int32>(ArrayLimitNumber), 0, 256);

	const FAssetEnricherDef* Def = FindEnricher(Asset);
	TArray<TSharedPtr<FJsonValue>> Warnings = BuildValidationWarnings(Asset, Def, ArrayLimit);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Result->SetBoolField(TEXT("supported"), Def != nullptr);
	Result->SetStringField(TEXT("enricher"), Def ? Def->Key : TEXT("generic"));
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	Result->SetBoolField(TEXT("valid"), !HasBlockingValidationWarnings(Warnings));
	return FMonolithActionResult::Success(Result);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace MonolithAsset::Tests
{
	bool DoesSoftReferenceExistForTest(const FSoftObjectPath& Path)
	{
		return DoesSoftReferenceExist(Path);
	}
}
#endif
