#include "MonolithGASInputAssetActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithGASInputAssetActions
{
	constexpr int32 DefaultAssetPageLimit = 200;
	constexpr int32 MaxAssetPageLimit = 1000;
	constexpr int32 DefaultMappingPageLimit = 100;
	constexpr int32 MaxMappingPageLimit = 500;
	constexpr int32 MaxInstancedObjectsPerArray = 256;
	constexpr int32 MaxInstancedObjectsPerMapping = 64;
	constexpr int32 DefaultMappingScanLimit = 4096;
	constexpr int32 MaxMappingScanLimit = 10000;
	constexpr int32 MaxExplicitContextPaths = 1000;

	FMonolithActionResult InvalidParam(const TCHAR* Field, const FString& Detail)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), Field, *Detail),
			-32602);
	}

	bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
	{
		return Params.IsValid() && Params->HasField(Field);
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
		if (HasParam(Params, Field))
		{
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				OutError = InvalidParam(Field, TEXT("expected an integer JSON number"));
				return false;
			}
		}

		if (!FMath::IsFinite(Number)
			|| Number != FMath::TruncToDouble(Number)
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

	bool ParseOptionalBool(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		bool DefaultValue,
		bool& OutValue,
		FMonolithActionResult& OutError)
	{
		OutValue = DefaultValue;
		if (!HasParam(Params, Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a boolean"));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		FString& OutValue,
		FMonolithActionResult& OutError)
	{
		if (!HasParam(Params, Field))
		{
			OutError = InvalidParam(Field, TEXT("field is required"));
			return false;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid()
			|| Value->Type != EJson::String
			|| !Value->TryGetString(OutValue)
			|| OutValue.IsEmpty())
		{
			OutError = InvalidParam(Field, TEXT("expected a non-empty string"));
			return false;
		}
		return true;
	}

	bool ReadOptionalString(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		const FString& DefaultValue,
		FString& OutValue,
		FMonolithActionResult& OutError)
	{
		OutValue = DefaultValue;
		if (!HasParam(Params, Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a string"));
			return false;
		}
		return true;
	}

	bool ParsePackageFilter(
		const FString& Input,
		FString& OutFilter,
		FMonolithActionResult& OutError)
	{
		OutFilter = Input;
		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != Input)
		{
			OutError = InvalidParam(TEXT("path"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT(":"))
			|| Input.Contains(TEXT("."))
			|| Input.EndsWith(TEXT("/")))
		{
			OutError = InvalidParam(
				TEXT("path"),
				TEXT("expected a canonical Unreal package prefix such as /Game/Input"));
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(Input))
		{
			OutError = InvalidParam(
				TEXT("path"),
				TEXT("expected a valid mounted long package prefix"));
			return false;
		}
		return true;
	}

	bool ParseAssetPath(
		const FString& Input,
		const TCHAR* Field,
		FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed != Input)
		{
			OutError = InvalidParam(Field, TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (Input.Contains(TEXT("\\"))
			|| Input.Contains(TEXT(":"))
			|| Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = InvalidParam(
				Field,
				TEXT("expected a canonical Unreal package or top-level object path"));
			return false;
		}

		FString PackagePath;
		FString ObjectName;
		const bool bObjectPath = Input.Split(TEXT("."), &PackagePath, &ObjectName);
		if (!bObjectPath)
		{
			PackagePath = Input;
		}
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			OutError = InvalidParam(Field, TEXT("expected a valid mounted long package name"));
			return false;
		}

		const FString PackageLeaf = FPackageName::GetLongPackageAssetName(PackagePath);
		if (PackageLeaf.IsEmpty())
		{
			OutError = InvalidParam(Field, TEXT("asset name is missing"));
			return false;
		}
		if (bObjectPath && ObjectName != PackageLeaf)
		{
			OutError = InvalidParam(
				Field,
				FString::Printf(
					TEXT("object name '%s' must match package leaf '%s'"),
					*ObjectName,
					*PackageLeaf));
			return false;
		}

		OutObjectPath = PackagePath + TEXT(".") + PackageLeaf;
		if (!FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = InvalidParam(Field, TEXT("expected a valid top-level object path"));
			return false;
		}
		return true;
	}

	template <typename TAsset>
	TAsset* LoadExactAsset(
		const FString& Input,
		const TCHAR* Field,
		const TCHAR* ExpectedType,
		FMonolithActionResult& OutError)
	{
		FString ObjectPath;
		if (!ParseAssetPath(Input, Field, ObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Object = FSoftObjectPath(ObjectPath).TryLoad();
		if (!Object)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("Asset not found: %s"), *ObjectPath));
			return nullptr;
		}
		TAsset* Asset = Cast<TAsset>(Object);
		if (!Asset)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Asset '%s' is %s, expected %s"),
					*ObjectPath,
					*Object->GetClass()->GetName(),
					ExpectedType));
			return nullptr;
		}
		return Asset;
	}

	FString ValueTypeToString(EInputActionValueType ValueType)
	{
		switch (ValueType)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D: return TEXT("Axis1D");
		case EInputActionValueType::Axis2D: return TEXT("Axis2D");
		case EInputActionValueType::Axis3D: return TEXT("Axis3D");
		default: return TEXT("Unknown");
		}
	}

	FString AccumulationToString(EInputActionAccumulationBehavior Behavior)
	{
		return Behavior == EInputActionAccumulationBehavior::Cumulative
			? TEXT("Cumulative")
			: TEXT("TakeHighestAbsoluteValue");
	}

	FString TrackingModeToString(EMappingContextRegistrationTrackingMode Mode)
	{
		return Mode == EMappingContextRegistrationTrackingMode::CountRegistrations
			? TEXT("CountRegistrations")
			: TEXT("Untracked");
	}

	TSharedPtr<FJsonObject> InstancedObjectToJson(const UObject* Object)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("class"), Object ? Object->GetClass()->GetName() : TEXT("None"));
		Json->SetStringField(TEXT("class_path"), Object ? Object->GetClass()->GetPathName() : TEXT(""));
		return Json;
	}

	template <typename TObject>
	TArray<TSharedPtr<FJsonValue>> InstancedObjectArrayToJson(
		const TArray<TObjectPtr<TObject>>& Objects,
		int32 Limit,
		bool& bOutTruncated)
	{
		const int32 Count = FMath::Min(Objects.Num(), Limit);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values.Add(MakeShared<FJsonValueObject>(InstancedObjectToJson(Objects[Index].Get())));
		}
		bOutTruncated = Objects.Num() > Count;
		return Values;
	}

	TSharedPtr<FJsonObject> InputActionToJson(const UInputAction* Action)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Action)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Action->GetPathName());
		Json->SetStringField(TEXT("package_path"), Action->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Action->GetName());
		Json->SetStringField(TEXT("value_type"), ValueTypeToString(Action->ValueType));
		Json->SetStringField(TEXT("description"), Action->ActionDescription.ToString());
		Json->SetBoolField(TEXT("consume_input"), Action->bConsumeInput);
		Json->SetBoolField(TEXT("consume_legacy_mappings"), Action->bConsumesActionAndAxisMappings);
		Json->SetBoolField(TEXT("trigger_when_paused"), Action->bTriggerWhenPaused);
		Json->SetBoolField(TEXT("reserve_all_mappings"), Action->bReserveAllMappings);
		Json->SetStringField(TEXT("accumulation"), AccumulationToString(Action->AccumulationBehavior));

		bool bTriggersTruncated = false;
		bool bModifiersTruncated = false;
		Json->SetArrayField(
			TEXT("triggers"),
			InstancedObjectArrayToJson(
				Action->Triggers,
				MaxInstancedObjectsPerArray,
				bTriggersTruncated));
		Json->SetArrayField(
			TEXT("modifiers"),
			InstancedObjectArrayToJson(
				Action->Modifiers,
				MaxInstancedObjectsPerArray,
				bModifiersTruncated));
		Json->SetNumberField(TEXT("trigger_count"), Action->Triggers.Num());
		Json->SetNumberField(TEXT("modifier_count"), Action->Modifiers.Num());
		Json->SetBoolField(TEXT("triggers_truncated"), bTriggersTruncated);
		Json->SetBoolField(TEXT("modifiers_truncated"), bModifiersTruncated);
		Json->SetBoolField(
			TEXT("has_player_mappable_settings"),
			Action->GetPlayerMappableKeySettings() != nullptr);
		return Json;
	}

	TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("index"), Index);
		Json->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
		Json->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
		Json->SetStringField(TEXT("key"), Mapping.Key.ToString());
		Json->SetBoolField(TEXT("key_valid"), Mapping.Key.IsValid());
		Json->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());
		Json->SetStringField(TEXT("mapping_name"), Mapping.GetMappingName().ToString());
		Json->SetStringField(TEXT("display_name"), Mapping.GetDisplayName().ToString());
		Json->SetStringField(TEXT("display_category"), Mapping.GetDisplayCategory().ToString());

		bool bTriggersTruncated = false;
		bool bModifiersTruncated = false;
		Json->SetArrayField(
			TEXT("triggers"),
			InstancedObjectArrayToJson(
				Mapping.Triggers,
				MaxInstancedObjectsPerMapping,
				bTriggersTruncated));
		Json->SetArrayField(
			TEXT("modifiers"),
			InstancedObjectArrayToJson(
				Mapping.Modifiers,
				MaxInstancedObjectsPerMapping,
				bModifiersTruncated));
		Json->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
		Json->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
		Json->SetBoolField(TEXT("triggers_truncated"), bTriggersTruncated);
		Json->SetBoolField(TEXT("modifiers_truncated"), bModifiersTruncated);
		return Json;
	}

	void SetPageMetadata(
		const TSharedPtr<FJsonObject>& Json,
		int32 Total,
		int32 Offset,
		int32 Limit,
		int32 Count)
	{
		Json->SetNumberField(TEXT("total"), Total);
		Json->SetNumberField(TEXT("offset"), Offset);
		Json->SetNumberField(TEXT("limit"), Limit);
		Json->SetNumberField(TEXT("count"), Count);
		Json->SetBoolField(TEXT("has_more"), static_cast<int64>(Offset) + Count < Total);
	}

	TSharedPtr<FJsonObject> MappingContextToJson(
		const UInputMappingContext* Context,
		int32 MappingOffset,
		int32 MappingLimit)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Context)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Context->GetPathName());
		Json->SetStringField(TEXT("package_path"), Context->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Context->GetName());
		Json->SetStringField(TEXT("description"), Context->ContextDescription.ToString());
		Json->SetBoolField(TEXT("filters_by_input_mode"), Context->ShouldFilterMappingByInputMode());
		Json->SetStringField(
			TEXT("registration_tracking_mode"),
			TrackingModeToString(Context->GetRegistrationTrackingMode()));

		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		const int32 Start = FMath::Min(MappingOffset, Mappings.Num());
		const int32 End = static_cast<int32>(FMath::Min<int64>(
			Mappings.Num(),
			static_cast<int64>(MappingOffset) + MappingLimit));
		TArray<TSharedPtr<FJsonValue>> MappingValues;
		MappingValues.Reserve(End - Start);
		for (int32 Index = Start; Index < End; ++Index)
		{
			MappingValues.Add(MakeShared<FJsonValueObject>(MappingToJson(Mappings[Index], Index)));
		}
		Json->SetArrayField(TEXT("mappings"), MappingValues);
		Json->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		Json->SetNumberField(TEXT("mapping_offset"), MappingOffset);
		Json->SetNumberField(TEXT("mapping_limit"), MappingLimit);
		Json->SetNumberField(TEXT("mappings_returned"), MappingValues.Num());
		Json->SetBoolField(
			TEXT("mappings_truncated"),
			Start > 0 || End < Mappings.Num());
		Json->SetBoolField(TEXT("has_more_mappings"), End < Mappings.Num());
		return Json;
	}

	void QueryAssets(
		UClass* AssetClass,
		const FString& PackageFilter,
		TArray<FAssetData>& OutAssets)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(AssetClass->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*PackageFilter));
		Filter.bRecursivePaths = true;

		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Module.Get().GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});
	}

	bool ParseAssetListParams(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutPath,
		int32& OutOffset,
		int32& OutLimit,
		bool& bOutIncludeDetails,
		FMonolithActionResult& OutError)
	{
		FString Path;
		if (!ReadOptionalString(Params, TEXT("path"), TEXT("/Game"), Path, OutError)
			|| !ParsePackageFilter(Path, OutPath, OutError)
			|| !ParseBoundedInteger(Params, TEXT("offset"), 0, 0, MAX_int32, OutOffset, OutError)
			|| !ParseBoundedInteger(
				Params,
				TEXT("limit"),
				DefaultAssetPageLimit,
				1,
				MaxAssetPageLimit,
				OutLimit,
				OutError)
			|| !ParseOptionalBool(Params, TEXT("include_details"), false, bOutIncludeDetails, OutError))
		{
			return false;
		}
		return true;
	}

	bool ParseContextPaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutPaths,
		FMonolithActionResult& OutError)
	{
		OutPaths.Reset();
		if (!HasParam(Params, TEXT("context_paths")))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("context_paths"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Field.IsValid()
			|| Field->Type != EJson::Array
			|| !Field->TryGetArray(Values)
			|| !Values
			|| Values->IsEmpty())
		{
			OutError = InvalidParam(
				TEXT("context_paths"),
				TEXT("expected a non-empty array of canonical asset paths"));
			return false;
		}
		if (Values->Num() > MaxExplicitContextPaths)
		{
			OutError = InvalidParam(
				TEXT("context_paths"),
				FString::Printf(TEXT("at most %d paths are accepted"), MaxExplicitContextPaths));
			return false;
		}

		TSet<FString> Seen;
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			FString Path;
			if (!(*Values)[Index].IsValid()
				|| (*Values)[Index]->Type != EJson::String
				|| !(*Values)[Index]->TryGetString(Path)
				|| Path.IsEmpty())
			{
				OutError = InvalidParam(
					TEXT("context_paths"),
					FString::Printf(TEXT("element %d must be a non-empty string"), Index));
				return false;
			}

			FString ObjectPath;
			if (!ParseAssetPath(Path, TEXT("context_paths"), ObjectPath, OutError))
			{
				return false;
			}
			if (Seen.Contains(ObjectPath))
			{
				OutError = InvalidParam(
					TEXT("context_paths"),
					FString::Printf(TEXT("duplicate path '%s'"), *ObjectPath));
				return false;
			}
			Seen.Add(ObjectPath);
			OutPaths.Add(ObjectPath);
		}
		OutPaths.Sort();
		return true;
	}

	TSharedPtr<FJsonObject> MakeIssue(
		const TCHAR* Type,
		const TCHAR* Severity,
		const FString& Message,
		int32 MappingIndex = INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), Type);
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		if (MappingIndex != INDEX_NONE)
		{
			Issue->SetNumberField(TEXT("mapping_index"), MappingIndex);
		}
		return Issue;
	}
}

void FMonolithGASInputAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	FMonolithDispatcherAnnotations Annotations;
	Annotations.bReadOnlyHint = true;
	Annotations.bIdempotentHint = true;
	Annotations.Title = TEXT("Enhanced Input Asset Inspection");
	Registry.SetDispatcherAnnotations(TEXT("input"), Annotations);

	Registry.RegisterAction(
		TEXT("input"),
		TEXT("list_input_actions"),
		TEXT("List Enhanced Input action assets with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&HandleListInputActions),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load only the returned page and include action details"), TEXT("false"))
			.Build());

	Registry.RegisterAction(
		TEXT("input"),
		TEXT("get_input_action"),
		TEXT("Inspect one Enhanced Input action asset without modifying it"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputAction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputAction package or object path"))
			.Build());

	Registry.RegisterAction(
		TEXT("input"),
		TEXT("list_input_mapping_contexts"),
		TEXT("List Enhanced Input mapping contexts with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&HandleListInputMappingContexts),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load only the returned page and include bounded mappings"), TEXT("false"))
			.Optional(TEXT("mapping_limit"), TEXT("integer"), TEXT("Mappings per detailed context (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(
		TEXT("input"),
		TEXT("get_input_mapping_context"),
		TEXT("Inspect one Enhanced Input mapping context with bounded mapping pagination"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputMappingContext),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputMappingContext package or object path"))
			.Optional(TEXT("mapping_offset"), TEXT("integer"), TEXT("Zero-based mapping offset"), TEXT("0"))
			.Optional(TEXT("mapping_limit"), TEXT("integer"), TEXT("Maximum mappings to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(
		TEXT("input"),
		TEXT("validate_input_mappings"),
		TEXT("Read-only validation for missing actions, invalid keys, duplicate-key warnings, and scan completeness"),
		FMonolithActionHandler::CreateStatic(&HandleValidateInputMappings),
		FParamSchemaBuilder()
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Canonical InputMappingContext paths; mutually exclusive with path"))
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root when context_paths is omitted; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based context offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum contexts to validate (1-1000)"), TEXT("200"))
			.Optional(TEXT("mapping_scan_limit"), TEXT("integer"), TEXT("Maximum mappings scanned per context (1-10000)"), TEXT("4096"))
			.Build());
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputActions(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PackageFilter;
	int32 Offset = 0;
	int32 Limit = 0;
	bool bIncludeDetails = false;
	FMonolithActionResult Error;
	if (!MonolithGASInputAssetActions::ParseAssetListParams(
		Params,
		PackageFilter,
		Offset,
		Limit,
		bIncludeDetails,
		Error))
	{
		return Error;
	}

	TArray<FAssetData> Assets;
	MonolithGASInputAssetActions::QueryAssets(UInputAction::StaticClass(), PackageFilter, Assets);
	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		Assets.Num(),
		static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(End - Start);
	for (int32 Index = Start; Index < End; ++Index)
	{
		const FAssetData& AssetData = Assets[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputAction* Action = Cast<UInputAction>(AssetData.GetAsset()))
			{
				Row = MonolithGASInputAssetActions::InputActionToJson(Action);
			}
			else
			{
				Row->SetStringField(TEXT("load_error"), TEXT("Asset could not be loaded as UInputAction"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackageFilter);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetArrayField(TEXT("actions"), Rows);
	MonolithGASInputAssetActions::SetPageMetadata(
		Result,
		Assets.Num(),
		Offset,
		Limit,
		Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputAction(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Error;
	if (!MonolithGASInputAssetActions::ReadRequiredString(
			Params,
			TEXT("asset_path"),
			AssetPath,
			Error))
	{
		return Error;
	}
	UInputAction* Action = MonolithGASInputAssetActions::LoadExactAsset<UInputAction>(
		AssetPath,
		TEXT("asset_path"),
		TEXT("UInputAction"),
		Error);
	return Action
		? FMonolithActionResult::Success(MonolithGASInputAssetActions::InputActionToJson(Action))
		: Error;
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputMappingContexts(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PackageFilter;
	int32 Offset = 0;
	int32 Limit = 0;
	int32 MappingLimit = MonolithGASInputAssetActions::DefaultMappingPageLimit;
	bool bIncludeDetails = false;
	FMonolithActionResult Error;
	if (!MonolithGASInputAssetActions::ParseAssetListParams(
		Params,
		PackageFilter,
		Offset,
		Limit,
		bIncludeDetails,
		Error)
		|| !MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("mapping_limit"),
			MonolithGASInputAssetActions::DefaultMappingPageLimit,
			1,
			MonolithGASInputAssetActions::MaxMappingPageLimit,
			MappingLimit,
			Error))
	{
		return Error;
	}

	TArray<FAssetData> Assets;
	MonolithGASInputAssetActions::QueryAssets(
		UInputMappingContext::StaticClass(),
		PackageFilter,
		Assets);
	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		Assets.Num(),
		static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(End - Start);
	for (int32 Index = Start; Index < End; ++Index)
	{
		const FAssetData& AssetData = Assets[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(AssetData.GetAsset()))
			{
				Row = MonolithGASInputAssetActions::MappingContextToJson(Context, 0, MappingLimit);
			}
			else
			{
				Row->SetStringField(
					TEXT("load_error"),
					TEXT("Asset could not be loaded as UInputMappingContext"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackageFilter);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetNumberField(TEXT("mapping_limit"), MappingLimit);
	Result->SetArrayField(TEXT("contexts"), Rows);
	MonolithGASInputAssetActions::SetPageMetadata(
		Result,
		Assets.Num(),
		Offset,
		Limit,
		Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputMappingContext(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	int32 MappingOffset = 0;
	int32 MappingLimit = MonolithGASInputAssetActions::DefaultMappingPageLimit;
	FMonolithActionResult Error;
	if (!MonolithGASInputAssetActions::ReadRequiredString(
			Params,
			TEXT("asset_path"),
			AssetPath,
			Error)
		|| !MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("mapping_offset"),
			0,
			0,
			MAX_int32,
			MappingOffset,
			Error)
		|| !MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("mapping_limit"),
			MonolithGASInputAssetActions::DefaultMappingPageLimit,
			1,
			MonolithGASInputAssetActions::MaxMappingPageLimit,
			MappingLimit,
			Error))
	{
		return Error;
	}

	UInputMappingContext* Context =
		MonolithGASInputAssetActions::LoadExactAsset<UInputMappingContext>(
		AssetPath,
		TEXT("asset_path"),
		TEXT("UInputMappingContext"),
		Error);
	return Context
		? FMonolithActionResult::Success(
			MonolithGASInputAssetActions::MappingContextToJson(
				Context,
				MappingOffset,
				MappingLimit))
		: Error;
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleValidateInputMappings(
	const TSharedPtr<FJsonObject>& Params)
{
	const bool bExplicitContexts =
		MonolithGASInputAssetActions::HasParam(Params, TEXT("context_paths"));
	if (bExplicitContexts && MonolithGASInputAssetActions::HasParam(Params, TEXT("path")))
	{
		return MonolithGASInputAssetActions::InvalidParam(
			TEXT("path"),
			TEXT("path and context_paths are mutually exclusive"));
	}

	TArray<FString> ContextPaths;
	FMonolithActionResult Error;
	if (!MonolithGASInputAssetActions::ParseContextPaths(Params, ContextPaths, Error))
	{
		return Error;
	}
	if (!bExplicitContexts)
	{
		FString Path;
		FString PackageFilter;
		if (!MonolithGASInputAssetActions::ReadOptionalString(
				Params,
				TEXT("path"),
				TEXT("/Game"),
				Path,
				Error)
			|| !MonolithGASInputAssetActions::ParsePackageFilter(Path, PackageFilter, Error))
		{
			return Error;
		}
		TArray<FAssetData> Assets;
		MonolithGASInputAssetActions::QueryAssets(
			UInputMappingContext::StaticClass(),
			PackageFilter,
			Assets);
		ContextPaths.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			ContextPaths.Add(Asset.GetObjectPathString());
		}
	}

	int32 Offset = 0;
	int32 Limit = MonolithGASInputAssetActions::DefaultAssetPageLimit;
	int32 MappingScanLimit = MonolithGASInputAssetActions::DefaultMappingScanLimit;
	if (!MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("offset"),
			0,
			0,
			MAX_int32,
			Offset,
			Error)
		|| !MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("limit"),
			MonolithGASInputAssetActions::DefaultAssetPageLimit,
			1,
			MonolithGASInputAssetActions::MaxAssetPageLimit,
			Limit,
			Error)
		|| !MonolithGASInputAssetActions::ParseBoundedInteger(
			Params,
			TEXT("mapping_scan_limit"),
			MonolithGASInputAssetActions::DefaultMappingScanLimit,
			1,
			MonolithGASInputAssetActions::MaxMappingScanLimit,
			MappingScanLimit,
			Error))
	{
		return Error;
	}

	const int32 Start = FMath::Min(Offset, ContextPaths.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		ContextPaths.Num(),
		static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> ContextResults;
	ContextResults.Reserve(End - Start);
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	bool bPageComplete = true;

	for (int32 ContextIndex = Start; ContextIndex < End; ++ContextIndex)
	{
		const FString& ContextPath = ContextPaths[ContextIndex];
		TSharedPtr<FJsonObject> ContextResult = MakeShared<FJsonObject>();
		ContextResult->SetStringField(TEXT("context_path"), ContextPath);
		TArray<TSharedPtr<FJsonValue>> Issues;
		UInputMappingContext* Context =
			MonolithGASInputAssetActions::LoadExactAsset<UInputMappingContext>(
			ContextPath,
			TEXT("context_paths"),
			TEXT("UInputMappingContext"),
			Error);
		if (!Context)
		{
			++ErrorCount;
			bPageComplete = false;
			Issues.Add(MakeShared<FJsonValueObject>(MonolithGASInputAssetActions::MakeIssue(
				TEXT("context_load_failed"),
				TEXT("error"),
				Error.ErrorMessage)));
			ContextResult->SetBoolField(TEXT("valid"), false);
			ContextResult->SetBoolField(TEXT("complete"), false);
			ContextResult->SetNumberField(TEXT("errors"), 1);
			ContextResult->SetNumberField(TEXT("warnings"), 0);
			ContextResult->SetArrayField(TEXT("issues"), Issues);
			ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
			continue;
		}

		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		const int32 ScanCount = FMath::Min(Mappings.Num(), MappingScanLimit);
		int32 ContextErrors = 0;
		int32 ContextWarnings = 0;
		TMap<FString, TArray<FString>> KeyToActions;
		for (int32 MappingIndex = 0; MappingIndex < ScanCount; ++MappingIndex)
		{
			const FEnhancedActionKeyMapping& Mapping = Mappings[MappingIndex];
			if (!Mapping.Action)
			{
				++ContextErrors;
				Issues.Add(MakeShared<FJsonValueObject>(MonolithGASInputAssetActions::MakeIssue(
					TEXT("missing_action"),
					TEXT("error"),
					TEXT("Mapping has no InputAction"),
					MappingIndex)));
			}
			if (!Mapping.Key.IsValid())
			{
				++ContextErrors;
				Issues.Add(MakeShared<FJsonValueObject>(MonolithGASInputAssetActions::MakeIssue(
					TEXT("invalid_key"),
					TEXT("error"),
					TEXT("Mapping has an invalid FKey"),
					MappingIndex)));
			}
			if (Mapping.Action && Mapping.Key.IsValid())
			{
				TArray<FString>& Actions = KeyToActions.FindOrAdd(Mapping.Key.ToString());
				Actions.AddUnique(Mapping.Action->GetPathName());
			}
		}

		TArray<FString> Keys;
		KeyToActions.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			TArray<FString> Actions = KeyToActions.FindChecked(Key);
			if (Actions.Num() < 2)
			{
				continue;
			}
			Actions.Sort();
			++ContextWarnings;
			TSharedPtr<FJsonObject> Issue = MonolithGASInputAssetActions::MakeIssue(
				TEXT("duplicate_key_assignment"),
				TEXT("warning"),
				FString::Printf(TEXT("Key '%s' is assigned to multiple actions"), *Key));
			Issue->SetStringField(TEXT("key"), Key);
			TArray<TSharedPtr<FJsonValue>> ActionValues;
			for (const FString& Action : Actions)
			{
				ActionValues.Add(MakeShared<FJsonValueString>(Action));
			}
			Issue->SetArrayField(TEXT("actions"), ActionValues);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		const bool bContextComplete = ScanCount == Mappings.Num();
		if (!bContextComplete)
		{
			++ContextErrors;
			Issues.Add(MakeShared<FJsonValueObject>(MonolithGASInputAssetActions::MakeIssue(
				TEXT("mapping_scan_limit_exceeded"),
				TEXT("error"),
				FString::Printf(
					TEXT("Validation scanned %d of %d mappings"),
					ScanCount,
					Mappings.Num()))));
			bPageComplete = false;
		}

		ErrorCount += ContextErrors;
		WarningCount += ContextWarnings;
		ContextResult->SetStringField(TEXT("asset_path"), Context->GetPathName());
		ContextResult->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		ContextResult->SetNumberField(TEXT("mappings_scanned"), ScanCount);
		ContextResult->SetNumberField(TEXT("mapping_scan_limit"), MappingScanLimit);
		ContextResult->SetBoolField(TEXT("complete"), bContextComplete);
		ContextResult->SetBoolField(TEXT("valid"), bContextComplete && ContextErrors == 0);
		ContextResult->SetNumberField(TEXT("errors"), ContextErrors);
		ContextResult->SetNumberField(TEXT("warnings"), ContextWarnings);
		ContextResult->SetArrayField(TEXT("issues"), Issues);
		ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
	}

	const bool bAllContextsCovered = Start == 0 && End == ContextPaths.Num();
	const bool bComplete = bPageComplete && bAllContextsCovered;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), bComplete && ErrorCount == 0);
	Result->SetBoolField(TEXT("complete"), bComplete);
	Result->SetBoolField(TEXT("page_complete"), bPageComplete);
	Result->SetBoolField(TEXT("all_contexts_covered"), bAllContextsCovered);
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetNumberField(TEXT("warnings"), WarningCount);
	Result->SetNumberField(TEXT("mapping_scan_limit"), MappingScanLimit);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	MonolithGASInputAssetActions::SetPageMetadata(
		Result,
		ContextPaths.Num(),
		Offset,
		Limit,
		ContextResults.Num());
	return FMonolithActionResult::Success(Result);
}
