#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithReflectionReader.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/DataTable.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "MonolithBlueprintEditCradle.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_user_defined_struct"),
		TEXT("Create a new User Defined Struct asset with the specified fields. Each field has a name, type (same type strings as add_variable), and optional default_value."),
		FMonolithActionHandler::CreateStatic(&HandleCreateUserDefinedStruct),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset save path, e.g. /Game/Data/S_MyStruct"))
			.Required(TEXT("fields"),    TEXT("array"),  TEXT("Array of field objects: [{name, type, default_value?}]. Type uses same strings as add_variable (bool, int, float, string, name, text, Vector, Rotator, Transform, object:ClassName, etc.)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_user_defined_enum"),
		TEXT("Create a new User Defined Enum asset with the specified enumerator values."),
		FMonolithActionHandler::CreateStatic(&HandleCreateUserDefinedEnum),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset save path, e.g. /Game/Data/E_MyEnum"))
			.Required(TEXT("values"),    TEXT("array"),  TEXT("Array of enumerator display name strings, e.g. [\"Idle\", \"Running\", \"Jumping\"]"))
			.Build());

	// DataTable actions (Phase 3C)
	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_data_table"),
		TEXT("Create a new DataTable asset backed by the specified row struct (UScriptStruct). The struct must already exist (native or user-defined)."),
		FMonolithActionHandler::CreateStatic(&HandleCreateDataTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"),  TEXT("Asset save path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_struct"), TEXT("string"), TEXT("Name of the row struct, e.g. FMyRowStruct, MyRowStruct, or a full path like /Script/MyModule.MyRowStruct"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_data_table_row"),
		TEXT("Add a row to an existing DataTable. Values are a JSON object mapping column names to values (uses ImportText per field)."),
		FMonolithActionHandler::CreateStatic(&HandleAddDataTableRow),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_name"),   TEXT("string"), TEXT("Row name / key"))
			.Required(TEXT("values"),     TEXT("object"), TEXT("JSON object of {column_name: value, ...}. Values are converted via ImportText."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_data_table_rows"),
		TEXT("Read rows from a DataTable. Returns all rows, or a single row if row_name is specified."),
		FMonolithActionHandler::CreateStatic(&HandleGetDataTableRows),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Optional(TEXT("row_name"),   TEXT("string"), TEXT("If provided, return only this row. Otherwise return all rows."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_data_table_schema"),
		TEXT("Inspect a DataTable row struct and return column metadata without row payloads."),
		FMonolithActionHandler::CreateStatic(&HandleGetDataTableSchema),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("update_data_table_row"),
		TEXT("Update an existing DataTable row, optionally creating it when create_if_missing=true. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleUpdateDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_name"), TEXT("string"), TEXT("Row name / key"))
			.Required(TEXT("values"), TEXT("object"), TEXT("JSON object of {column_name: value, ...}. Values are converted via ImportText."))
			.Optional(TEXT("create_if_missing"), TEXT("boolean"), TEXT("Create row when it does not already exist"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save DataTable package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_data_table_row"),
		TEXT("Remove one DataTable row by key. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_name"), TEXT("string"), TEXT("Row name / key"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save DataTable package after mutation"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("export_data_table_csv"),
		TEXT("Export a DataTable to CSV under the project directory. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleExportDataTableCsv),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("file_path"), TEXT("string"), TEXT("Destination CSV path under the project directory"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without writing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_data_asset"),
		TEXT("Create a raw UObject asset (NOT a Blueprint). Use for DataAssets, MaterialParameterCollections, PhysicalMaterials, CurveFloats, and any UObject-derived class that needs to exist as a direct instance rather than a Blueprint-generated class. Resolves class_name via FindFirstObject with U/A prefix fallback. Rejects abstract, deprecated, Actor-derived, and Blueprint classes."),
		FMonolithActionHandler::CreateStatic(&HandleCreateDataAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"),  TEXT("Asset save path, e.g. /Game/Data/DA_ResponseMap"))
			.Required(TEXT("class_name"), TEXT("string"),  TEXT("UObject class name, e.g. CarnageFXResponseMap, MaterialParameterCollection, PhysicalMaterial, CurveFloat. Can also use full path /Script/Module.ClassName for disambiguation"))
			.Optional(TEXT("skip_save"),  TEXT("boolean"), TEXT("Skip synchronous package save (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("seed_data_asset"),
		TEXT("Create AND populate a UObject DataAsset in one call: create_data_asset's body plus a reflection-walker fill of the supplied property 'tree'. Supports dry_run (validate before creating) and strict. Returns asset_path, actual_class, and FDryRunReport-shaped field_writes."),
		FMonolithActionHandler::CreateStatic(&HandleSeedDataAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"),  TEXT("Asset save path, e.g. /Game/Data/DA_HealingPotion"))
			.Required(TEXT("class_name"), TEXT("string"),  TEXT("UObject class name (same resolution as create_data_asset)."))
			.Required(TEXT("tree"),       TEXT("object"),  TEXT("Nested JSON object of properties to walk against the new asset's reflection schema."))
			.Optional(TEXT("dry_run"),    TEXT("boolean"), TEXT("If true, validate the tree against the class WITHOUT creating the asset."), TEXT("false"))
			.Optional(TEXT("strict"),     TEXT("boolean"), TEXT("If true, promote silent drops / unknown fields / enum misses to hard errors."), TEXT("false"))
			.Optional(TEXT("skip_save"),  TEXT("boolean"), TEXT("Skip synchronous package save (default: false)."), TEXT("false"))
			.Optional(TEXT("read_back_values"), TEXT("boolean"), TEXT("If true, after the write succeeds re-read the written top-level fields' live values and attach them as 'values': { field: <json> }. Pure read-only verify (no extra transaction, no extra dirty). Default: false."), TEXT("false"))
			.Build());
}

// ============================================================
//  create_user_defined_struct
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("fields"), FieldsArray) || !FieldsArray || FieldsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: fields (array of {name, type, default_value?})"));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset (same pattern as create_blueprint)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	// Create the user defined struct
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Struct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("FStructureEditorUtils::CreateUserDefinedStruct failed for: %s"), *AssetName));
	}

	// CreateUserDefinedStruct creates one default member variable. We'll track fields added.
	TArray<TSharedPtr<FJsonValue>> FieldResults;
	int32 FieldIndex = 0;

	for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArray)
	{
		const TSharedPtr<FJsonObject>* FieldObjPtr = nullptr;
		if (!FieldVal.IsValid() || !FieldVal->TryGetObject(FieldObjPtr) || !FieldObjPtr || !(*FieldObjPtr).IsValid())
		{
			FieldResults.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Field %d: skipped (not a valid JSON object)"), FieldIndex)));
			FieldIndex++;
			continue;
		}

		const TSharedPtr<FJsonObject>& FieldObj = *FieldObjPtr;
		FString FieldName;
		FieldObj->TryGetStringField(TEXT("name"), FieldName);
		FString TypeStr;
		FieldObj->TryGetStringField(TEXT("type"), TypeStr);

		if (FieldName.IsEmpty() || TypeStr.IsEmpty())
		{
			FieldResults.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Field %d: skipped (missing name or type)"), FieldIndex)));
			FieldIndex++;
			continue;
		}

		// Parse the type string to FEdGraphPinType
		FEdGraphPinType PinType = MonolithBlueprintInternal::ParsePinTypeFromString(TypeStr);

		// The first field replaces the default member created by CreateUserDefinedStruct.
		// Subsequent fields need AddVariable.
		if (FieldIndex > 0)
		{
			bool bAdded = FStructureEditorUtils::AddVariable(Struct, PinType);
			if (!bAdded)
			{
				TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
				FieldResult->SetStringField(TEXT("name"), FieldName);
				FieldResult->SetStringField(TEXT("error"), TEXT("AddVariable failed"));
				FieldResults.Add(MakeShared<FJsonValueObject>(FieldResult));
				FieldIndex++;
				continue;
			}
		}
		else
		{
			// For the first field, change the type of the default variable
			TArray<FStructVariableDescription>& VarDesc = FStructureEditorUtils::GetVarDesc(Struct);
			if (VarDesc.Num() > 0)
			{
				FStructureEditorUtils::ChangeVariableType(Struct, VarDesc[0].VarGuid, PinType);
			}
		}

		// Get the VarDesc for the field we just added/modified (it's the last entry for added, or first for index 0)
		TArray<FStructVariableDescription>& VarDesc = FStructureEditorUtils::GetVarDesc(Struct);
		int32 DescIndex = (FieldIndex == 0) ? 0 : VarDesc.Num() - 1;

		if (VarDesc.IsValidIndex(DescIndex))
		{
			FGuid VarGuid = VarDesc[DescIndex].VarGuid;

			// Rename the variable to the desired display name
			FStructureEditorUtils::RenameVariable(Struct, VarGuid, FieldName);

			// Set default value if provided
			FString DefaultValue;
			FieldObj->TryGetStringField(TEXT("default_value"), DefaultValue);
			if (!DefaultValue.IsEmpty())
			{
				FStructureEditorUtils::ChangeVariableDefaultValue(Struct, VarGuid, DefaultValue);
			}

			TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
			FieldResult->SetStringField(TEXT("name"), FieldName);
			FieldResult->SetStringField(TEXT("type"), TypeStr);
			FieldResult->SetStringField(TEXT("guid"), VarGuid.ToString());
			if (!DefaultValue.IsEmpty())
			{
				FieldResult->SetStringField(TEXT("default_value"), DefaultValue);
			}
			FieldResults.Add(MakeShared<FJsonValueObject>(FieldResult));
		}

		FieldIndex++;
	}

	// Compile the struct
	FStructureEditorUtils::CompileStructure(Struct);

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Struct);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Struct, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetNumberField(TEXT("field_count"), FieldResults.Num());
	Root->SetArrayField(TEXT("fields"), FieldResults);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_user_defined_enum
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateUserDefinedEnum(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ValuesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("values"), ValuesArray) || !ValuesArray || ValuesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: values (array of strings)"));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	// Create the user defined enum — returns UEnum*, cast to UUserDefinedEnum*
	UEnum* RawEnum = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName), RF_Public | RF_Standalone);
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(RawEnum);
	if (!Enum)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("FEnumEditorUtils::CreateUserDefinedEnum failed for: %s"), *AssetName));
	}

	// FEnumEditorUtils::CreateUserDefinedEnum seeds the enum via SetEnums(EmptyNames),
	// which leaves ZERO user enumerators plus the auto-appended _MAX sentinel
	// (NumEnums() == 1, index 0 == _MAX). It does NOT create a default user entry.
	// AddNewEnumeratorForUserDefinedEnum appends exactly ONE real enumerator each call
	// (it copies the existing entries without _MAX, adds one, then re-appends _MAX).
	// So for N requested values we must Add exactly N times — the previous loop ran
	// (N-1) times, authoring only N-1 real enumerators and leaving the last requested
	// value's SetEnumeratorDisplayName to land on the _MAX sentinel slot (orphaned).
	int32 NumValues = ValuesArray->Num();
	for (int32 i = 0; i < NumValues; i++)
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
	}

	// Set display names for each enumerator
	TArray<TSharedPtr<FJsonValue>> ValueResults;
	for (int32 i = 0; i < NumValues; i++)
	{
		FString DisplayName;
		if ((*ValuesArray)[i].IsValid())
		{
			DisplayName = (*ValuesArray)[i]->AsString();
		}

		if (!DisplayName.IsEmpty())
		{
			FEnumEditorUtils::SetEnumeratorDisplayName(Enum, i, FText::FromString(DisplayName));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), i);
		Entry->SetStringField(TEXT("display_name"), DisplayName);
		// Get the internal name for reference
		if (i < Enum->NumEnums() - 1) // -1 to skip _MAX
		{
			Entry->SetStringField(TEXT("internal_name"), Enum->GetNameStringByIndex(i));
		}
		ValueResults.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Enum);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Enum, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetNumberField(TEXT("enumerator_count"), NumValues);
	Root->SetArrayField(TEXT("values"), ValueResults);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  Helper: Resolve a UScriptStruct by name
// ============================================================

static UScriptStruct* ResolveScriptStruct(const FString& StructName)
{
	FString NormalizedStructName = StructName;
	NormalizedStructName.TrimStartAndEndInline();
	if (NormalizedStructName.IsEmpty())
	{
		return nullptr;
	}

	auto TryResolveObjectPath = [](const FString& ObjectPath) -> UScriptStruct*
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* LoadedStruct = LoadObject<UScriptStruct>(nullptr, *ObjectPath))
		{
			return LoadedStruct;
		}
		if (UScriptStruct* MemoryStruct = FindObject<UScriptStruct>(nullptr, *ObjectPath))
		{
			return MemoryStruct;
		}
		if (UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			if (UScriptStruct* LoadedStruct = Cast<UScriptStruct>(LoadedObject))
			{
				return LoadedStruct;
			}
		}
		if (UObject* SoftLoadedObject = FSoftObjectPath(ObjectPath).TryLoad())
		{
			if (UScriptStruct* LoadedStruct = Cast<UScriptStruct>(SoftLoadedObject))
			{
				return LoadedStruct;
			}
		}
		if (UObject* MemoryObject = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			if (UScriptStruct* MemoryStruct = Cast<UScriptStruct>(MemoryObject))
			{
				return MemoryStruct;
			}
		}
		return nullptr;
	};

	if (NormalizedStructName.StartsWith(TEXT("/")))
	{
		FString ObjectPath = NormalizedStructName;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			int32 LastSlash = INDEX_NONE;
			if (ObjectPath.FindLastChar(TEXT('/'), LastSlash) && LastSlash >= 0 && LastSlash + 1 < ObjectPath.Len())
			{
				ObjectPath += TEXT(".");
				ObjectPath += ObjectPath.Mid(LastSlash + 1);
			}
		}

		if (UScriptStruct* ResolvedStruct = TryResolveObjectPath(ObjectPath))
		{
			return ResolvedStruct;
		}

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (!AssetData.IsValid())
		{
			AssetRegistryModule.Get().ScanPathsSynchronous(
				{ FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(ObjectPath)) },
				/*bForceRescan=*/true);
			AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		}
		if (AssetData.IsValid())
		{
			if (UScriptStruct* ResolvedStruct = TryResolveObjectPath(AssetData.GetSoftObjectPath().ToString()))
			{
				return ResolvedStruct;
			}
		}
	}

	// Try as-is first
	UScriptStruct* Found = FindFirstObject<UScriptStruct>(*NormalizedStructName, EFindFirstObjectOptions::NativeFirst);
	if (Found) return Found;

	// Try with F prefix (common C++ convention: FMyStruct)
	if (!NormalizedStructName.StartsWith(TEXT("F")))
	{
		Found = FindFirstObject<UScriptStruct>(*(TEXT("F") + NormalizedStructName), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
	}

	// Try stripping F prefix if provided
	if (NormalizedStructName.StartsWith(TEXT("F")) && NormalizedStructName.Len() > 1)
	{
		Found = FindFirstObject<UScriptStruct>(*NormalizedStructName.Mid(1), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
	}

	return nullptr;
}

// ============================================================
//  Helper: Serialize a single DataTable row to JSON
// ============================================================

// Get a user-friendly property name — display name for UDS properties, internal name otherwise
static FString GetFriendlyPropertyName(FProperty* Prop)
{
	FString DisplayName = Prop->GetMetaData(TEXT("DisplayName"));
	if (!DisplayName.IsEmpty()) return DisplayName;
	// Fallback: strip GUID suffix from UDS names (e.g., "Name_2_C392053F..." → "Name")
	FString Name = Prop->GetName();
	// UDS properties follow pattern: DisplayName_N_GUID
	int32 FirstUnderscore;
	if (Name.FindChar(TEXT('_'), FirstUnderscore))
	{
		FString Prefix = Name.Left(FirstUnderscore);
		// Check if next char after underscore is a digit (UDS naming pattern)
		if (FirstUnderscore + 1 < Name.Len() && FChar::IsDigit(Name[FirstUnderscore + 1]))
		{
			return Prefix;
		}
	}
	return Name;
}

static TSharedPtr<FJsonObject> SerializeRowToJson(const UScriptStruct* RowStruct, const uint8* RowData)
{
	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Prop = *It;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
		ValuesObj->SetStringField(GetFriendlyPropertyName(Prop), ValueStr);
	}
	return ValuesObj;
}

struct FDataTableMutationOptions
{
	bool bDryRun = false;
	bool bConfirm = false;
	bool bSave = false;
};

static bool ReadRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(FieldName, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
	{
		OutError = FString::Printf(TEXT("Missing required parameter: %s"), FieldName);
		return false;
	}
	return true;
}

static bool ReadOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& OutValue, FString& OutError)
{
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> FieldValue = Params->TryGetField(FieldName);
	if (!FieldValue.IsValid() || FieldValue->IsNull())
	{
		return true;
	}
	if (FieldValue->Type != EJson::Boolean || !Params->TryGetBoolField(FieldName, OutValue))
	{
		OutError = FString::Printf(TEXT("Malformed parameter: %s must be a boolean"), FieldName);
		return false;
	}
	return true;
}

static bool ReadDataTableMutationOptions(const TSharedPtr<FJsonObject>& Params, FDataTableMutationOptions& OutOptions, FString& OutError)
{
	if (!ReadOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError) ||
		!ReadOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError) ||
		!ReadOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError))
	{
		return false;
	}
	if (!OutOptions.bDryRun && !OutOptions.bConfirm)
	{
		OutError = TEXT("Mutating DataTable actions require dry_run=true or confirm=true");
		return false;
	}
	return true;
}

static UDataTable* LoadDataTableFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	if (!ReadRequiredStringParam(Params, TEXT("asset_path"), OutAssetPath, OutError))
	{
		return nullptr;
	}

	OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(OutAssetPath);
	if (!DataTable)
	{
		OutError = FString::Printf(TEXT("DataTable not found: %s"), *OutAssetPath);
		return nullptr;
	}
	if (!DataTable->GetRowStruct())
	{
		OutError = FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *OutAssetPath);
		return nullptr;
	}
	return DataTable;
}

static bool ResolveProjectFilePath(const FString& RawPath, FString& OutFilePath, FString& OutError)
{
	if (RawPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Missing required parameter: file_path");
		return false;
	}

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	FString ProjectPrefix = ProjectDir;
	if (!ProjectPrefix.EndsWith(TEXT("/")))
	{
		ProjectPrefix += TEXT("/");
	}

	OutFilePath = FPaths::IsRelative(RawPath)
		? FPaths::ConvertRelativePathToFull(ProjectDir, RawPath)
		: FPaths::ConvertRelativePathToFull(RawPath);
	FPaths::NormalizeFilename(OutFilePath);

	if (!(OutFilePath.Equals(ProjectDir, ESearchCase::IgnoreCase) || OutFilePath.StartsWith(ProjectPrefix, ESearchCase::IgnoreCase)))
	{
		OutError = FString::Printf(TEXT("DataTable CSV file path '%s' must stay under project directory '%s'"), *OutFilePath, *ProjectDir);
		return false;
	}
	return true;
}

static FProperty* FindDataTableProperty(const UScriptStruct* RowStruct, const FString& FieldName)
{
	if (!RowStruct)
	{
		return nullptr;
	}

	if (FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldName)))
	{
		return Prop;
	}

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		if (It->GetName().Equals(FieldName, ESearchCase::IgnoreCase))
		{
			return *It;
		}

		const FString DisplayName = It->GetMetaData(TEXT("DisplayName"));
		if (!DisplayName.IsEmpty() && DisplayName.Equals(FieldName, ESearchCase::IgnoreCase))
		{
			return *It;
		}

		FString PropName = It->GetName();
		int32 UnderscoreIdx = INDEX_NONE;
		if (PropName.FindChar(TEXT('_'), UnderscoreIdx) && PropName.Left(UnderscoreIdx).Equals(FieldName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}

	return nullptr;
}

static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& JsonVal, const FProperty* Property)
{
	if (!JsonVal.IsValid())
	{
		return FString();
	}
	if (JsonVal->Type == EJson::Number)
	{
		const double NumberValue = JsonVal->AsNumber();
		const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
		if (NumericProperty && NumericProperty->IsInteger())
		{
			const int64 WholeValue = static_cast<int64>(NumberValue);
			if (FMath::IsNearlyEqual(NumberValue, static_cast<double>(WholeValue)))
			{
				return FString::Printf(TEXT("%lld"), WholeValue);
			}
		}
		return FString::SanitizeFloat(NumberValue);
	}
	if (JsonVal->Type == EJson::Boolean)
	{
		return JsonVal->AsBool() ? TEXT("true") : TEXT("false");
	}
	return JsonVal->AsString();
}

static void PopulateDataTableRowFromJson(const UScriptStruct* RowStruct, uint8* RowData, const TSharedPtr<FJsonObject>& ValuesObj, TArray<FString>& OutSetFields, TArray<FString>& OutSkippedFields)
{
	for (const auto& Pair : FMonolithJsonUtils::GetFields(ValuesObj))
	{
		const FString FieldName = Pair.Key;
		FProperty* Prop = FindDataTableProperty(RowStruct, FieldName);
		if (!Prop)
		{
			OutSkippedFields.Add(FString::Printf(TEXT("%s (not found)"), *FieldName));
			continue;
		}

		const FString ValueStr = JsonValueToImportText(Pair.Value, Prop);
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueStr, ValuePtr, nullptr, PPF_None);
		if (ImportResult)
		{
			OutSetFields.Add(FieldName);
		}
		else
		{
			OutSkippedFields.Add(FString::Printf(TEXT("%s (ImportText failed for value: %s)"), *FieldName, *ValueStr));
		}
	}
}

static TArray<TSharedPtr<FJsonValue>> StringsToJsonValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

static void AddDataTableMutationFields(TSharedPtr<FJsonObject>& Result, const FString& AssetPath, const FDataTableMutationOptions& Options, bool bChanged, bool bSaved)
{
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("confirm_received"), Options.bConfirm);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("saved"), bSaved);
}

// ============================================================
//  create_data_table
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString RowStructName;
	Params->TryGetStringField(TEXT("row_struct"), RowStructName);
	if (RowStructName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_struct"));
	}

	// Resolve the row struct
	UScriptStruct* RowStruct = ResolveScriptStruct(RowStructName);
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find UScriptStruct '%s'. Tried as-is, with 'F' prefix, and without 'F' prefix."), *RowStructName));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	// Create the DataTable
	UDataTable* DataTable = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create UDataTable: %s"), *AssetName));
	}

	DataTable->RowStruct = RowStruct;

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DataTable);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  add_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleAddDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString RowName;
	Params->TryGetStringField(TEXT("row_name"), RowName);
	if (RowName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name"));
	}

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("values"), ValuesObj) || !ValuesObj || !(*ValuesObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: values (JSON object of column->value)"));
	}

	// Load the DataTable
	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *AssetPath));
	}

	// Check if row already exists
	FName RowFName(*RowName);
	if (DataTable->GetRowMap().Contains(RowFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' already exists in DataTable '%s'. Remove it first or use a different name."), *RowName, *AssetPath));
	}

	// Allocate and initialize row memory
	const int32 StructSize = RowStruct->GetStructureSize();
	uint8* RowData = static_cast<uint8*>(FMemory::Malloc(StructSize));
	RowStruct->InitializeStruct(RowData);

	// Populate fields from the values JSON object
	TArray<FString> SetFields;
	TArray<FString> SkippedFields;

	for (const auto& Pair : FMonolithJsonUtils::GetFields(*ValuesObj))
	{
		const FString FieldName = Pair.Key;
		const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

		// Find property by name — try exact, case-insensitive, then display name
		// UDS properties have GUID-encoded names (e.g., "Name_2_C392053F...") but
		// callers use friendly display names ("Name"). Check DisplayName metadata.
		FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				// Case-insensitive internal name match
				if (It->GetName().Equals(FieldName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
				// Display name match (critical for UserDefinedStructs)
				FString DisplayName = It->GetMetaData(TEXT("DisplayName"));
				if (!DisplayName.IsEmpty() && DisplayName.Equals(FieldName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
				// Also try stripping the GUID suffix: "Name_2_GUID" → check if starts with "FieldName_"
				FString PropName = It->GetName();
				int32 UnderscoreIdx;
				if (PropName.FindChar(TEXT('_'), UnderscoreIdx))
				{
					FString ShortName = PropName.Left(UnderscoreIdx);
					if (ShortName.Equals(FieldName, ESearchCase::IgnoreCase))
					{
						Prop = *It;
						break;
					}
				}
			}
		}

		if (!Prop)
		{
			SkippedFields.Add(FString::Printf(TEXT("%s (not found)"), *FieldName));
			continue;
		}

		// Convert JSON value to string for ImportText
		FString ValueStr;
		if (JsonVal.IsValid())
		{
			if (JsonVal->Type == EJson::Number)
			{
				ValueStr = FString::SanitizeFloat(JsonVal->AsNumber());
			}
			else if (JsonVal->Type == EJson::Boolean)
			{
				ValueStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				ValueStr = JsonVal->AsString();
			}
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

		// UserDefinedEnum fields inside a UserDefinedStruct compile to a plain
		// numeric FProperty (no FEnumProperty), so a friendly/authored enum name
		// would fail the numeric ImportText below. Resolve such tokens to the
		// enum's integer value first via the shared walker helper; bare integers
		// return false and fall through to the existing ImportText path.
		int64 ResolvedEnumValue = 0;
		if (FMonolithReflectionWalker::ResolveUserDefinedEnumToken(Prop, ValueStr, ResolvedEnumValue))
		{
			if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
			{
				NumProp->SetIntPropertyValue(ValuePtr, ResolvedEnumValue);
				SetFields.Add(FieldName);
				continue;
			}
		}

		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueStr, ValuePtr, nullptr, PPF_None);
		if (ImportResult)
		{
			SetFields.Add(FieldName);
		}
		else
		{
			SkippedFields.Add(FString::Printf(TEXT("%s (ImportText failed for value: %s)"), *FieldName, *ValueStr));
		}
	}

	// Add the row to the DataTable — uses the uint8*/UScriptStruct overload which copies internally
	DataTable->AddRow(RowFName, RowData, RowStruct);

	// Free our temporary copy
	RowStruct->DestroyStruct(RowData);
	FMemory::Free(RowData);

	DataTable->Modify();
	DataTable->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetNumberField(TEXT("fields_set"), SetFields.Num());

	TArray<TSharedPtr<FJsonValue>> SetArr;
	for (const FString& F : SetFields) SetArr.Add(MakeShared<FJsonValueString>(F));
	Root->SetArrayField(TEXT("set_fields"), SetArr);

	if (SkippedFields.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SkipArr;
		for (const FString& F : SkippedFields) SkipArr.Add(MakeShared<FJsonValueString>(F));
		Root->SetArrayField(TEXT("skipped_fields"), SkipArr);
	}

	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_data_table_rows
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleGetDataTableRows(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString RowNameFilter;
	Params->TryGetStringField(TEXT("row_name"), RowNameFilter);

	// Load the DataTable
	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *AssetPath));
	}

	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();

	TArray<TSharedPtr<FJsonValue>> RowResults;

	if (!RowNameFilter.IsEmpty())
	{
		// Single row lookup
		const FName RowFName(*RowNameFilter);
		const uint8* const* FoundRow = RowMap.Find(RowFName);
		if (!FoundRow || !(*FoundRow))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Row '%s' not found in DataTable '%s'"), *RowNameFilter, *AssetPath));
		}

		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowNameFilter);
		RowObj->SetObjectField(TEXT("values"), SerializeRowToJson(RowStruct, *FoundRow));
		RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
	}
	else
	{
		// All rows
		for (const auto& Pair : RowMap)
		{
			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
			RowObj->SetObjectField(TEXT("values"), SerializeRowToJson(RowStruct, Pair.Value));
			RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
		}
	}

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetNumberField(TEXT("row_count"), RowResults.Num());
	Root->SetNumberField(TEXT("total_rows"), RowMap.Num());
	Root->SetArrayField(TEXT("rows"), RowResults);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_data_table_schema
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleGetDataTableSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UDataTable* DataTable = LoadDataTableFromParams(Params, AssetPath, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	TArray<TSharedPtr<FJsonValue>> Columns;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Prop = *It;
		TSharedPtr<FJsonObject> Column = MakeShared<FJsonObject>();
		Column->SetStringField(TEXT("name"), GetFriendlyPropertyName(Prop));
		Column->SetStringField(TEXT("internal_name"), Prop->GetName());
		Column->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
		Column->SetStringField(TEXT("property_class"), Prop->GetClass()->GetName());
		Columns.Add(MakeShared<FJsonValueObject>(Column));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Root->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
	Root->SetNumberField(TEXT("column_count"), Columns.Num());
	Root->SetArrayField(TEXT("columns"), Columns);
	Root->SetBoolField(TEXT("read_only"), true);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  update_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleUpdateDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	FDataTableMutationOptions Options;
	FString Error;
	if (!ReadDataTableMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RowName;
	if (!ReadRequiredStringParam(Params, TEXT("row_name"), RowName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("values"), ValuesObj) || !ValuesObj || !(*ValuesObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: values (JSON object of column->value)"));
	}

	bool bCreateIfMissing = false;
	if (!ReadOptionalBoolParam(Params, TEXT("create_if_missing"), bCreateIfMissing, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UDataTable* DataTable = LoadDataTableFromParams(Params, AssetPath, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	const FName RowFName(*RowName);
	uint8* const* ExistingRowPtr = DataTable->GetRowMap().Find(RowFName);
	const bool bHadRow = ExistingRowPtr && *ExistingRowPtr;
	if (!bHadRow && !bCreateIfMissing)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' not found in DataTable '%s'. Pass create_if_missing=true to create it."), *RowName, *AssetPath));
	}

	const int32 StructSize = RowStruct->GetStructureSize();
	uint8* RowData = static_cast<uint8*>(FMemory::Malloc(StructSize));
	RowStruct->InitializeStruct(RowData);
	if (bHadRow)
	{
		RowStruct->CopyScriptStruct(RowData, *ExistingRowPtr);
	}

	TArray<FString> SetFields;
	TArray<FString> SkippedFields;
	PopulateDataTableRowFromJson(RowStruct, RowData, *ValuesObj, SetFields, SkippedFields);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	AddDataTableMutationFields(Root, AssetPath, Options, false, false);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetBoolField(TEXT("row_existed"), bHadRow);
	Root->SetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
	Root->SetNumberField(TEXT("fields_set"), SetFields.Num());
	Root->SetArrayField(TEXT("set_fields"), StringsToJsonValues(SetFields));
	if (SkippedFields.Num() > 0)
	{
		Root->SetArrayField(TEXT("skipped_fields"), StringsToJsonValues(SkippedFields));
	}
	Root->SetObjectField(TEXT("preview_values"), SerializeRowToJson(RowStruct, RowData));

	const bool bChanged = SetFields.Num() > 0 || !bHadRow;
	if (Options.bDryRun)
	{
		Root->SetBoolField(TEXT("would_update"), bChanged);
		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);
		return FMonolithActionResult::Success(Root);
	}

	if (bChanged)
	{
		DataTable->Modify();
		DataTable->AddRow(RowFName, RowData, RowStruct);
		DataTable->MarkPackageDirty();
	}
	RowStruct->DestroyStruct(RowData);
	FMemory::Free(RowData);

	bool bSaved = false;
	if (bChanged && Options.bSave)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);
	}

	AddDataTableMutationFields(Root, AssetPath, Options, bChanged, bSaved);
	if (uint8* UpdatedRow = DataTable->FindRowUnchecked(RowFName))
	{
		Root->SetObjectField(TEXT("row"), SerializeRowToJson(RowStruct, UpdatedRow));
	}
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleRemoveDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	FDataTableMutationOptions Options;
	FString Error;
	if (!ReadDataTableMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RowName;
	if (!ReadRequiredStringParam(Params, TEXT("row_name"), RowName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UDataTable* DataTable = LoadDataTableFromParams(Params, AssetPath, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FName RowFName(*RowName);
	const uint8* ExistingRow = DataTable->FindRowUnchecked(RowFName);
	if (!ExistingRow)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Row '%s' not found in DataTable '%s'"), *RowName, *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	AddDataTableMutationFields(Root, AssetPath, Options, false, false);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetObjectField(TEXT("removed_row_preview"), SerializeRowToJson(DataTable->GetRowStruct(), ExistingRow));
	if (Options.bDryRun)
	{
		Root->SetBoolField(TEXT("would_remove"), true);
		return FMonolithActionResult::Success(Root);
	}

	DataTable->Modify();
	DataTable->RemoveRow(RowFName);
	DataTable->MarkPackageDirty();

	bool bSaved = false;
	if (Options.bSave)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);
	}

	AddDataTableMutationFields(Root, AssetPath, Options, true, bSaved);
	Root->SetNumberField(TEXT("remaining_rows"), DataTable->GetRowMap().Num());
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  export_data_table_csv
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleExportDataTableCsv(const TSharedPtr<FJsonObject>& Params)
{
	FDataTableMutationOptions Options;
	FString Error;
	if (!ReadDataTableMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RawFilePath;
	if (!ReadRequiredStringParam(Params, TEXT("file_path"), RawFilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString FilePath;
	if (!ResolveProjectFilePath(RawFilePath, FilePath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString AssetPath;
	UDataTable* DataTable = LoadDataTableFromParams(Params, AssetPath, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString Csv = DataTable->GetTableAsCSV();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	AddDataTableMutationFields(Root, AssetPath, Options, false, false);
	Root->SetStringField(TEXT("file_path"), FilePath);
	Root->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
	const FTCHARToUTF8 CsvUtf8(*Csv);
	Root->SetNumberField(TEXT("byte_count"), CsvUtf8.Length());
	if (Options.bDryRun)
	{
		Root->SetBoolField(TEXT("would_export"), true);
		return FMonolithActionResult::Success(Root);
	}

	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}
	if (!FFileHelper::SaveStringToFile(Csv, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write CSV file '%s'"), *FilePath));
	}
	const int64 ActualByteCount = IFileManager::Get().FileSize(*FilePath);
	if (ActualByteCount >= 0)
	{
		Root->SetNumberField(TEXT("byte_count"), static_cast<double>(ActualByteCount));
	}

	AddDataTableMutationFields(Root, AssetPath, Options, true, true);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_data_asset
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString ClassName;
	Params->TryGetStringField(TEXT("class_name"), ClassName);
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_name"));
	}

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Resolve class_name → UClass*
	UClass* ResolvedClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class not found: '%s'. Tried as-is, with 'U' prefix, and with 'A' prefix. "
				 "Use full path (e.g. /Script/Module.ClassName) for disambiguation."), *ClassName));
	}

	// Guard: reject Blueprint and BlueprintGeneratedClass (use create_blueprint instead)
	if (ResolvedClass->IsChildOf(UBlueprint::StaticClass()) ||
		ResolvedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is a Blueprint class. Use create_blueprint instead."), *ResolvedClass->GetName()));
	}

	// Guard: reject abstract, deprecated, or superseded classes
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		FString Reason;
		if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract)) Reason = TEXT("abstract");
		else if (ResolvedClass->HasAnyClassFlags(CLASS_Deprecated)) Reason = TEXT("deprecated");
		else Reason = TEXT("superseded by a newer version");
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot instantiate class '%s': it is %s."), *ResolvedClass->GetName(), *Reason));
	}

	// Guard: reject Actor-derived classes (use spawn_actor instead)
	if (ResolvedClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is Actor-derived. Actors must live in a ULevel — use spawn_actor or create_blueprint instead."),
			*ResolvedClass->GetName()));
	}

	// Guard against existing asset (2-tier: Asset Registry + FindObject)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	// Create the raw UObject instance
	UObject* NewAsset = NewObject<UObject>(Package, ResolvedClass, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("NewObject failed for class '%s' at path '%s'."), *ResolvedClass->GetName(), *SavePath));
	}

	// Fire edit cradle on all properties — initializes FOverridableManager state (#29).
	NewAsset->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintStructActions",
		"CreateDataAsset", "Monolith Create Data Asset"));
	NewAsset->Modify();

	for (TFieldIterator<FProperty> It(ResolvedClass); It; ++It)
	{
		FProperty* Prop = *It;
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			continue;
		MonolithEditCradle::FireFullCradle(NewAsset, Prop);
	}

	// Read skip_save param
	bool bSkipSave = false;
	Params->TryGetBoolField(TEXT("skip_save"), bSkipSave);

	// Notify and save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	bool bSaved = false;
	if (!bSkipSave)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewAsset, false);
	}

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("class_name"), ClassName);
	Root->SetStringField(TEXT("actual_class"), ResolvedClass->GetName());
	Root->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  seed_data_asset  (create + populate in one call)
// ============================================================

namespace MonolithSeedDataAssetInternal
{
	// Serialise an FDryRunReport's field writes into the response (same shape as
	// FMonolithDryRunGuard::ReportToJson, inlined here to avoid extra includes).
	static void AppendFieldWrites(const TSharedPtr<FJsonObject>& Root, const FDryRunReport& Report)
	{
		TArray<TSharedPtr<FJsonValue>> Writes;
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("path"), W.Path);
			O->SetStringField(TEXT("current"), W.CurrentValue);
			O->SetStringField(TEXT("proposed"), W.ProposedValue);
			O->SetBoolField(TEXT("ok"), W.bOk);
			if (!W.bOk) { O->SetStringField(TEXT("reason"), W.Reason); }
			Writes.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("field_writes"), Writes);
		Root->SetNumberField(TEXT("errors"), Report.Errors);
	}
}

FMonolithActionResult FMonolithBlueprintStructActions::HandleSeedDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSeedDataAssetInternal;

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_name"));
	}

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	const TSharedPtr<FJsonObject>* TreePtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("tree"), TreePtr) || !TreePtr || !(*TreePtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: tree (JSON object of property->value)"));
	}
	TSharedPtr<FJsonObject> Tree = *TreePtr;

	bool bDryRun = false;
	bool bStrict = false;
	bool bSkipSave = false;
	bool bReadBackValues = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Params->TryGetBoolField(TEXT("strict"), bStrict);
	Params->TryGetBoolField(TEXT("skip_save"), bSkipSave);
	Params->TryGetBoolField(TEXT("read_back_values"), bReadBackValues);

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Resolve class_name → UClass* (same resolution as create_data_asset).
	UClass* ResolvedClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class not found: '%s'. Tried as-is, with 'U' prefix, and with 'A' prefix. "
				 "Use full path (e.g. /Script/Module.ClassName) for disambiguation."), *ClassName));
	}

	// Guard: reject Blueprint / BlueprintGeneratedClass.
	if (ResolvedClass->IsChildOf(UBlueprint::StaticClass()) ||
		ResolvedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is a Blueprint class. Use create_blueprint instead."), *ResolvedClass->GetName()));
	}

	// Guard: reject abstract / deprecated / superseded.
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		FString Reason;
		if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract)) Reason = TEXT("abstract");
		else if (ResolvedClass->HasAnyClassFlags(CLASS_Deprecated)) Reason = TEXT("deprecated");
		else Reason = TEXT("superseded by a newer version");
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot instantiate class '%s': it is %s."), *ResolvedClass->GetName(), *Reason));
	}

	// Guard: reject Actor-derived.
	if (ResolvedClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is Actor-derived. Actors must live in a ULevel — use spawn_actor or create_blueprint instead."),
			*ResolvedClass->GetName()));
	}

	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("blueprint");
	Spec.TargetAsset = SavePath;
	Spec.Tree = Tree;
	Spec.bDryRun = bDryRun;
	Spec.bStrict = bStrict;

	// Dry-run: validate the tree against the class CDO without creating anything.
	if (bDryRun)
	{
		UObject* CDO = ResolvedClass->GetDefaultObject(true);
		FDryRunReport Report = FMonolithReflectionWalker::InspectTree(Tree, ResolvedClass, CDO, Spec);
		Report.bWouldApply = false;

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), SavePath);
		Root->SetStringField(TEXT("actual_class"), ResolvedClass->GetName());
		Root->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
		AppendFieldWrites(Root, Report);
		Root->SetBoolField(TEXT("would_apply"), Report.Errors == 0);
		Root->SetBoolField(TEXT("dry_run"), true);
		Root->SetBoolField(TEXT("saved"), false);
		Root->SetBoolField(TEXT("success"), true);
		return FMonolithActionResult::Success(Root);
	}

	// Guard against existing asset (2-tier: Asset Registry + FindObject).
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package + instance.
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	UObject* NewAsset = NewObject<UObject>(Package, ResolvedClass, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("NewObject failed for class '%s' at path '%s'."), *ResolvedClass->GetName(), *SavePath));
	}

	// Engine edit cradle + walker fill (mirror MonolithBlueprintBulkFillAdapter).
	NewAsset->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintStructActions",
		"SeedDataAsset", "Monolith Seed Data Asset"));
	NewAsset->Modify();
	NewAsset->PreEditChange(nullptr);

	FDryRunReport Report = FMonolithReflectionWalker::WriteTree(Tree, ResolvedClass, NewAsset, NewAsset, Spec);

	// Strict + errors: cancel so the half-written asset never lands.
	if (!Report.bWouldApply)
	{
		Transaction.Cancel();
		// The asset was created in-package; mark transient so it is GC'd rather
		// than persisted. (No FAssetRegistryModule::AssetCreated was fired.)
		NewAsset->ClearFlags(RF_Public | RF_Standalone);
		NewAsset->SetFlags(RF_Transient);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), SavePath);
		Root->SetStringField(TEXT("actual_class"), ResolvedClass->GetName());
		AppendFieldWrites(Root, Report);
		Root->SetBoolField(TEXT("would_apply"), false);
		Root->SetBoolField(TEXT("saved"), false);
		Root->SetBoolField(TEXT("success"), false);
		return FMonolithActionResult::Success(Root);
	}

	// Post-write cradle for the top-level fields the tree touched.
	for (const auto& KV : FMonolithJsonUtils::GetFields(Tree))
	{
		FProperty* TopProp = FMonolithReflectionWalker::FindPropertyForwarding(ResolvedClass, MonolithKeyToString(KV.Key));
		if (TopProp)
		{
			MonolithEditCradle::ReparentTransientInstancedSubobjects(NewAsset, TopProp);
			MonolithEditCradle::FireFullCradle(NewAsset, TopProp);
		}
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	bool bSaved = false;
	if (!bSkipSave)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewAsset, false);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("class_name"), ClassName);
	Root->SetStringField(TEXT("actual_class"), ResolvedClass->GetName());
	Root->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
	AppendFieldWrites(Root, Report);
	Root->SetBoolField(TEXT("would_apply"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);

	// Optional live readback: re-walk the written top-level fields through the
	// shared read-only reflection reader and echo their live values. This is a
	// pure read — it opens NO transaction and marks NOTHING dirty (the write
	// transaction above has already completed).
	if (bReadBackValues)
	{
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		for (const auto& KV : FMonolithJsonUtils::GetFields(Tree))
		{
			FProperty* TopProp = FMonolithReflectionWalker::FindPropertyForwarding(ResolvedClass, MonolithKeyToString(KV.Key));
			if (TopProp)
			{
				const void* ValuePtr = TopProp->ContainerPtrToValuePtr<void>(NewAsset);
				Values->SetField(TopProp->GetName(),
					FMonolithReflectionReader::PropertyToJsonValue(TopProp, ValuePtr, NewAsset));
			}
		}
		Root->SetObjectField(TEXT("values"), Values);
	}

	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}
