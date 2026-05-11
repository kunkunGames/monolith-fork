#include "MonolithAIChooserActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	static FTopLevelAssetPath GetChooserTableClassPath()
	{
		return FTopLevelAssetPath(TEXT("/Script/Chooser"), TEXT("ChooserTable"));
	}

	static UClass* LoadChooserTableClass()
	{
		UClass* ChooserClass = FindObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
		if (!ChooserClass)
		{
			ChooserClass = LoadObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
		}
		return ChooserClass;
	}

	static bool IsChooserClassAvailable()
	{
		return LoadChooserTableClass() != nullptr;
	}

	static FString NormalizePathFilter(FString PathFilter)
	{
		PathFilter.TrimStartAndEndInline();
		PathFilter.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!PathFilter.IsEmpty() && !PathFilter.StartsWith(TEXT("/")))
		{
			PathFilter = TEXT("/Game/") + PathFilter;
		}
		return PathFilter;
	}

	static TSharedPtr<FJsonObject> ObjectRefToJson(const UObject* Obj)
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

	static TSharedPtr<FJsonObject> SoftObjectRefToJson(const FSoftObjectPath& Path)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("path"), Path.ToString());
		Ref->SetStringField(TEXT("asset_path"), Path.GetAssetPathString());
		Ref->SetStringField(TEXT("sub_path"), Path.GetSubPathString());
		Ref->SetBoolField(TEXT("valid"), Path.IsValid());
		Ref->SetBoolField(TEXT("exists"), Path.IsValid() && FMonolithAssetUtils::AssetExists(Path.GetAssetPathString()));
		return Ref;
	}

	static FString ExportPropertyValue(const FProperty* Property, const void* ValuePtr)
	{
		if (!Property || !ValuePtr)
		{
			return FString();
		}

		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return Exported;
	}

	static TSharedPtr<FJsonObject> SerializeStructFields(
		const UStruct* Struct,
		const void* StructMemory,
		int32 MaxDepth,
		bool bCompact);

	static TSharedPtr<FJsonObject> SerializeInstancedStruct(
		const FInstancedStruct& Instance,
		int32 MaxDepth,
		bool bCompact)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Instance.IsValid());

		const UScriptStruct* ScriptStruct = Instance.GetScriptStruct();
		Obj->SetStringField(TEXT("script_struct"), ScriptStruct ? ScriptStruct->GetName() : FString());
		Obj->SetStringField(TEXT("display_name"), ScriptStruct ? ScriptStruct->GetDisplayNameText().ToString() : FString());

		if (ScriptStruct && Instance.GetMemory())
		{
			Obj->SetObjectField(TEXT("fields"), SerializeStructFields(ScriptStruct, Instance.GetMemory(), MaxDepth, bCompact));
		}

		return Obj;
	}

	static TSharedPtr<FJsonValue> PropertyValueToJson(
		const FProperty* Property,
		const void* ValuePtr,
		int32 MaxDepth,
		bool bCompact)
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

		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			if (NumProp->IsInteger())
			{
				const bool bUnsigned =
					CastField<FUInt16Property>(Property) ||
					CastField<FUInt32Property>(Property) ||
					CastField<FUInt64Property>(Property);
				if (bUnsigned)
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetUnsignedIntPropertyValue(ValuePtr)));
				}
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
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
			const FSoftObjectPtr SoftPtr = SoftObjectProp->GetPropertyValue(ValuePtr);
			return MakeShared<FJsonValueObject>(SoftObjectRefToJson(SoftPtr.ToSoftObjectPath()));
		}

		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
		{
			return MakeShared<FJsonValueObject>(ObjectRefToJson(ObjectProp->GetObjectPropertyValue(ValuePtr)));
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (StructProp->Struct == FInstancedStruct::StaticStruct())
			{
				return MakeShared<FJsonValueObject>(
					SerializeInstancedStruct(*reinterpret_cast<const FInstancedStruct*>(ValuePtr), MaxDepth - 1, bCompact));
			}

			if (MaxDepth <= 0)
			{
				return MakeShared<FJsonValueString>(ExportPropertyValue(Property, ValuePtr));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("script_struct"), StructProp->Struct ? StructProp->Struct->GetName() : FString());
			Obj->SetObjectField(TEXT("fields"), SerializeStructFields(StructProp->Struct, ValuePtr, MaxDepth - 1, bCompact));
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			const int32 Limit = bCompact ? FMath::Min(Helper.Num(), 8) : Helper.Num();
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Limit);
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				Values.Add(PropertyValueToJson(ArrayProp->Inner, Helper.GetRawPtr(Index), MaxDepth - 1, bCompact));
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("count"), Helper.Num());
			Obj->SetArrayField(TEXT("items"), Values);
			if (Limit < Helper.Num())
			{
				Obj->SetNumberField(TEXT("truncated_after"), Limit);
			}
			return MakeShared<FJsonValueObject>(Obj);
		}

		return MakeShared<FJsonValueString>(ExportPropertyValue(Property, ValuePtr));
	}

	static TSharedPtr<FJsonObject> SerializeStructFields(
		const UStruct* Struct,
		const void* StructMemory,
		int32 MaxDepth,
		bool bCompact)
	{
		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (!Struct || !StructMemory)
		{
			return Fields;
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructMemory);
			Fields->SetField(Property->GetName(), PropertyValueToJson(Property, ValuePtr, MaxDepth, bCompact));
		}

		return Fields;
	}

	static const FArrayProperty* FindArrayProperty(const UStruct* Struct, const TCHAR* PropertyName)
	{
		return Struct ? FindFProperty<FArrayProperty>(Struct, PropertyName) : nullptr;
	}

	static int32 GetArrayNum(const UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return 0;
		}
		const FArrayProperty* ArrayProp = FindArrayProperty(Object->GetClass(), PropertyName);
		if (!ArrayProp)
		{
			return 0;
		}
		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	}

	static bool GetBoolArrayValue(const UObject* Object, const TCHAR* PropertyName, int32 Index)
	{
		if (!Object)
		{
			return false;
		}

		const FArrayProperty* ArrayProp = FindArrayProperty(Object->GetClass(), PropertyName);
		const FBoolProperty* InnerBool = ArrayProp ? CastField<FBoolProperty>(ArrayProp->Inner) : nullptr;
		if (!ArrayProp || !InnerBool)
		{
			return false;
		}

		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Object));
		if (Index < 0 || Index >= Helper.Num())
		{
			return false;
		}
		return InnerBool->GetPropertyValue(Helper.GetRawPtr(Index));
	}

	static const FInstancedStruct* GetInstancedStructFromArray(const UObject* Object, const TCHAR* PropertyName, int32 Index)
	{
		if (!Object)
		{
			return nullptr;
		}

		const FArrayProperty* ArrayProp = FindArrayProperty(Object->GetClass(), PropertyName);
		const FStructProperty* InnerStruct = ArrayProp ? CastField<FStructProperty>(ArrayProp->Inner) : nullptr;
		if (!ArrayProp || !InnerStruct || InnerStruct->Struct != FInstancedStruct::StaticStruct())
		{
			return nullptr;
		}

		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Object));
		if (Index < 0 || Index >= Helper.Num())
		{
			return nullptr;
		}
		return reinterpret_cast<const FInstancedStruct*>(Helper.GetRawPtr(Index));
	}

	static FArrayProperty* FindRowValuesProperty(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}

		if (FArrayProperty* Exact = FindFProperty<FArrayProperty>(Struct, TEXT("RowValues")))
		{
			return Exact;
		}

		if (FArrayProperty* WithAny = FindFProperty<FArrayProperty>(Struct, TEXT("RowValuesWithAny")))
		{
			return WithAny;
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(*It))
			{
				if (ArrayProp->GetName().Contains(TEXT("RowValues")))
				{
					return ArrayProp;
				}
			}
		}

		return nullptr;
	}

	static int32 GetRowValuesCount(const FInstancedStruct& Column)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		FArrayProperty* RowValues = FindRowValuesProperty(Struct);
		if (!RowValues || !Column.GetMemory())
		{
			return 0;
		}

		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		return Helper.Num();
	}

	static TSharedPtr<FJsonValue> GetRowValueAt(const FInstancedStruct& Column, int32 RowIndex)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		FArrayProperty* RowValues = FindRowValuesProperty(Struct);
		if (!RowValues || !Column.GetMemory())
		{
			return MakeShared<FJsonValueNull>();
		}

		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		if (RowIndex < 0 || RowIndex >= Helper.Num())
		{
			return MakeShared<FJsonValueNull>();
		}

		return PropertyValueToJson(RowValues->Inner, Helper.GetRawPtr(RowIndex), 2, false);
	}

	static bool GetColumnBoolField(const FInstancedStruct& Column, const TCHAR* PropertyName, bool bDefault = false)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		const FBoolProperty* BoolProp = Struct ? FindFProperty<FBoolProperty>(Struct, PropertyName) : nullptr;
		if (!BoolProp || !Column.GetMemory())
		{
			return bDefault;
		}
		return BoolProp->GetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(Column.GetMemory()));
	}

	static bool IsOutputColumn(const FInstancedStruct& Column)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		if (!Struct)
		{
			return false;
		}

		const FString Name = Struct->GetName();
		return Name.StartsWith(TEXT("Output")) || FindFProperty<FProperty>(Struct, TEXT("FallbackValue")) != nullptr;
	}

	static TSharedPtr<FJsonObject> SerializeColumnSummary(const FInstancedStruct& Column, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = SerializeInstancedStruct(Column, 2, true);
		Obj->SetNumberField(TEXT("index"), Index);
		Obj->SetBoolField(TEXT("is_output"), IsOutputColumn(Column));
		Obj->SetBoolField(TEXT("is_disabled"), GetColumnBoolField(Column, TEXT("bDisabled")));

		if (FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct()))
		{
			Obj->SetStringField(TEXT("row_values_property"), RowValues->GetName());
			Obj->SetStringField(TEXT("row_values_type"), RowValues->Inner ? RowValues->Inner->GetCPPType() : FString());
			Obj->SetNumberField(TEXT("row_value_count"), GetRowValuesCount(Column));
		}
		else
		{
			Obj->SetStringField(TEXT("row_values_property"), FString());
			Obj->SetNumberField(TEXT("row_value_count"), 0);
		}
		return Obj;
	}

	static int32 GetChooserRowCount(const UObject* Chooser)
	{
		const int32 EditorRows = GetArrayNum(Chooser, TEXT("ResultsStructs"));
		if (EditorRows > 0)
		{
			return EditorRows;
		}

		const int32 CookedRows = GetArrayNum(Chooser, TEXT("CookedResults"));
		if (CookedRows > 0)
		{
			return CookedRows;
		}

		int32 MaxRows = 0;
		const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
		for (int32 Index = 0; Index < ColumnCount; ++Index)
		{
			if (const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), Index))
			{
				MaxRows = FMath::Max(MaxRows, GetRowValuesCount(*Column));
			}
		}
		return MaxRows;
	}

	static UObject* LoadChooserFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		Params->TryGetStringField(TEXT("asset_path"), OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}

		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
		UClass* ChooserClass = LoadChooserTableClass();
		if (!ChooserClass)
		{
			OutError = TEXT("ChooserTable class is unavailable. Enable the engine Chooser plugin to inspect Chooser assets.");
			return nullptr;
		}

		UObject* Chooser = FMonolithAssetUtils::LoadAssetByPath(ChooserClass, OutAssetPath);
		if (!Chooser)
		{
			OutError = FString::Printf(TEXT("ChooserTable asset not found at '%s'"), *OutAssetPath);
		}
		return Chooser;
	}

	static TArray<TSharedPtr<FJsonValue>> GetChooserAssets(FString PathFilter)
	{
		PathFilter = NormalizePathFilter(PathFilter);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(GetChooserTableClassPath(), Assets, true);

		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FAssetData& Asset : Assets)
		{
			const FString PackageName = Asset.PackageName.ToString();
			const FString ObjectPath = Asset.GetObjectPathString();
			if (!PathFilter.IsEmpty() && !PackageName.StartsWith(PathFilter) && !ObjectPath.StartsWith(PathFilter))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), PackageName);
			Entry->SetStringField(TEXT("object_path"), ObjectPath);
			Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Result.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Result;
	}

	static void AddReference(
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		const FString& Source,
		const FString& Path,
		const FString& ClassName,
		bool bLoaded,
		bool bSoft)
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
			const FString AssetPath = FSoftObjectPath(Path).GetAssetPathString();
			Ref->SetStringField(TEXT("asset_path"), AssetPath);
			Ref->SetBoolField(TEXT("exists"), AssetPath.StartsWith(TEXT("/Game")) ? FMonolithAssetUtils::AssetExists(AssetPath) : true);
		}
		OutRefs.Add(MakeShared<FJsonValueObject>(Ref));
	}

	static void CollectReferencesFromStruct(
		const UStruct* Struct,
		const void* StructMemory,
		const FString& Source,
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		int32 MaxDepth);

	static void CollectReferencesFromProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const FString& Source,
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		int32 MaxDepth)
	{
		if (!Property || !ValuePtr || MaxDepth < 0)
		{
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

		if (const FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath Path = SoftObjectProp->GetPropertyValue(ValuePtr).ToSoftObjectPath();
			if (Path.IsValid())
			{
				AddReference(OutRefs, Seen, Source + TEXT(".") + Property->GetName(), Path.ToString(), TEXT("soft_object"), false, true);
			}
			return;
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (StructProp->Struct == FInstancedStruct::StaticStruct())
			{
				const FInstancedStruct& Instance = *reinterpret_cast<const FInstancedStruct*>(ValuePtr);
				if (Instance.IsValid())
				{
					CollectReferencesFromStruct(Instance.GetScriptStruct(), Instance.GetMemory(), Source + TEXT(".") + Property->GetName(), OutRefs, Seen, MaxDepth - 1);
				}
				return;
			}

			CollectReferencesFromStruct(StructProp->Struct, ValuePtr, Source + TEXT(".") + Property->GetName(), OutRefs, Seen, MaxDepth - 1);
			return;
		}

		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				CollectReferencesFromProperty(
					ArrayProp->Inner,
					Helper.GetRawPtr(Index),
					FString::Printf(TEXT("%s.%s[%d]"), *Source, *Property->GetName(), Index),
					OutRefs,
					Seen,
					MaxDepth - 1);
			}
		}
	}

	static void CollectReferencesFromStruct(
		const UStruct* Struct,
		const void* StructMemory,
		const FString& Source,
		TArray<TSharedPtr<FJsonValue>>& OutRefs,
		TSet<FString>& Seen,
		int32 MaxDepth)
	{
		if (!Struct || !StructMemory || MaxDepth < 0)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			CollectReferencesFromProperty(
				Property,
				Property->ContainerPtrToValuePtr<void>(StructMemory),
				Source,
				OutRefs,
				Seen,
				MaxDepth);
		}
	}

	static TArray<TSharedPtr<FJsonValue>> CollectChooserReferences(const UObject* Chooser)
	{
		TArray<TSharedPtr<FJsonValue>> Refs;
		TSet<FString> Seen;
		CollectReferencesFromStruct(Chooser ? Chooser->GetClass() : nullptr, Chooser, TEXT("chooser"), Refs, Seen, 8);
		return Refs;
	}

	static void AddStringIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Message,
		const FString& Code)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		Issue->SetStringField(TEXT("code"), Code);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}
}

void FMonolithAIChooserActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_tables"),
		TEXT("List ChooserTable assets without hard-linking the Chooser plugin."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include ChooserTables under this package prefix"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("get_chooser_table"),
		TEXT("Inspect a ChooserTable summary: rows, columns, results, context fields, and references."),
		FMonolithActionHandler::CreateStatic(&HandleGetChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ChooserTable asset path"))
			.Optional(TEXT("include_rows"), TEXT("boolean"), TEXT("Include row details in the response"), TEXT("false"))
			.Optional(TEXT("row_limit"), TEXT("number"), TEXT("Maximum rows to include when include_rows=true"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_columns"),
		TEXT("Return reflected ChooserTable columns with input/output type and row-value counts."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserColumns),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ChooserTable asset path"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_rows"),
		TEXT("Return reflected ChooserTable rows, per-column cells, disabled state, and row result data."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserRows),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ChooserTable asset path"))
			.Optional(TEXT("start_row"), TEXT("number"), TEXT("First row index"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum row count"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_references"),
		TEXT("List object and soft-object references found in a ChooserTable."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserReferences),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ChooserTable asset path"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("validate_chooser_table"),
		TEXT("Validate ChooserTable row/column consistency and unresolved reflected references."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ChooserTable asset path"))
			.Build());
}

FMonolithActionResult FMonolithAIChooserActions::HandleListChooserTables(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);

	TArray<TSharedPtr<FJsonValue>> Tables = GetChooserAssets(PathFilter);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("available"), IsChooserClassAvailable() || Tables.Num() > 0);
	Result->SetStringField(TEXT("class_path"), GetChooserTableClassPath().ToString());
	Result->SetArrayField(TEXT("tables"), Tables);
	Result->SetNumberField(TEXT("count"), Tables.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIChooserActions::HandleGetChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Chooser = LoadChooserFromParams(Params, AssetPath, Error);
	if (!Chooser)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeRows = false;
	Params->TryGetBoolField(TEXT("include_rows"), bIncludeRows);

	double RowLimitNumber = 50.0;
	Params->TryGetNumberField(TEXT("row_limit"), RowLimitNumber);
	const int32 RowLimit = FMath::Clamp(static_cast<int32>(RowLimitNumber), 0, 500);

	const int32 RowCount = GetChooserRowCount(Chooser);
	const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), Chooser->GetPathName());
	Result->SetStringField(TEXT("class"), Chooser->GetClass()->GetName());
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetNumberField(TEXT("result_count"), GetArrayNum(Chooser, TEXT("ResultsStructs")));
	Result->SetNumberField(TEXT("cooked_result_count"), GetArrayNum(Chooser, TEXT("CookedResults")));
	Result->SetNumberField(TEXT("disabled_row_count"), GetArrayNum(Chooser, TEXT("DisabledRows")));
	Result->SetNumberField(TEXT("nested_chooser_count"), GetArrayNum(Chooser, TEXT("NestedChoosers")));
	Result->SetNumberField(TEXT("nested_object_count"), GetArrayNum(Chooser, TEXT("NestedObjects")));
	Result->SetNumberField(TEXT("context_entry_count"), GetArrayNum(Chooser, TEXT("ContextData")));

	if (const FStructProperty* FallbackProp = FindFProperty<FStructProperty>(Chooser->GetClass(), TEXT("FallbackResult")))
	{
		const void* FallbackPtr = FallbackProp->ContainerPtrToValuePtr<void>(Chooser);
		Result->SetField(TEXT("fallback_result"), PropertyValueToJson(FallbackProp, FallbackPtr, 2, true));
	}

	TArray<TSharedPtr<FJsonValue>> Columns;
	Columns.Reserve(ColumnCount);
	for (int32 Index = 0; Index < ColumnCount; ++Index)
	{
		if (const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), Index))
		{
			Columns.Add(MakeShared<FJsonValueObject>(SerializeColumnSummary(*Column, Index)));
		}
	}
	Result->SetArrayField(TEXT("columns"), Columns);

	Result->SetArrayField(TEXT("references"), CollectChooserReferences(Chooser));

	if (bIncludeRows)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		const int32 Limit = FMath::Min(RowCount, RowLimit);
		for (int32 RowIndex = 0; RowIndex < Limit; ++RowIndex)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("row_index"), RowIndex);
			Row->SetBoolField(TEXT("disabled"), GetBoolArrayValue(Chooser, TEXT("DisabledRows"), RowIndex));

			if (const FInstancedStruct* ResultStruct = GetInstancedStructFromArray(Chooser, TEXT("ResultsStructs"), RowIndex))
			{
				Row->SetObjectField(TEXT("result"), SerializeInstancedStruct(*ResultStruct, 2, false));
			}

			TArray<TSharedPtr<FJsonValue>> Cells;
			for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
			{
				if (const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), ColumnIndex))
				{
					TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
					Cell->SetNumberField(TEXT("column_index"), ColumnIndex);
					Cell->SetStringField(TEXT("column_type"), Column->GetScriptStruct() ? Column->GetScriptStruct()->GetName() : FString());
					Cell->SetField(TEXT("value"), GetRowValueAt(*Column, RowIndex));
					Cells.Add(MakeShared<FJsonValueObject>(Cell));
				}
			}
			Row->SetArrayField(TEXT("cells"), Cells);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Result->SetArrayField(TEXT("rows"), Rows);
		Result->SetNumberField(TEXT("rows_returned"), Rows.Num());
		if (Limit < RowCount)
		{
			Result->SetNumberField(TEXT("rows_truncated_after"), Limit);
		}
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIChooserActions::HandleListChooserColumns(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Chooser = LoadChooserFromParams(Params, AssetPath, Error);
	if (!Chooser)
	{
		return FMonolithActionResult::Error(Error);
	}

	const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
	TArray<TSharedPtr<FJsonValue>> Columns;
	Columns.Reserve(ColumnCount);

	for (int32 Index = 0; Index < ColumnCount; ++Index)
	{
		if (const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), Index))
		{
			Columns.Add(MakeShared<FJsonValueObject>(SerializeColumnSummary(*Column, Index)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("columns"), Columns);
	Result->SetNumberField(TEXT("count"), Columns.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIChooserActions::HandleListChooserRows(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Chooser = LoadChooserFromParams(Params, AssetPath, Error);
	if (!Chooser)
	{
		return FMonolithActionResult::Error(Error);
	}

	double StartNumber = 0.0;
	double LimitNumber = 100.0;
	Params->TryGetNumberField(TEXT("start_row"), StartNumber);
	Params->TryGetNumberField(TEXT("limit"), LimitNumber);

	const int32 RowCount = GetChooserRowCount(Chooser);
	const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
	const int32 StartRow = FMath::Clamp(static_cast<int32>(StartNumber), 0, RowCount);
	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 0, 500);
	const int32 EndRow = FMath::Min(RowCount, StartRow + Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 RowIndex = StartRow; RowIndex < EndRow; ++RowIndex)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("row_index"), RowIndex);
		Row->SetBoolField(TEXT("disabled"), GetBoolArrayValue(Chooser, TEXT("DisabledRows"), RowIndex));

		if (const FInstancedStruct* ResultStruct = GetInstancedStructFromArray(Chooser, TEXT("ResultsStructs"), RowIndex))
		{
			Row->SetObjectField(TEXT("result"), SerializeInstancedStruct(*ResultStruct, 2, false));
		}

		TArray<TSharedPtr<FJsonValue>> Cells;
		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			if (const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), ColumnIndex))
			{
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("column_index"), ColumnIndex);
				Cell->SetStringField(TEXT("column_type"), Column->GetScriptStruct() ? Column->GetScriptStruct()->GetName() : FString());
				Cell->SetBoolField(TEXT("is_output"), IsOutputColumn(*Column));
				Cell->SetField(TEXT("value"), GetRowValueAt(*Column, RowIndex));
				Cells.Add(MakeShared<FJsonValueObject>(Cell));
			}
		}
		Row->SetArrayField(TEXT("cells"), Cells);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("start_row"), StartRow);
	Result->SetNumberField(TEXT("rows_returned"), Rows.Num());
	Result->SetArrayField(TEXT("rows"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIChooserActions::HandleListChooserReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Chooser = LoadChooserFromParams(Params, AssetPath, Error);
	if (!Chooser)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Refs = CollectChooserReferences(Chooser);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("references"), Refs);
	Result->SetNumberField(TEXT("count"), Refs.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAIChooserActions::HandleValidateChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Chooser = LoadChooserFromParams(Params, AssetPath, Error);
	if (!Chooser)
	{
		return FMonolithActionResult::Error(Error);
	}

	const int32 RowCount = GetChooserRowCount(Chooser);
	const int32 ColumnCount = GetArrayNum(Chooser, TEXT("ColumnsStructs"));
	TArray<TSharedPtr<FJsonValue>> Issues;

	if (RowCount == 0)
	{
		AddStringIssue(Issues, TEXT("warning"), TEXT("ChooserTable has no rows."), TEXT("empty_rows"));
	}
	if (ColumnCount == 0)
	{
		AddStringIssue(Issues, TEXT("warning"), TEXT("ChooserTable has no columns."), TEXT("empty_columns"));
	}

	for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
	{
		const FInstancedStruct* Column = GetInstancedStructFromArray(Chooser, TEXT("ColumnsStructs"), ColumnIndex);
		if (!Column || !Column->IsValid())
		{
			AddStringIssue(Issues, TEXT("error"), FString::Printf(TEXT("Column %d has no valid reflected struct."), ColumnIndex), TEXT("invalid_column_struct"));
			continue;
		}

		FArrayProperty* RowValues = FindRowValuesProperty(Column->GetScriptStruct());
		if (!RowValues)
		{
			AddStringIssue(Issues, TEXT("warning"), FString::Printf(TEXT("Column %d (%s) exposes no row-value array via reflection."),
				ColumnIndex,
				Column->GetScriptStruct() ? *Column->GetScriptStruct()->GetName() : TEXT("Unknown")), TEXT("unsupported_column_row_values"));
			continue;
		}

		const int32 ValueCount = GetRowValuesCount(*Column);
		if (ValueCount < RowCount)
		{
			AddStringIssue(Issues, TEXT("warning"), FString::Printf(TEXT("Column %d has %d row values for %d rows."), ColumnIndex, ValueCount, RowCount), TEXT("short_column_values"));
		}
	}

	const TArray<TSharedPtr<FJsonValue>> Refs = CollectChooserReferences(Chooser);
	for (const TSharedPtr<FJsonValue>& RefValue : Refs)
	{
		const TSharedPtr<FJsonObject>* RefObj = nullptr;
		if (!RefValue.IsValid() || !RefValue->TryGetObject(RefObj) || !RefObj || !RefObj->IsValid())
		{
			continue;
		}

		bool bSoft = false;
		(*RefObj)->TryGetBoolField(TEXT("soft"), bSoft);
		bool bExists = true;
		(*RefObj)->TryGetBoolField(TEXT("exists"), bExists);
		FString Path;
		(*RefObj)->TryGetStringField(TEXT("path"), Path);
		if (bSoft && !bExists)
		{
			AddStringIssue(Issues, TEXT("error"), FString::Printf(TEXT("Soft reference does not resolve: %s"), *Path), TEXT("unresolved_soft_reference"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	return FMonolithActionResult::Success(Result);
}
