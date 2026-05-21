#pragma once

#include "CoreMinimal.h"

class FMonolithIndexDatabase;
class FProperty;
class UMonolithSettings;

class FMonolithSearchValueWriter
{
public:
	explicit FMonolithSearchValueWriter(FMonolithIndexDatabase& InDatabase);

	bool IsEnabled() const;

	bool AddValue(
		int64 AssetId,
		const FString& SourceKind,
		const FString& ObjectName,
		const FString& ObjectPath,
		const FString& ObjectClass,
		const FString& FieldName,
		const FString& FieldPath,
		const FString& ValueText,
		const FString& Signal);

	static bool ExportPropertyValueForSearch(const FProperty* Property, const void* ValuePtr, FString& OutValue);

private:
	FMonolithIndexDatabase& Database;
	const UMonolithSettings* Settings = nullptr;
	TMap<int64, int32> AssetValueCounts;
	TMap<FString, int32> ObjectValueCounts;
};
