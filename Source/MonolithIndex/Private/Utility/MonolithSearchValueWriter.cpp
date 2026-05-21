#include "Utility/MonolithSearchValueWriter.h"

#include "MonolithIndexDatabase.h"
#include "MonolithSettings.h"
#include "UObject/UnrealType.h"

FMonolithSearchValueWriter::FMonolithSearchValueWriter(FMonolithIndexDatabase& InDatabase)
	: Database(InDatabase)
	, Settings(GetDefault<UMonolithSettings>())
{
}

bool FMonolithSearchValueWriter::IsEnabled() const
{
	return Settings
		&& Settings->bIndexSearchableValues
		&& Settings->MaxSearchableValuesPerAsset > 0
		&& Settings->MaxSearchableValuesPerObject > 0
		&& Settings->MaxSearchableValueChars > 0;
}

bool FMonolithSearchValueWriter::AddValue(
	int64 AssetId,
	const FString& SourceKind,
	const FString& ObjectName,
	const FString& ObjectPath,
	const FString& ObjectClass,
	const FString& FieldName,
	const FString& FieldPath,
	const FString& ValueText,
	const FString& Signal)
{
	if (!IsEnabled() || AssetId <= 0)
	{
		return false;
	}

	FString NormalizedValue = ValueText.TrimStartAndEnd();
	if (NormalizedValue.IsEmpty())
	{
		return false;
	}

	const int32 MaxChars = FMath::Max(1, Settings->MaxSearchableValueChars);
	if (NormalizedValue.Len() > MaxChars)
	{
		NormalizedValue.LeftInline(MaxChars);
	}

	int32& AssetCount = AssetValueCounts.FindOrAdd(AssetId);
	if (AssetCount >= Settings->MaxSearchableValuesPerAsset)
	{
		return false;
	}

	const FString ObjectKey = FString::Printf(TEXT("%lld:%s"), AssetId, ObjectPath.IsEmpty() ? *ObjectName : *ObjectPath);
	int32& ObjectCount = ObjectValueCounts.FindOrAdd(ObjectKey);
	if (ObjectCount >= Settings->MaxSearchableValuesPerObject)
	{
		return false;
	}

	FIndexedSearchValue SearchValue;
	SearchValue.AssetId = AssetId;
	SearchValue.SourceKind = SourceKind;
	SearchValue.ObjectName = ObjectName;
	SearchValue.ObjectPath = ObjectPath;
	SearchValue.ObjectClass = ObjectClass;
	SearchValue.FieldName = FieldName;
	SearchValue.FieldPath = FieldPath;
	SearchValue.ValueText = MoveTemp(NormalizedValue);
	SearchValue.Signal = Signal;

	if (Database.InsertAssetSearchValue(SearchValue) <= 0)
	{
		return false;
	}

	++AssetCount;
	++ObjectCount;
	return true;
}

bool FMonolithSearchValueWriter::ExportPropertyValueForSearch(const FProperty* Property, const void* ValuePtr, FString& OutValue)
{
	OutValue.Reset();
	if (!Property || !ValuePtr)
	{
		return false;
	}

	const bool bSearchableProperty =
		Property->IsA<FStrProperty>()
		|| Property->IsA<FNameProperty>()
		|| Property->IsA<FTextProperty>()
		|| Property->IsA<FEnumProperty>()
		|| Property->IsA<FSoftObjectProperty>()
		|| Property->IsA<FSoftClassProperty>()
		|| Property->IsA<FObjectPropertyBase>()
		|| Property->IsA<FClassProperty>();

	if (!bSearchableProperty)
	{
		return false;
	}

	Property->ExportTextItem_Direct(OutValue, ValuePtr, nullptr, nullptr, PPF_None);
	OutValue = OutValue.TrimStartAndEnd();
	return !OutValue.IsEmpty();
}
