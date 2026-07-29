#include "MonolithChooserReadActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

namespace MonolithChooserRead
{
	constexpr int32 MaxColumns = 512;
	constexpr int32 MaxRowsPerResponse = 500;
	constexpr int32 MaxTablesPerResponse = 1000;
	constexpr int32 MaxReferencesPerScan = 4096;
	constexpr int32 MaxReferenceDepth = 12;
	constexpr int32 MaxArrayElementsPerReferenceProperty = 4096;
	constexpr int32 MaxSerializedDepth = 3;
	constexpr int32 MaxCompactSerializedFields = 16;
	constexpr int32 MaxSerializedFields = 128;
	constexpr int32 MaxCompactContainerElements = 8;
	constexpr int32 MaxSerializedContainerElements = 256;
	constexpr int32 MaxSerializedStringChars = 4096;

	struct FReferenceScan
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		TSet<FString> Seen;
		TMap<FString, bool> PackageExistsCache;
		bool bTruncated = false;
	};

	FMonolithActionResult InvalidParam(const FString& Param, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("param"), Param);
		Data->SetStringField(TEXT("reason"), Reason);
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), *Param, *Reason),
			FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data);
	}

	FTopLevelAssetPath GetChooserTableClassPath()
	{
		return FTopLevelAssetPath(TEXT("/Script/Chooser"), TEXT("ChooserTable"));
	}

	UClass* LoadChooserTableClass()
	{
		UClass* ChooserClass = FindObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
		if (!ChooserClass)
		{
			ChooserClass = LoadObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
		}
		return ChooserClass;
	}

	bool ParseBoundedInteger(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FMonolithActionResult& OutError)
	{
		double Number = static_cast<double>(DefaultValue);
		if (Params->HasField(Field) && !Params->TryGetNumberField(Field, Number))
		{
			OutError = InvalidParam(Field, TEXT("expected an integer JSON number"));
			return false;
		}

		if (!FMath::IsFinite(Number) || Number != FMath::TruncToDouble(Number)
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue))
		{
			OutError = InvalidParam(
				Field,
				FString::Printf(TEXT("expected an integer in the range %d..%d"), MinValue, MaxValue));
			return false;
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool NormalizePackageFilter(
		const FString& Input,
		FString& OutFilter,
		FMonolithActionResult& OutError)
	{
		OutFilter = Input;
		if (OutFilter.IsEmpty())
		{
			return true;
		}

		FString Trimmed = OutFilter;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != OutFilter)
		{
			OutError = InvalidParam(TEXT("path_filter"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (OutFilter.Contains(TEXT("\\")) || OutFilter.Contains(TEXT(":"))
			|| OutFilter.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| OutFilter.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = InvalidParam(
				TEXT("path_filter"),
				TEXT("expected a canonical Unreal long package prefix such as /Game/Choosers"));
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(OutFilter))
		{
			OutError = InvalidParam(
				TEXT("path_filter"),
				TEXT("expected a canonical Unreal long package prefix beginning with a mounted root"));
			return false;
		}
		OutFilter.RemoveFromEnd(TEXT("/"));
		return true;
	}

	bool NormalizeChooserAssetPath(
		const FString& Input,
		FString& OutPackagePath,
		FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		OutPackagePath.Reset();
		OutObjectPath.Reset();
		if (Input.IsEmpty())
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("a non-empty path is required"));
			return false;
		}

		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != Input)
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (!Input.StartsWith(TEXT("/")) || Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT(":"))
			|| Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = InvalidParam(
				TEXT("asset_path"),
				TEXT("expected a canonical Unreal package or top-level object path, not a filesystem, relative, or subobject path"));
			return false;
		}

		FString RequestedObjectName;
		int32 DotIndex = INDEX_NONE;
		if (Input.FindLastChar(TEXT('.'), DotIndex))
		{
			OutPackagePath = Input.Left(DotIndex);
			RequestedObjectName = Input.Mid(DotIndex + 1);
			if (RequestedObjectName.IsEmpty() || RequestedObjectName.Contains(TEXT(".")))
			{
				OutError = InvalidParam(TEXT("asset_path"), TEXT("the top-level object name is malformed"));
				return false;
			}
		}
		else
		{
			OutPackagePath = Input;
		}

		if (!FPackageName::IsValidLongPackageName(OutPackagePath))
		{
			OutError = InvalidParam(
				TEXT("asset_path"),
				TEXT("the package portion is not a valid mounted Unreal long package name"));
			return false;
		}

		const FString CanonicalObjectName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (CanonicalObjectName.IsEmpty())
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("the package path has no asset name"));
			return false;
		}
		if (!RequestedObjectName.IsEmpty()
			&& !RequestedObjectName.Equals(CanonicalObjectName, ESearchCase::CaseSensitive))
		{
			OutError = InvalidParam(
				TEXT("asset_path"),
				FString::Printf(
					TEXT("top-level object name must exactly match the package asset name '%s'"),
					*CanonicalObjectName));
			return false;
		}

		OutObjectPath = OutPackagePath + TEXT(".") + CanonicalObjectName;
		return true;
	}

	UObject* LoadChooserFromParams(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutPackagePath,
		FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		FString RequestedPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), RequestedPath))
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("expected a string"));
			return nullptr;
		}
		if (!NormalizeChooserAssetPath(RequestedPath, OutPackagePath, OutObjectPath, OutError))
		{
			return nullptr;
		}

		UClass* ChooserClass = LoadChooserTableClass();
		if (!ChooserClass)
		{
			OutError = FMonolithActionResult::Error(
				TEXT("ChooserTable class is unavailable; enable the engine Chooser plugin for asset readback"),
				FMonolithJsonUtils::ErrOptionalDepUnavailable);
			return nullptr;
		}

		UObject* Chooser = FMonolithAssetUtils::LoadAssetByPath(ChooserClass, OutObjectPath);
		if (!Chooser)
		{
			OutError = InvalidParam(
				TEXT("asset_path"),
				FString::Printf(TEXT("no ChooserTable resolves at the exact path '%s'"), *OutObjectPath));
			return nullptr;
		}

		const FString LoadedPath = Chooser->GetPathName();
		if (!LoadedPath.Equals(OutObjectPath, ESearchCase::CaseSensitive))
		{
			OutError = InvalidParam(
				TEXT("asset_path"),
				FString::Printf(
					TEXT("aliases, redirectors, and case-only variants are rejected (requested '%s', resolved '%s')"),
					*OutObjectPath,
					*LoadedPath));
			return nullptr;
		}
		return Chooser;
	}

	const FArrayProperty* FindArrayProperty(const UStruct* Struct, const TCHAR* PropertyName)
	{
		return Struct ? FindFProperty<FArrayProperty>(Struct, PropertyName) : nullptr;
	}

	bool TryGetArrayNum(const UObject* Object, const TCHAR* PropertyName, int32& OutNum)
	{
		OutNum = 0;
		if (!Object)
		{
			return false;
		}
		const FArrayProperty* ArrayProperty = FindArrayProperty(Object->GetClass(), PropertyName);
		if (!ArrayProperty)
		{
			return false;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		OutNum = Helper.Num();
		return true;
	}

	int32 GetArrayNum(const UObject* Object, const TCHAR* PropertyName)
	{
		int32 Num = 0;
		TryGetArrayNum(Object, PropertyName, Num);
		return Num;
	}

	bool GetBoolArrayValue(const UObject* Object, const TCHAR* PropertyName, int32 Index)
	{
		if (!Object)
		{
			return false;
		}
		const FArrayProperty* ArrayProperty = FindArrayProperty(Object->GetClass(), PropertyName);
		const FBoolProperty* InnerBool = ArrayProperty ? CastField<FBoolProperty>(ArrayProperty->Inner) : nullptr;
		if (!ArrayProperty || !InnerBool)
		{
			return false;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		return Index >= 0 && Index < Helper.Num()
			? InnerBool->GetPropertyValue(Helper.GetRawPtr(Index))
			: false;
	}

	const FInstancedStruct* GetInstancedStructFromArray(
		const UObject* Object,
		const TCHAR* PropertyName,
		int32 Index)
	{
		if (!Object)
		{
			return nullptr;
		}
		const FArrayProperty* ArrayProperty = FindArrayProperty(Object->GetClass(), PropertyName);
		const FStructProperty* InnerStruct = ArrayProperty
			? CastField<FStructProperty>(ArrayProperty->Inner)
			: nullptr;
		if (!ArrayProperty || !InnerStruct || InnerStruct->Struct != FInstancedStruct::StaticStruct())
		{
			return nullptr;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		return Index >= 0 && Index < Helper.Num()
			? reinterpret_cast<const FInstancedStruct*>(Helper.GetRawPtr(Index))
			: nullptr;
	}

	FArrayProperty* FindRowValuesProperty(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}
		if (FArrayProperty* Exact = FindFProperty<FArrayProperty>(Struct, TEXT("RowValues")))
		{
			if (!Exact->HasAnyPropertyFlags(CPF_Deprecated))
			{
				return Exact;
			}
		}
		if (FArrayProperty* WithAny = FindFProperty<FArrayProperty>(Struct, TEXT("RowValuesWithAny")))
		{
			return WithAny;
		}

		FArrayProperty* BestCandidate = nullptr;
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FArrayProperty* Candidate = CastField<FArrayProperty>(*It);
			if (!Candidate
				|| !Candidate->GetName().Contains(TEXT("RowValues"))
				|| Candidate->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			if (!BestCandidate
				|| Candidate->GetName() < BestCandidate->GetName())
			{
				BestCandidate = Candidate;
			}
		}
		return BestCandidate;
	}

	int32 GetRowValuesCount(const FInstancedStruct& Column)
	{
		FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct());
		if (!RowValues || !Column.GetMemory())
		{
			return 0;
		}
		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		return Helper.Num();
	}

	TSharedPtr<FJsonObject> SerializeStructFields(
		const UStruct* Struct,
		const void* StructMemory,
		int32 MaxDepth,
		bool Compact);

	TSharedPtr<FJsonObject> SerializeInstancedStruct(
		const FInstancedStruct& Instance,
		int32 MaxDepth,
		bool Compact);

	TSharedPtr<FJsonValue> PropertyValueToJson(
		const FProperty* Property,
		const void* Value,
		int32 MaxDepth,
		bool Compact);

	TSharedPtr<FJsonValue> GetRowValueAt(const FInstancedStruct& Column, int32 RowIndex)
	{
		FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct());
		if (!RowValues || !Column.GetMemory())
		{
			return MakeShared<FJsonValueNull>();
		}
		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		if (RowIndex < 0 || RowIndex >= Helper.Num())
		{
			return MakeShared<FJsonValueNull>();
		}
		return PropertyValueToJson(
			RowValues->Inner,
			Helper.GetRawPtr(RowIndex),
			MaxSerializedDepth,
			false);
	}

	bool GetColumnBoolField(const FInstancedStruct& Column, const TCHAR* PropertyName, bool DefaultValue = false)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		const FBoolProperty* Property = Struct
			? FindFProperty<FBoolProperty>(Struct, PropertyName)
			: nullptr;
		if (!Property || !Column.GetMemory())
		{
			return DefaultValue;
		}
		return Property->GetPropertyValue(Property->ContainerPtrToValuePtr<void>(Column.GetMemory()));
	}

	bool IsOutputColumn(const FInstancedStruct& Column)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		if (!Struct)
		{
			return false;
		}
		return Struct->GetName().StartsWith(TEXT("Output"))
			|| FindFProperty<FProperty>(Struct, TEXT("FallbackValue")) != nullptr;
	}

	TSharedPtr<FJsonValue> BoundedStringValue(
		const FString& Value,
		const TCHAR* Serialization)
	{
		if (Value.Len() <= MaxSerializedStringChars)
		{
			return MakeShared<FJsonValueString>(Value);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("serialization"), Serialization);
		Result->SetStringField(
			TEXT("value"),
			Value.Left(MaxSerializedStringChars));
		Result->SetNumberField(TEXT("original_char_count"), Value.Len());
		Result->SetNumberField(TEXT("truncated_after"), MaxSerializedStringChars);
		return MakeShared<FJsonValueObject>(Result);
	}

	TSharedPtr<FJsonValue> ExportPropertyValue(
		const FProperty* Property,
		const void* Value)
	{
		if (!Property || !Value)
		{
			return MakeShared<FJsonValueNull>();
		}

		FString Exported;
		Property->ExportTextItem_Direct(Exported, Value, nullptr, nullptr, PPF_None);
		return BoundedStringValue(Exported, TEXT("export_text"));
	}

	TSharedPtr<FJsonValue> PropertyValueToJson(
		const FProperty* Property,
		const void* Value,
		int32 MaxDepth,
		bool Compact)
	{
		if (!Property || !Value)
		{
			return MakeShared<FJsonValueNull>();
		}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(Value));
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 RawValue =
				EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			return MakeShared<FJsonValueString>(
				EnumProperty->GetEnum()
					? EnumProperty->GetEnum()->GetNameStringByValue(RawValue)
					: FString::Printf(TEXT("%lld"), RawValue));
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 RawValue = ByteProperty->GetPropertyValue(Value);
			if (ByteProperty->Enum)
			{
				return MakeShared<FJsonValueString>(
					ByteProperty->Enum->GetNameStringByValue(RawValue));
			}
			return MakeShared<FJsonValueNumber>(RawValue);
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				const bool Unsigned =
					CastField<FUInt16Property>(Property)
					|| CastField<FUInt32Property>(Property)
					|| CastField<FUInt64Property>(Property);
				return MakeShared<FJsonValueNumber>(
					Unsigned
						? static_cast<double>(
							NumericProperty->GetUnsignedIntPropertyValue(Value))
						: static_cast<double>(
							NumericProperty->GetSignedIntPropertyValue(Value)));
			}
			return MakeShared<FJsonValueNumber>(
				NumericProperty->GetFloatingPointPropertyValue(Value));
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(
				NameProperty->GetPropertyValue(Value).ToString());
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return BoundedStringValue(
				StringProperty->GetPropertyValue(Value),
				TEXT("string"));
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return BoundedStringValue(
				TextProperty->GetPropertyValue(Value).ToString(),
				TEXT("text"));
		}

		if (const FSoftObjectProperty* SoftProperty =
			CastField<FSoftObjectProperty>(Property))
		{
			return MakeShared<FJsonValueString>(
				SoftProperty->GetPropertyValue(Value).ToSoftObjectPath().ToString());
		}
		if (const FObjectPropertyBase* ObjectProperty =
			CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
			return MakeShared<FJsonValueString>(
				Object ? Object->GetPathName() : TEXT("None"));
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (MaxDepth <= 0)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
				Result->SetBoolField(TEXT("depth_limited"), true);
				Result->SetStringField(TEXT("property_type"), Property->GetCPPType());
				if (StructProperty->Struct == FInstancedStruct::StaticStruct())
				{
					const FInstancedStruct& Instance =
						*reinterpret_cast<const FInstancedStruct*>(Value);
					Result->SetBoolField(TEXT("valid"), Instance.IsValid());
					Result->SetStringField(
						TEXT("script_struct"),
						Instance.GetScriptStruct()
							? Instance.GetScriptStruct()->GetPathName()
							: FString());
				}
				else
				{
					Result->SetStringField(
						TEXT("script_struct"),
						StructProperty->Struct
							? StructProperty->Struct->GetPathName()
							: FString());
				}
				return MakeShared<FJsonValueObject>(Result);
			}
			if (StructProperty->Struct == FInstancedStruct::StaticStruct())
			{
				return MakeShared<FJsonValueObject>(
					SerializeInstancedStruct(
						*reinterpret_cast<const FInstancedStruct*>(Value),
						MaxDepth - 1,
						Compact));
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("script_struct"),
				StructProperty->Struct
					? StructProperty->Struct->GetPathName()
					: FString());
			Result->SetObjectField(
				TEXT("fields"),
				SerializeStructFields(
					StructProperty->Struct,
					Value,
					MaxDepth - 1,
					Compact));
			return MakeShared<FJsonValueObject>(Result);
		}

		const int32 ContainerLimit =
			Compact ? MaxCompactContainerElements : MaxSerializedContainerElements;
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (MaxDepth <= 0)
			{
				FScriptArrayHelper Helper(ArrayProperty, Value);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Items;
				Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
				Result->SetBoolField(TEXT("depth_limited"), true);
				Result->SetNumberField(TEXT("count"), Helper.Num());
				Result->SetArrayField(TEXT("items"), Items);
				return MakeShared<FJsonValueObject>(Result);
			}
			FScriptArrayHelper Helper(ArrayProperty, Value);
			const int32 Count = FMath::Min(Helper.Num(), ContainerLimit);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Items.Add(PropertyValueToJson(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					MaxDepth - 1,
					Compact));
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Helper.Num());
			Result->SetArrayField(TEXT("items"), Items);
			if (Count < Helper.Num())
			{
				Result->SetNumberField(TEXT("truncated_after"), Count);
			}
			return MakeShared<FJsonValueObject>(Result);
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			if (MaxDepth <= 0)
			{
				FScriptSetHelper Helper(SetProperty, Value);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Items;
				Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
				Result->SetBoolField(TEXT("depth_limited"), true);
				Result->SetNumberField(TEXT("count"), Helper.Num());
				Result->SetArrayField(TEXT("items"), Items);
				return MakeShared<FJsonValueObject>(Result);
			}
			FScriptSetHelper Helper(SetProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(FMath::Min(Helper.Num(), ContainerLimit));
			for (int32 Index = 0;
				Index < Helper.GetMaxIndex() && Items.Num() < ContainerLimit;
				++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Items.Add(PropertyValueToJson(
						SetProperty->ElementProp,
						Helper.GetElementPtr(Index),
						MaxDepth - 1,
						Compact));
				}
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Helper.Num());
			Result->SetArrayField(TEXT("items"), Items);
			if (Items.Num() < Helper.Num())
			{
				Result->SetNumberField(TEXT("truncated_after"), Items.Num());
			}
			return MakeShared<FJsonValueObject>(Result);
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			if (MaxDepth <= 0)
			{
				FScriptMapHelper Helper(MapProperty, Value);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Entries;
				Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
				Result->SetBoolField(TEXT("depth_limited"), true);
				Result->SetNumberField(TEXT("count"), Helper.Num());
				Result->SetArrayField(TEXT("entries"), Entries);
				return MakeShared<FJsonValueObject>(Result);
			}
			FScriptMapHelper Helper(MapProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Entries;
			Entries.Reserve(FMath::Min(Helper.Num(), ContainerLimit));
			for (int32 Index = 0;
				Index < Helper.GetMaxIndex() && Entries.Num() < ContainerLimit;
				++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(
					TEXT("key"),
					PropertyValueToJson(
						MapProperty->KeyProp,
						Helper.GetKeyPtr(Index),
						MaxDepth - 1,
						Compact));
				Entry->SetField(
					TEXT("value"),
					PropertyValueToJson(
						MapProperty->ValueProp,
						Helper.GetValuePtr(Index),
						MaxDepth - 1,
						Compact));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Helper.Num());
			Result->SetArrayField(TEXT("entries"), Entries);
			if (Entries.Num() < Helper.Num())
			{
				Result->SetNumberField(TEXT("truncated_after"), Entries.Num());
			}
			return MakeShared<FJsonValueObject>(Result);
		}

		return ExportPropertyValue(Property, Value);
	}

	TSharedPtr<FJsonObject> SerializeStructFields(
		const UStruct* Struct,
		const void* StructMemory,
		int32 MaxDepth,
		bool Compact)
	{
		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (!Struct || !StructMemory)
		{
			return Fields;
		}

		const int32 FieldLimit =
			Compact ? MaxCompactSerializedFields : MaxSerializedFields;
		int32 FieldCount = 0;
		for (TFieldIterator<FProperty> It(
				Struct,
				EFieldIteratorFlags::IncludeSuper,
				EFieldIteratorFlags::ExcludeDeprecated);
			It;
			++It)
		{
			if (FieldCount >= FieldLimit)
			{
				Fields->SetNumberField(TEXT("__truncated_after"), FieldCount);
				break;
			}
			const FProperty* Property = *It;
			Fields->SetField(
				Property->GetName(),
				PropertyValueToJson(
					Property,
					Property->ContainerPtrToValuePtr<void>(StructMemory),
					MaxDepth,
					Compact));
			++FieldCount;
		}
		return Fields;
	}

	TSharedPtr<FJsonObject> SerializeInstancedStruct(
		const FInstancedStruct& Instance,
		int32 MaxDepth,
		bool Compact)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), Instance.IsValid());

		const UScriptStruct* Struct = Instance.GetScriptStruct();
		Result->SetStringField(TEXT("script_struct"), Struct ? Struct->GetPathName() : FString());
		Result->SetStringField(TEXT("type"), Struct ? Struct->GetName() : FString());
		Result->SetStringField(TEXT("display_name"), Struct ? Struct->GetDisplayNameText().ToString() : FString());

		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (Struct && Instance.GetMemory())
		{
			Fields = SerializeStructFields(
				Struct,
				Instance.GetMemory(),
				MaxDepth,
				Compact);
		}
		Result->SetObjectField(TEXT("fields"), Fields);
		return Result;
	}

	TSharedPtr<FJsonObject> SerializeColumnSummary(const FInstancedStruct& Column, int32 Index)
	{
		TSharedPtr<FJsonObject> Result =
			SerializeInstancedStruct(Column, 2, true);
		Result->SetNumberField(TEXT("index"), Index);
		Result->SetBoolField(TEXT("is_output"), IsOutputColumn(Column));
		Result->SetBoolField(TEXT("is_disabled"), GetColumnBoolField(Column, TEXT("bDisabled")));

		if (FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct()))
		{
			Result->SetStringField(TEXT("row_values_property"), RowValues->GetName());
			Result->SetStringField(
				TEXT("row_values_type"),
				RowValues->Inner ? RowValues->Inner->GetCPPType() : FString());
			Result->SetNumberField(TEXT("row_value_count"), GetRowValuesCount(Column));
		}
		else
		{
			Result->SetStringField(TEXT("row_values_property"), FString());
			Result->SetNumberField(TEXT("row_value_count"), 0);
		}
		return Result;
	}

	int32 GetChooserRowCount(const UObject* Chooser)
	{
		int32 RowCount = 0;
		RowCount = FMath::Max(RowCount, GetArrayNum(Chooser, TEXT("ResultsStructs")));
		RowCount = FMath::Max(RowCount, GetArrayNum(Chooser, TEXT("CookedResults")));
		RowCount = FMath::Max(RowCount, GetArrayNum(Chooser, TEXT("DisabledRows")));

		const int32 ColumnCount = FMath::Min(GetArrayNum(Chooser, TEXT("ColumnsStructs")), MaxColumns);
		for (int32 Index = 0; Index < ColumnCount; ++Index)
		{
			if (const FInstancedStruct* Column =
				GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), Index))
			{
				RowCount = FMath::Max(RowCount, GetRowValuesCount(*Column));
			}
		}
		return RowCount;
	}

	TArray<TSharedPtr<FJsonValue>> SerializeColumns(
		const UObject* Chooser,
		int32& OutTotalColumns,
		bool& OutTruncated)
	{
		OutTotalColumns = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
		const int32 Count = FMath::Min(OutTotalColumns, MaxColumns);
		OutTruncated = Count < OutTotalColumns;

		TArray<TSharedPtr<FJsonValue>> Columns;
		Columns.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (const FInstancedStruct* Column =
				GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), Index))
			{
				Columns.Add(MakeShared<FJsonValueObject>(SerializeColumnSummary(*Column, Index)));
			}
		}
		return Columns;
	}

	TArray<TSharedPtr<FJsonValue>> SerializeRows(
		const UObject* Chooser,
		int32 StartRow,
		int32 Limit)
	{
		const int32 RowCount = GetChooserRowCount(Chooser);
		const int32 ColumnCount = FMath::Min(GetArrayNum(Chooser, TEXT("ColumnsStructs")), MaxColumns);
		const int32 EndRow = FMath::Min(RowCount, StartRow + Limit);

		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(FMath::Max(0, EndRow - StartRow));
		for (int32 RowIndex = StartRow; RowIndex < EndRow; ++RowIndex)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("row_index"), RowIndex);
			Row->SetBoolField(TEXT("disabled"), GetBoolArrayValue(Chooser, TEXT("DisabledRows"), RowIndex));

			if (const FInstancedStruct* ResultStruct =
				GetInstancedStructFromArray(Chooser, TEXT("ResultsStructs"), RowIndex))
			{
				Row->SetObjectField(
					TEXT("result"),
					SerializeInstancedStruct(
						*ResultStruct,
						MaxSerializedDepth,
						false));
			}
			else
			{
				Row->SetField(TEXT("result"), MakeShared<FJsonValueNull>());
			}

			TArray<TSharedPtr<FJsonValue>> Cells;
			Cells.Reserve(ColumnCount);
			for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
			{
				if (const FInstancedStruct* Column =
					GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), ColumnIndex))
				{
					TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
					Cell->SetNumberField(TEXT("column_index"), ColumnIndex);
					Cell->SetStringField(
						TEXT("column_type"),
						Column->GetScriptStruct() ? Column->GetScriptStruct()->GetName() : FString());
					Cell->SetBoolField(TEXT("is_output"), IsOutputColumn(*Column));
					Cell->SetField(TEXT("value"), GetRowValueAt(*Column, RowIndex));
					Cells.Add(MakeShared<FJsonValueObject>(Cell));
				}
			}
			Row->SetArrayField(TEXT("cells"), Cells);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	bool PackageExistsForSoftPath(const FSoftObjectPath& Path, FReferenceScan& Scan)
	{
		const FString AssetPath = Path.GetAssetPathString();
		if (AssetPath.IsEmpty())
		{
			return false;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (PackageName.StartsWith(TEXT("/Script/")))
		{
			return true;
		}
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return false;
		}
		if (const bool* Cached = Scan.PackageExistsCache.Find(PackageName))
		{
			return *Cached;
		}

		bool Exists = FindPackage(nullptr, *PackageName) != nullptr;
		if (!Exists)
		{
			IAssetRegistry& Registry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<FAssetData> PackageAssets;
			Registry.GetAssetsByPackageName(FName(*PackageName), PackageAssets);
			Exists = PackageAssets.Num() > 0;
		}
		Scan.PackageExistsCache.Add(PackageName, Exists);
		return Exists;
	}

	void AddReference(
		FReferenceScan& Scan,
		const FString& Source,
		const FString& Path,
		const FString& ClassName,
		bool Loaded,
		bool Soft,
		bool Exists)
	{
		if (Path.IsEmpty())
		{
			return;
		}
		if (Scan.Values.Num() >= MaxReferencesPerScan)
		{
			Scan.bTruncated = true;
			return;
		}

		const FString Key = Source + TEXT("|") + Path;
		if (Scan.Seen.Contains(Key))
		{
			return;
		}
		Scan.Seen.Add(Key);

		TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
		Reference->SetStringField(TEXT("source"), Source);
		Reference->SetStringField(TEXT("path"), Path);
		Reference->SetStringField(TEXT("class"), ClassName);
		Reference->SetBoolField(TEXT("loaded"), Loaded);
		Reference->SetBoolField(TEXT("soft"), Soft);
		Reference->SetBoolField(TEXT("exists"), Exists);
		if (Soft)
		{
			const FSoftObjectPath SoftPath(Path);
			Reference->SetStringField(TEXT("asset_path"), SoftPath.GetAssetPathString());
			Reference->SetStringField(TEXT("sub_path"), SoftPath.GetSubPathString());
		}
		Scan.Values.Add(MakeShared<FJsonValueObject>(Reference));
	}

	void CollectReferencesFromStruct(
		const UStruct* Struct,
		const void* StructMemory,
		const FString& Source,
		FReferenceScan& Scan,
		int32 Depth);

	void CollectReferencesFromProperty(
		const FProperty* Property,
		const void* Value,
		const FString& Source,
		FReferenceScan& Scan,
		int32 Depth)
	{
		if (!Property || !Value || Depth < 0 || Scan.bTruncated)
		{
			return;
		}

		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath Path =
				SoftProperty->GetPropertyValue(Value).ToSoftObjectPath();
			if (Path.IsValid())
			{
				AddReference(
					Scan,
					Source,
					Path.ToString(),
					SoftProperty->PropertyClass ? SoftProperty->PropertyClass->GetName() : TEXT("soft_object"),
					Path.ResolveObject() != nullptr,
					true,
					PackageExistsForSoftPath(Path, Scan));
			}
			return;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value))
			{
				AddReference(
					Scan,
					Source,
					Object->GetPathName(),
					Object->GetClass()->GetName(),
					true,
					false,
					true);
			}
			return;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FInstancedStruct::StaticStruct())
			{
				const FInstancedStruct& Instance =
					*reinterpret_cast<const FInstancedStruct*>(Value);
				if (Instance.IsValid())
				{
					CollectReferencesFromStruct(
						Instance.GetScriptStruct(),
						Instance.GetMemory(),
						Source,
						Scan,
						Depth - 1);
				}
				return;
			}
			CollectReferencesFromStruct(
				StructProperty->Struct,
				Value,
				Source,
				Scan,
				Depth - 1);
			return;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, Value);
			const int32 Count = FMath::Min(Helper.Num(), MaxArrayElementsPerReferenceProperty);
			const bool bPropertyTruncated = Count < Helper.Num();
			for (int32 Index = 0; Index < Count && !Scan.bTruncated; ++Index)
			{
				CollectReferencesFromProperty(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					FString::Printf(TEXT("%s[%d]"), *Source, Index),
					Scan,
					Depth - 1);
			}
			Scan.bTruncated |= bPropertyTruncated;
			return;
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, Value);
			int32 Visited = 0;
			for (int32 Index = 0;
				Index < Helper.GetMaxIndex() && !Scan.bTruncated;
				++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (++Visited > MaxArrayElementsPerReferenceProperty)
				{
					Scan.bTruncated = true;
					break;
				}
				CollectReferencesFromProperty(
					SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					FString::Printf(TEXT("%s{%d}"), *Source, Index),
					Scan,
					Depth - 1);
			}
			return;
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, Value);
			int32 Visited = 0;
			for (int32 Index = 0;
				Index < Helper.GetMaxIndex() && !Scan.bTruncated;
				++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (++Visited > MaxArrayElementsPerReferenceProperty)
				{
					Scan.bTruncated = true;
					break;
				}
				const FString EntrySource = FString::Printf(TEXT("%s{%d}"), *Source, Index);
				CollectReferencesFromProperty(
					MapProperty->KeyProp,
					Helper.GetKeyPtr(Index),
					EntrySource + TEXT(".key"),
					Scan,
					Depth - 1);
				CollectReferencesFromProperty(
					MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					EntrySource + TEXT(".value"),
					Scan,
					Depth - 1);
			}
		}
	}

	void CollectReferencesFromStruct(
		const UStruct* Struct,
		const void* StructMemory,
		const FString& Source,
		FReferenceScan& Scan,
		int32 Depth)
	{
		if (!Struct || !StructMemory || Depth < 0 || Scan.bTruncated)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper);
			It && !Scan.bTruncated;
			++It)
		{
			const FProperty* Property = *It;
			CollectReferencesFromProperty(
				Property,
				Property->ContainerPtrToValuePtr<void>(StructMemory),
				Source + TEXT(".") + Property->GetName(),
				Scan,
				Depth);
		}
	}

	FReferenceScan CollectChooserReferences(const UObject* Chooser)
	{
		FReferenceScan Scan;
		CollectReferencesFromStruct(
			Chooser ? Chooser->GetClass() : nullptr,
			Chooser,
			TEXT("chooser"),
			Scan,
			MaxReferenceDepth);
		return Scan;
	}

	void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		int32& ErrorCount,
		int32& WarningCount,
		const FString& Severity,
		const FString& Code,
		const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		if (Severity == TEXT("error"))
		{
			++ErrorCount;
		}
		else if (Severity == TEXT("warning"))
		{
			++WarningCount;
		}
	}

	TArray<TSharedPtr<FJsonValue>> GetChooserAssets(
		const FString& PathFilter,
		int32 Offset,
		int32 Limit,
		int32& OutTotal)
	{
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(GetChooserTableClassPath(), Assets, true);
		Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		TArray<FAssetData> Filtered;
		Filtered.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			const FString PackagePath = Asset.PackageName.ToString();
			if (PathFilter.IsEmpty()
				|| PackagePath.Equals(PathFilter, ESearchCase::CaseSensitive)
				|| PackagePath.StartsWith(
					PathFilter + TEXT("/"),
					ESearchCase::CaseSensitive))
			{
				Filtered.Add(Asset);
			}
		}
		OutTotal = Filtered.Num();

		TArray<TSharedPtr<FJsonValue>> Result;
		const int32 End = Offset >= OutTotal
			? OutTotal
			: FMath::Min(OutTotal, Offset + Limit);
		Result.Reserve(FMath::Max(0, End - Offset));
		for (int32 Index = Offset; Index < End; ++Index)
		{
			const FAssetData& Asset = Filtered[Index];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
			Entry->SetStringField(TEXT("package_path"), Asset.PackageName.ToString());
			Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Result.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Result;
	}
}

void FMonolithChooserReadActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithChooserRead;

	// asset_path deliberately uses the untagged string schema below. AssetPath
	// parameters are normalized by the central dispatcher before handlers run,
	// while this read surface must reject backslashes and other non-canonical
	// spellings instead of silently repairing them.
	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("list_chooser_tables"),
		TEXT("List ChooserTable assets deterministically from AssetRegistry with exact package-prefix filtering and bounded pagination."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Canonical mounted long package prefix, for example /Game/Choosers"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tables to return (1-1000)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("get_chooser_table"),
		TEXT("Read a bounded ChooserTable summary including counts, columns, references, and optional row readback."),
		FMonolithActionHandler::CreateStatic(&HandleGetChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact ChooserTable package or top-level object path"))
			.Optional(TEXT("include_rows"), TEXT("boolean"), TEXT("Include bounded row and cell readback"), TEXT("false"))
			.Optional(TEXT("row_limit"), TEXT("integer"), TEXT("Maximum rows when include_rows=true (1-500)"), TEXT("50"))
			.Build());

	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("list_chooser_columns"),
		TEXT("Read reflected ChooserTable columns with type, output/input classification, and row-value counts."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserColumns),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact ChooserTable package or top-level object path"))
			.Build());

	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("list_chooser_rows"),
		TEXT("Read a bounded page of ChooserTable result rows and reflected per-column cell values."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserRows),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact ChooserTable package or top-level object path"))
			.Optional(TEXT("start_row"), TEXT("integer"), TEXT("Zero-based first row"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("list_chooser_references"),
		TEXT("List reflected hard and soft references in a ChooserTable with source locations and bounded pagination."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserReferences),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact ChooserTable package or top-level object path"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based reference offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum references to return (1-1000)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(
		TEXT("chooser"),
		TEXT("validate_chooser_table"),
		TEXT("Validate reflected ChooserTable row/column alignment and reference resolution without compiling, mutating, or dirtying the package."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact ChooserTable package or top-level object path"))
			.Build());
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserTables(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FString RequestedFilter;
	if (Params->HasField(TEXT("path_filter"))
		&& !Params->TryGetStringField(TEXT("path_filter"), RequestedFilter))
	{
		return InvalidParam(TEXT("path_filter"), TEXT("expected a string"));
	}
	FString PathFilter;
	FMonolithActionResult Error;
	if (!NormalizePackageFilter(RequestedFilter, PathFilter, Error))
	{
		return Error;
	}

	int32 Offset = 0;
	int32 Limit = 200;
	if (!ParseBoundedInteger(Params, TEXT("offset"), 0, 0, MAX_int32, Offset, Error)
		|| !ParseBoundedInteger(
			Params,
			TEXT("limit"),
			200,
			1,
			MaxTablesPerResponse,
			Limit,
			Error))
	{
		return Error;
	}

	int32 Total = 0;
	TArray<TSharedPtr<FJsonValue>> Tables =
		GetChooserAssets(PathFilter, Offset, Limit, Total);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("available"), LoadChooserTableClass() != nullptr);
	Result->SetStringField(TEXT("class_path"), GetChooserTableClassPath().ToString());
	Result->SetStringField(TEXT("path_filter"), PathFilter);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("count"), Tables.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetBoolField(TEXT("has_more"), Offset + Tables.Num() < Total);
	Result->SetArrayField(TEXT("tables"), Tables);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleGetChooserTable(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	bool IncludeRows = false;
	if (Params->HasField(TEXT("include_rows"))
		&& !Params->TryGetBoolField(TEXT("include_rows"), IncludeRows))
	{
		return InvalidParam(TEXT("include_rows"), TEXT("expected a boolean"));
	}
	int32 RowLimit = 50;
	FMonolithActionResult Error;
	if (!ParseBoundedInteger(
		Params,
		TEXT("row_limit"),
		50,
		1,
		MaxRowsPerResponse,
		RowLimit,
		Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = GetChooserRowCount(Chooser);
	int32 ColumnCount = 0;
	bool ColumnsTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Columns =
		SerializeColumns(Chooser, ColumnCount, ColumnsTruncated);
	FReferenceScan References = CollectChooserReferences(Chooser);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetStringField(TEXT("class"), Chooser->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetNumberField(TEXT("result_count"), GetArrayNum(Chooser, TEXT("ResultsStructs")));
	Result->SetNumberField(TEXT("cooked_result_count"), GetArrayNum(Chooser, TEXT("CookedResults")));
	Result->SetNumberField(TEXT("disabled_row_count"), GetArrayNum(Chooser, TEXT("DisabledRows")));
	Result->SetNumberField(TEXT("nested_chooser_count"), GetArrayNum(Chooser, TEXT("NestedChoosers")));
	Result->SetNumberField(TEXT("nested_object_count"), GetArrayNum(Chooser, TEXT("NestedObjects")));
	Result->SetNumberField(TEXT("context_entry_count"), GetArrayNum(Chooser, TEXT("ContextData")));
	Result->SetArrayField(TEXT("columns"), Columns);
	Result->SetBoolField(TEXT("columns_truncated"), ColumnsTruncated);
	Result->SetArrayField(TEXT("references"), References.Values);
	Result->SetBoolField(TEXT("references_truncated"), References.bTruncated);

	if (FStructProperty* Fallback =
		FindFProperty<FStructProperty>(Chooser->GetClass(), TEXT("FallbackResult")))
	{
		Result->SetField(
			TEXT("fallback_result"),
			PropertyValueToJson(
				Fallback,
				Fallback->ContainerPtrToValuePtr<void>(Chooser),
				MaxSerializedDepth,
				false));
	}

	if (IncludeRows)
	{
		TArray<TSharedPtr<FJsonValue>> Rows =
			SerializeRows(Chooser, 0, FMath::Min(RowCount, RowLimit));
		Result->SetArrayField(TEXT("rows"), Rows);
		Result->SetNumberField(TEXT("rows_returned"), Rows.Num());
		Result->SetBoolField(TEXT("rows_truncated"), Rows.Num() < RowCount);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserColumns(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FString PackagePath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	int32 Total = 0;
	bool Truncated = false;
	TArray<TSharedPtr<FJsonValue>> Columns = SerializeColumns(Chooser, Total, Truncated);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetArrayField(TEXT("columns"), Columns);
	Result->SetNumberField(TEXT("count"), Columns.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetBoolField(TEXT("truncated"), Truncated);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserRows(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	int32 StartRow = 0;
	int32 Limit = 100;
	FMonolithActionResult Error;
	if (!ParseBoundedInteger(
			Params,
			TEXT("start_row"),
			0,
			0,
			MAX_int32,
			StartRow,
			Error)
		|| !ParseBoundedInteger(
			Params,
			TEXT("limit"),
			100,
			1,
			MaxRowsPerResponse,
			Limit,
			Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = GetChooserRowCount(Chooser);
	if (StartRow > RowCount)
	{
		return InvalidParam(
			TEXT("start_row"),
			FString::Printf(TEXT("expected a row index in the range 0..%d"), RowCount));
	}

	TArray<TSharedPtr<FJsonValue>> Rows = SerializeRows(Chooser, StartRow, Limit);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("start_row"), StartRow);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetBoolField(TEXT("has_more"), StartRow + Rows.Num() < RowCount);
	Result->SetArrayField(TEXT("rows"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserReferences(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	int32 Offset = 0;
	int32 Limit = 200;
	FMonolithActionResult Error;
	if (!ParseBoundedInteger(
		Params,
		TEXT("offset"),
		0,
		0,
		MaxReferencesPerScan,
		Offset,
		Error)
		|| !ParseBoundedInteger(
			Params,
			TEXT("limit"),
			200,
			1,
			1000,
			Limit,
			Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	FReferenceScan Scan = CollectChooserReferences(Chooser);
	const int32 Total = Scan.Values.Num();
	const int32 End = FMath::Min(Total, Offset + Limit);
	TArray<TSharedPtr<FJsonValue>> Page;
	if (Offset < Total)
	{
		Page.Append(&Scan.Values[Offset], End - Offset);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("count"), Page.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetBoolField(TEXT("has_more"), Offset + Page.Num() < Total);
	Result->SetBoolField(TEXT("scan_truncated"), Scan.bTruncated);
	Result->SetArrayField(TEXT("references"), Page);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleValidateChooserTable(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FString PackagePath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = GetChooserRowCount(Chooser);
	const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	if (RowCount == 0)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("warning"),
			TEXT("empty_rows"),
			TEXT("ChooserTable has no rows."));
	}
	if (ColumnCount == 0)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("warning"),
			TEXT("empty_columns"),
			TEXT("ChooserTable has no columns."));
	}
	if (ColumnCount > MaxColumns)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("error"),
			TEXT("column_limit_exceeded"),
			FString::Printf(
				TEXT("ChooserTable has %d columns; bounded validation supports at most %d."),
				ColumnCount,
				MaxColumns));
	}

	int32 ResultsCount = 0;
	if (TryGetArrayNum(Chooser, TEXT("ResultsStructs"), ResultsCount)
		&& ResultsCount != RowCount)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("error"),
			TEXT("results_row_count_mismatch"),
			FString::Printf(
				TEXT("ResultsStructs has %d entries for %d reflected rows."),
				ResultsCount,
				RowCount));
	}

	int32 DisabledCount = 0;
	if (TryGetArrayNum(Chooser, TEXT("DisabledRows"), DisabledCount)
		&& DisabledCount != RowCount)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("error"),
			TEXT("disabled_row_count_mismatch"),
			FString::Printf(
				TEXT("DisabledRows has %d entries for %d reflected rows."),
				DisabledCount,
				RowCount));
	}

	const int32 ColumnsToValidate = FMath::Min(ColumnCount, MaxColumns);
	for (int32 ColumnIndex = 0; ColumnIndex < ColumnsToValidate; ++ColumnIndex)
	{
		const FInstancedStruct* Column =
			GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), ColumnIndex);
		if (!Column || !Column->IsValid())
		{
			AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("error"),
				TEXT("invalid_column_struct"),
				FString::Printf(TEXT("Column %d has no valid reflected struct."), ColumnIndex));
			continue;
		}

		if (!FindRowValuesProperty(Column->GetScriptStruct()))
		{
			AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("warning"),
				TEXT("column_has_no_reflected_row_values"),
				FString::Printf(
					TEXT("Column %d (%s) exposes no reflected row-value array."),
					ColumnIndex,
					Column->GetScriptStruct()
						? *Column->GetScriptStruct()->GetName()
						: TEXT("Unknown")));
			continue;
		}

		const int32 ValueCount = GetRowValuesCount(*Column);
		if (ValueCount != RowCount)
		{
			AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("error"),
				TEXT("column_row_count_mismatch"),
				FString::Printf(
					TEXT("Column %d has %d row values for %d reflected rows."),
					ColumnIndex,
					ValueCount,
					RowCount));
		}
	}

	FReferenceScan References = CollectChooserReferences(Chooser);
	for (const TSharedPtr<FJsonValue>& ReferenceValue : References.Values)
	{
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		if (!ReferenceValue.IsValid()
			|| !ReferenceValue->TryGetObject(Reference)
			|| !Reference
			|| !Reference->IsValid())
		{
			continue;
		}

		bool Soft = false;
		bool Exists = true;
		(*Reference)->TryGetBoolField(TEXT("soft"), Soft);
		(*Reference)->TryGetBoolField(TEXT("exists"), Exists);
		if (Soft && !Exists)
		{
			FString Path;
			(*Reference)->TryGetStringField(TEXT("path"), Path);
			AddIssue(
				Issues,
				ErrorCount,
				WarningCount,
				TEXT("error"),
				TEXT("unresolved_soft_reference"),
				FString::Printf(TEXT("Soft-reference package does not resolve: %s"), *Path));
		}
	}
	if (References.bTruncated)
	{
		AddIssue(
			Issues,
			ErrorCount,
			WarningCount,
			TEXT("error"),
			TEXT("reference_scan_truncated"),
			TEXT("Reference validation exceeded its bounded scan limit; validity cannot be proven."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);
	Result->SetBoolField(TEXT("complete"), !References.bTruncated && ColumnCount <= MaxColumns);
	Result->SetBoolField(TEXT("valid"), ErrorCount == 0);
	return FMonolithActionResult::Success(Result);
}
