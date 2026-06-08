#include "MonolithWorldConditionsActions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if WITH_MONOLITH_WORLDCONDITIONS
#include "StructUtils/InstancedStruct.h"
#include "WorldConditionBase.h"
#include "WorldConditionQuery.h"
#endif

#if WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS
#include "SmartObjectDefinition.h"
#endif

namespace
{
constexpr int32 DefaultOwnerLimit = 100;
constexpr int32 MaxOwnerLimit = 500;
constexpr int32 DefaultConditionTypeLimit = 128;
constexpr int32 MaxConditionTypeLimit = 500;
constexpr int32 MaxConditionRows = 64;
constexpr int32 MaxSafeProperties = 64;
constexpr int32 MaxTextValueLen = 256;

int32 ReadLimit(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MaxValue)
{
	int32 Limit = DefaultValue;
	if (Params.IsValid() && Params->HasTypedField<EJson::Number>(FieldName))
	{
		Limit = static_cast<int32>(Params->GetNumberField(FieldName));
	}
	return FMath::Clamp(Limit, 1, MaxValue);
}

FString ReadStringOrDefault(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue)
{
	if (Params.IsValid() && Params->HasTypedField<EJson::String>(FieldName))
	{
		return Params->GetStringField(FieldName);
	}
	return DefaultValue;
}

FString NormalizePackagePath(FString Path)
{
	Path.TrimStartAndEndInline();
	if (Path.IsEmpty())
	{
		return TEXT("/Game");
	}
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/") + Path;
	}
	if (Path.EndsWith(TEXT("/")) && Path.Len() > 1)
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

bool IsFeatureEnabled()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	return Settings && Settings->bEnableWorldConditionsInspection;
}

TSharedPtr<FJsonObject> MakeBaseStatus()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("enabled"), IsFeatureEnabled());
	Result->SetBoolField(TEXT("with_world_conditions"), WITH_MONOLITH_WORLDCONDITIONS != 0);
	Result->SetBoolField(TEXT("with_smartobjects"), WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS != 0);
	Result->SetBoolField(TEXT("world_conditions_module_exists"), FModuleManager::Get().ModuleExists(TEXT("WorldConditions")));
	Result->SetBoolField(TEXT("smartobjects_module_exists"), FModuleManager::Get().ModuleExists(TEXT("SmartObjectsModule")));
	return Result;
}

void AddUnavailableFields(TSharedPtr<FJsonObject>& Result)
{
	Result->SetStringField(TEXT("dependency_state"), TEXT("unavailable"));
	Result->SetStringField(TEXT("reason"), TEXT("WorldConditions or SmartObjects optional dependency is not compiled into this Monolith build."));
}

FTopLevelAssetPath SmartObjectDefinitionClassPath()
{
#if WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS
	return USmartObjectDefinition::StaticClass()->GetClassPathName();
#else
	return FTopLevelAssetPath(TEXT("/Script/SmartObjectsModule"), TEXT("SmartObjectDefinition"));
#endif
}

TArray<FAssetData> CollectSmartObjectDefinitionAssets(const FString& PathFilter, int32 Limit)
{
	TArray<FAssetData> Assets;
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*NormalizePackagePath(PathFilter)));
	Filter.ClassPaths.Add(SmartObjectDefinitionClassPath());
	Filter.bRecursivePaths = true;
	AssetRegistry.GetAssets(Filter, Assets);

	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.PackageName.LexicalLess(B.PackageName);
	});

	if (Assets.Num() > Limit)
	{
		Assets.SetNum(Limit);
	}
	return Assets;
}

TSharedPtr<FJsonObject> AssetDataToOwnerJson(const FAssetData& AssetData)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_path"), AssetData.GetSoftObjectPath().ToString());
	Obj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
	Obj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
	Obj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
	Obj->SetStringField(TEXT("owner_shape"), TEXT("SmartObjectDefinition"));
	Obj->SetBoolField(TEXT("supports_object_preconditions"), true);
	Obj->SetBoolField(TEXT("supports_slot_selection_preconditions"), true);
	return Obj;
}

FString ExportPropertyToString(const FProperty* Property, const void* ValuePtr, bool& bOutTruncated)
{
	bOutTruncated = false;
	if (!Property || !ValuePtr)
	{
		return FString();
	}

	FString Text;
	Property->ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	if (Text.Len() > MaxTextValueLen)
	{
		Text = Text.Left(MaxTextValueLen) + TEXT("...");
		bOutTruncated = true;
	}
	return Text;
}

bool WriteSafePropertyValue(const FProperty* Property, const void* StructMemory, TSharedPtr<FJsonObject>& OutProperties, bool& bOutTruncated)
{
	if (!Property || !StructMemory || !OutProperties.IsValid())
	{
		return false;
	}

	if (Property->IsA<FArrayProperty>() || Property->IsA<FMapProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>())
	{
		return false;
	}

	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructMemory);
	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		OutProperties->SetBoolField(Property->GetName(), BoolProp->GetPropertyValue(ValuePtr));
		return true;
	}

	if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
	{
		if (NumProp->IsFloatingPoint())
		{
			OutProperties->SetNumberField(Property->GetName(), NumProp->GetFloatingPointPropertyValue(ValuePtr));
		}
		else
		{
			OutProperties->SetNumberField(Property->GetName(), static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}
		return true;
	}

	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		OutProperties->SetStringField(Property->GetName(), EnumProp->GetEnum()->GetNameStringByValue(Value));
		return true;
	}

	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProp->Enum)
		{
			OutProperties->SetStringField(Property->GetName(), Enum->GetNameStringByValue(ByteProp->GetPropertyValue(ValuePtr)));
		}
		else
		{
			OutProperties->SetNumberField(Property->GetName(), ByteProp->GetPropertyValue(ValuePtr));
		}
		return true;
	}

	if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		OutProperties->SetStringField(Property->GetName(), NameProp->GetPropertyValue(ValuePtr).ToString());
		return true;
	}

	if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString Value = StrProp->GetPropertyValue(ValuePtr);
		if (Value.Len() > MaxTextValueLen)
		{
			Value = Value.Left(MaxTextValueLen) + TEXT("...");
			bOutTruncated = true;
		}
		OutProperties->SetStringField(Property->GetName(), Value);
		return true;
	}

	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		FString Value = TextProp->GetPropertyValue(ValuePtr).ToString();
		if (Value.Len() > MaxTextValueLen)
		{
			Value = Value.Left(MaxTextValueLen) + TEXT("...");
			bOutTruncated = true;
		}
		OutProperties->SetStringField(Property->GetName(), Value);
		return true;
	}

	if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
	{
		const UObject* ObjectValue = ObjectProp->GetObjectPropertyValue(ValuePtr);
		OutProperties->SetStringField(Property->GetName(), ObjectValue ? ObjectValue->GetPathName() : FString());
		return true;
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		bool bValueTruncated = false;
		const FString Exported = ExportPropertyToString(StructProp, ValuePtr, bValueTruncated);
		bOutTruncated = bOutTruncated || bValueTruncated;
		OutProperties->SetStringField(Property->GetName(), Exported);
		return true;
	}

	return false;
}

#if WITH_MONOLITH_WORLDCONDITIONS
FString OperatorToString(EWorldConditionOperator Operator)
{
	switch (Operator)
	{
	case EWorldConditionOperator::And:
		return TEXT("and");
	case EWorldConditionOperator::Or:
		return TEXT("or");
	case EWorldConditionOperator::Copy:
		return TEXT("copy");
	default:
		return TEXT("unknown");
	}
}

void AddConditionStructMetadata(const UScriptStruct* Struct, TSharedPtr<FJsonObject>& Obj)
{
	if (!Struct || !Obj.IsValid())
	{
		return;
	}

	Obj->SetStringField(TEXT("type"), Struct->GetName());
	Obj->SetStringField(TEXT("path"), Struct->GetPathName());
	Obj->SetStringField(TEXT("display_name"), Struct->GetDisplayNameText().ToString());
}

TSharedPtr<FJsonObject> ConditionToJson(const FInstancedStruct& Condition, int32 Index, uint8 ExpressionDepth, EWorldConditionOperator Operator, bool bInvert)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("index"), Index);
	Row->SetNumberField(TEXT("expression_depth"), ExpressionDepth);
	Row->SetStringField(TEXT("operator"), OperatorToString(Operator));
	Row->SetBoolField(TEXT("invert"), bInvert);

	const UScriptStruct* Struct = Condition.GetScriptStruct();
	const uint8* Memory = Condition.GetMemory();
	AddConditionStructMetadata(Struct, Row);

	bool bTruncated = false;
	int32 Unsupported = 0;
	int32 PropertyCount = 0;
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	if (Struct && Memory)
	{
		if (const FWorldConditionBase* Base = Condition.GetPtr<FWorldConditionBase>())
		{
			Row->SetStringField(TEXT("description"), Base->GetDescription().ToString());
		}

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const FString PropertyName = Property->GetName();
			if (PropertyName == TEXT("StateDataOffset") ||
				PropertyName == TEXT("ConditionIndex") ||
				PropertyName == TEXT("bInvert") ||
				PropertyName == TEXT("Operator") ||
				PropertyName == TEXT("NextExpressionDepth"))
			{
				continue;
			}

			if (PropertyCount >= MaxSafeProperties)
			{
				++Unsupported;
				bTruncated = true;
				continue;
			}

			bool bPropertyTruncated = false;
			if (WriteSafePropertyValue(Property, Memory, Properties, bPropertyTruncated))
			{
				++PropertyCount;
				bTruncated = bTruncated || bPropertyTruncated;
			}
			else
			{
				++Unsupported;
			}
		}
	}

	Row->SetObjectField(TEXT("properties"), Properties);
	Row->SetNumberField(TEXT("property_count"), PropertyCount);
	Row->SetNumberField(TEXT("unsupported_property_count"), Unsupported);
	Row->SetBoolField(TEXT("truncated"), bTruncated);
	return Row;
}

TSharedPtr<FJsonObject> QueryToJson(const FWorldConditionQueryDefinition& QueryDefinition)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), QueryDefinition.IsValid());
	Result->SetStringField(TEXT("description"), QueryDefinition.GetDescription().ToString());

	if (UClass* SchemaClass = QueryDefinition.GetSchemaClass())
	{
		Result->SetStringField(TEXT("schema_class"), SchemaClass->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("schema_class"), FString());
	}

	TArray<TSharedPtr<FJsonValue>> Conditions;
	int32 UnsupportedTotal = 0;
	bool bTruncated = false;

#if WITH_EDITORONLY_DATA
	const UScriptStruct* QueryStruct = FWorldConditionQueryDefinition::StaticStruct();
	const FArrayProperty* EditableArray = FindFProperty<FArrayProperty>(QueryStruct, TEXT("EditableConditions"));
	if (EditableArray)
	{
		const FStructProperty* EditableStructProp = CastField<FStructProperty>(EditableArray->Inner);
		const void* ArrayPtr = EditableArray->ContainerPtrToValuePtr<void>(&QueryDefinition);
		FScriptArrayHelper Helper(EditableArray, ArrayPtr);

		if (EditableStructProp && EditableStructProp->Struct)
		{
			const FByteProperty* ExpressionDepthProp = FindFProperty<FByteProperty>(EditableStructProp->Struct, TEXT("ExpressionDepth"));
			const FProperty* OperatorProp = FindFProperty<FProperty>(EditableStructProp->Struct, TEXT("Operator"));
			const FBoolProperty* InvertProp = FindFProperty<FBoolProperty>(EditableStructProp->Struct, TEXT("bInvert"));
			const FStructProperty* ConditionProp = FindFProperty<FStructProperty>(EditableStructProp->Struct, TEXT("Condition"));

			const int32 RowCount = FMath::Min(Helper.Num(), MaxConditionRows);
			Conditions.Reserve(RowCount);
			for (int32 Index = 0; Index < RowCount; ++Index)
			{
				const uint8* ElementPtr = Helper.GetRawPtr(Index);
				uint8 ExpressionDepth = ExpressionDepthProp ? ExpressionDepthProp->GetPropertyValue_InContainer(ElementPtr) : 0;
				bool bInvert = InvertProp ? InvertProp->GetPropertyValue_InContainer(ElementPtr) : false;
				EWorldConditionOperator Operator = EWorldConditionOperator::And;

				if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(OperatorProp))
				{
					const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(ElementPtr);
					Operator = static_cast<EWorldConditionOperator>(EnumProp->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(ValuePtr));
				}
				else if (const FByteProperty* ByteProp = CastField<FByteProperty>(OperatorProp))
				{
					Operator = static_cast<EWorldConditionOperator>(ByteProp->GetPropertyValue_InContainer(ElementPtr));
				}

				if (ConditionProp)
				{
					const FInstancedStruct* Condition = ConditionProp->ContainerPtrToValuePtr<FInstancedStruct>(ElementPtr);
					if (Condition && Condition->IsValid())
					{
						TSharedPtr<FJsonObject> Row = ConditionToJson(*Condition, Index, ExpressionDepth, Operator, bInvert);
						UnsupportedTotal += Row->GetIntegerField(TEXT("unsupported_property_count"));
						bTruncated = bTruncated || Row->GetBoolField(TEXT("truncated"));
						Conditions.Add(MakeShared<FJsonValueObject>(Row));
					}
					else
					{
						TSharedPtr<FJsonObject> EmptyRow = MakeShared<FJsonObject>();
						EmptyRow->SetNumberField(TEXT("index"), Index);
						EmptyRow->SetBoolField(TEXT("empty"), true);
						Conditions.Add(MakeShared<FJsonValueObject>(EmptyRow));
					}
				}
			}

			if (Helper.Num() > RowCount)
			{
				bTruncated = true;
				UnsupportedTotal += Helper.Num() - RowCount;
			}
		}
	}
#else
	if (const FWorldConditionQuerySharedDefinition* SharedDefinition = QueryDefinition.GetSharedDefinition())
	{
		const FInstancedStructContainer& SharedConditions = SharedDefinition->GetConditions();
		const int32 RowCount = FMath::Min(SharedConditions.Num(), MaxConditionRows);
		Conditions.Reserve(RowCount);
		for (int32 Index = 0; Index < RowCount; ++Index)
		{
			const FConstStructView View = SharedConditions[Index];
			FInstancedStruct Temp;
			Temp.InitializeAs(View.GetScriptStruct(), View.GetMemory());
			const FWorldConditionBase* Base = Temp.GetPtr<FWorldConditionBase>();
			TSharedPtr<FJsonObject> Row = ConditionToJson(
				Temp,
				Index,
				Base ? Base->GetNextExpressionDepth() : 0,
				Base ? Base->GetOperator() : EWorldConditionOperator::And,
				Base ? Base->ShouldInvertResult() : false);
			UnsupportedTotal += Row->GetIntegerField(TEXT("unsupported_property_count"));
			bTruncated = bTruncated || Row->GetBoolField(TEXT("truncated"));
			Conditions.Add(MakeShared<FJsonValueObject>(Row));
		}
		if (SharedConditions.Num() > RowCount)
		{
			bTruncated = true;
			UnsupportedTotal += SharedConditions.Num() - RowCount;
		}
	}
#endif

	Result->SetArrayField(TEXT("conditions"), Conditions);
	Result->SetNumberField(TEXT("condition_count"), Conditions.Num());
	Result->SetNumberField(TEXT("unsupported_property_count"), UnsupportedTotal);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	return Result;
}

TSharedPtr<FJsonObject> ConditionTypeToJson(const UScriptStruct* Struct)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	AddConditionStructMetadata(Struct, Obj);

	TArray<TSharedPtr<FJsonValue>> Props;
	if (Struct)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), Property->GetName());
			PropObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			PropObj->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
			Props.Add(MakeShared<FJsonValueObject>(PropObj));
		}
	}
	Obj->SetArrayField(TEXT("properties"), Props);
	Obj->SetNumberField(TEXT("property_count"), Props.Num());
	return Obj;
}
#endif
}

void FMonolithWorldConditionsActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("world_conditions"), TEXT("get_status"),
		TEXT("Report WorldConditions inspection feature state and optional dependency availability."),
		FMonolithActionHandler::CreateStatic(&HandleGetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("world_conditions"), TEXT("list_query_owners"),
		TEXT("List SmartObjectDefinition assets that can own WorldCondition query definitions."),
		FMonolithActionHandler::CreateStatic(&HandleListQueryOwners),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Package path to search recursively."), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum owners to return."), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("world_conditions"), TEXT("describe_query"),
		TEXT("Describe a SmartObjectDefinition WorldCondition query without mutating the asset."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeQuery),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("SmartObjectDefinition asset path."))
			.Optional(TEXT("query"), TEXT("string"), TEXT("preconditions or slot_selection_preconditions."), TEXT("preconditions"))
			.Optional(TEXT("slot_index"), TEXT("integer"), TEXT("Required when query=slot_selection_preconditions."))
			.Build());

	Registry.RegisterAction(TEXT("world_conditions"), TEXT("describe_condition_types"),
		TEXT("List loaded FWorldConditionBase-derived struct types and reflected property metadata."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeConditionTypes),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum condition types to return."), TEXT("128"))
			.Build());
}

FMonolithActionResult FMonolithWorldConditionsActions::HandleGetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeBaseStatus();

#if WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS
	const int32 Count = CollectSmartObjectDefinitionAssets(TEXT("/Game"), MaxOwnerLimit).Num();
	Result->SetNumberField(TEXT("candidate_owner_count"), Count);
	Result->SetStringField(TEXT("owner_shape"), TEXT("SmartObjectDefinition"));
	Result->SetStringField(TEXT("dependency_state"), TEXT("available"));
#else
	Result->SetNumberField(TEXT("candidate_owner_count"), 0);
	AddUnavailableFields(Result);
#endif

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorldConditionsActions::HandleListQueryOwners(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeBaseStatus();
	Result->SetStringField(TEXT("path_filter"), NormalizePackagePath(ReadStringOrDefault(Params, TEXT("path_filter"), TEXT("/Game"))));

	if (!IsFeatureEnabled())
	{
		Result->SetStringField(TEXT("dependency_state"), TEXT("disabled"));
		Result->SetArrayField(TEXT("owners"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetNumberField(TEXT("count"), 0);
		return FMonolithActionResult::Success(Result);
	}

#if WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS
	const int32 Limit = ReadLimit(Params, TEXT("limit"), DefaultOwnerLimit, MaxOwnerLimit);
	TArray<FAssetData> Assets = CollectSmartObjectDefinitionAssets(Result->GetStringField(TEXT("path_filter")), Limit);
	TArray<TSharedPtr<FJsonValue>> Owners;
	Owners.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		Owners.Add(MakeShared<FJsonValueObject>(AssetDataToOwnerJson(Asset)));
	}

	Result->SetStringField(TEXT("dependency_state"), TEXT("available"));
	Result->SetArrayField(TEXT("owners"), Owners);
	Result->SetNumberField(TEXT("count"), Owners.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), Owners.Num() >= Limit);
#else
	AddUnavailableFields(Result);
	Result->SetArrayField(TEXT("owners"), TArray<TSharedPtr<FJsonValue>>());
	Result->SetNumberField(TEXT("count"), 0);
#endif

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorldConditionsActions::HandleDescribeQuery(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeBaseStatus();

	if (!IsFeatureEnabled())
	{
		Result->SetStringField(TEXT("dependency_state"), TEXT("disabled"));
		return FMonolithActionResult::Success(Result);
	}

#if WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS
	const FString AssetPath = ReadStringOrDefault(Params, TEXT("asset_path"), FString());
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"), FMonolithJsonUtils::ErrInvalidParams);
	}

	USmartObjectDefinition* Definition = FMonolithAssetUtils::LoadAssetByPath<USmartObjectDefinition>(AssetPath);
	if (!Definition)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("SmartObjectDefinition not found: %s"), *AssetPath), FMonolithJsonUtils::ErrInvalidParams)
			.WithHint(TEXT("Use world_conditions.list_query_owners to discover valid SmartObjectDefinition assets."));
	}

	const FString QueryName = ReadStringOrDefault(Params, TEXT("query"), TEXT("preconditions"));
	Result->SetStringField(TEXT("asset_path"), Definition->GetPathName());
	Result->SetStringField(TEXT("owner_shape"), TEXT("SmartObjectDefinition"));
	Result->SetStringField(TEXT("dependency_state"), TEXT("available"));
	Result->SetStringField(TEXT("query"), QueryName);

	if (QueryName == TEXT("preconditions"))
	{
		Result->SetObjectField(TEXT("query_definition"), QueryToJson(Definition->GetPreconditions()));
		return FMonolithActionResult::Success(Result);
	}

	if (QueryName == TEXT("slot_selection_preconditions"))
	{
		if (!Params.IsValid() || !Params->HasTypedField<EJson::Number>(TEXT("slot_index")))
		{
			return FMonolithActionResult::Error(TEXT("slot_index is required when query=slot_selection_preconditions"), FMonolithJsonUtils::ErrInvalidParams);
		}

		const int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
		if (!Definition->IsValidSlotIndex(SlotIndex))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid slot_index %d for %s"), SlotIndex, *Definition->GetPathName()), FMonolithJsonUtils::ErrInvalidParams);
		}

		Result->SetNumberField(TEXT("slot_index"), SlotIndex);
		Result->SetObjectField(TEXT("query_definition"), QueryToJson(Definition->GetSlot(SlotIndex).SelectionPreconditions));
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(TEXT("query must be preconditions or slot_selection_preconditions"), FMonolithJsonUtils::ErrInvalidParams);
#else
	AddUnavailableFields(Result);
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithWorldConditionsActions::HandleDescribeConditionTypes(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeBaseStatus();

	if (!IsFeatureEnabled())
	{
		Result->SetStringField(TEXT("dependency_state"), TEXT("disabled"));
		Result->SetArrayField(TEXT("types"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetNumberField(TEXT("count"), 0);
		return FMonolithActionResult::Success(Result);
	}

#if WITH_MONOLITH_WORLDCONDITIONS
	const int32 Limit = ReadLimit(Params, TEXT("limit"), DefaultConditionTypeLimit, MaxConditionTypeLimit);
	TArray<const UScriptStruct*> Structs;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		const UScriptStruct* Struct = *It;
		if (Struct && Struct != FWorldConditionBase::StaticStruct() && Struct->IsChildOf(FWorldConditionBase::StaticStruct()))
		{
			Structs.Add(Struct);
		}
	}

	Structs.Sort([](const UScriptStruct& A, const UScriptStruct& B)
	{
		return A.GetPathName().Compare(B.GetPathName()) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> TypeRows;
	const int32 Count = FMath::Min(Structs.Num(), Limit);
	TypeRows.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		TypeRows.Add(MakeShared<FJsonValueObject>(ConditionTypeToJson(Structs[Index])));
	}

	Result->SetStringField(TEXT("dependency_state"), TEXT("available"));
	Result->SetArrayField(TEXT("types"), TypeRows);
	Result->SetNumberField(TEXT("count"), TypeRows.Num());
	Result->SetNumberField(TEXT("loaded_type_count"), Structs.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), Structs.Num() > TypeRows.Num());
#else
	AddUnavailableFields(Result);
	Result->SetArrayField(TEXT("types"), TArray<TSharedPtr<FJsonValue>>());
	Result->SetNumberField(TEXT("count"), 0);
#endif

	return FMonolithActionResult::Success(Result);
}
