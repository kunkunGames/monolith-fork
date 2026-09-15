#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithJsonUtils.h"
#include "MonolithPinTypeGrammar.h"
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

	// --- Field-level editing of an EXISTING struct ------------------------------
	// create_user_defined_struct authors a struct once. Without these a struct is
	// write-once: adding a member to one that shipping Blueprints already break had
	// to be done by hand in the editor, and its schema could not be read back at all.

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_struct_fields"),
		TEXT("Read the field schema of a User Defined Struct, in declaration order. Types are reported in the same grammar add_variable / add_struct_field accept, so a reported type can be fed straight back into the writers. Returns name (the display name the other struct-field actions target), type, guid, var_name, default_value and tooltip. A never-named member is flagged 'placeholder': every new struct is seeded with one because a struct can never be empty, so when reproducing this schema in a FRESH struct, configure that struct's own placeholder (set_struct_field_type / rename_struct_field) rather than adding over it."),
		FMonolithActionHandler::CreateStatic(&HandleGetStructFields),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("User Defined Struct asset path, e.g. /Game/Data/S_MyStruct"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_struct_field"),
		TEXT("Append a field to an existing User Defined Struct, optionally positioned after a named field. Recompiles the struct; assets that break it are reported as 'dependents' but are NOT recompiled by this call. A name that collides with the struct's seeded placeholder member is refused with the recipe for configuring it instead (it cannot be deleted first -- a struct can never be empty)."),
		FMonolithActionHandler::CreateStatic(&HandleAddStructField),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),  TEXT("User Defined Struct asset path"))
			.Required(TEXT("name"),          TEXT("string"),  TEXT("Display name for the new field"))
			.Required(TEXT("type"),          TEXT("string"),  TEXT("Field type, same grammar as add_variable (bool, int, int64, float, double, string, name, text, byte, struct:Vector, object:ClassName, class:ClassName, enum:E_Name, softobject:Texture2D, array:int, set:name, map:string:int)"))
			.Optional(TEXT("default_value"), TEXT("string"),  TEXT("Optional default value, applied via ChangeVariableDefaultValue. The engine validates it against the field type and silently keeps the type default if it does not parse — the response reports whether it took."))
			.Optional(TEXT("after"),         TEXT("string"),  TEXT("Insert directly after this existing field instead of appending at the end"))
			.Optional(TEXT("skip_save"),     TEXT("boolean"), TEXT("Skip the synchronous package save (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_struct_field"),
		TEXT("Remove a field from an existing User Defined Struct. A struct cannot be left empty, so removing the last remaining field is refused. Recompiling drops the member from every Blueprint that breaks this struct — audit 'dependents' before removing."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveStructField),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("User Defined Struct asset path"))
			.Required(TEXT("name"),        TEXT("string"),  TEXT("Display name of the field to remove"))
			.Optional(TEXT("skip_save"),   TEXT("boolean"), TEXT("Skip the synchronous package save (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_struct_field"),
		TEXT("Rename a field on an existing User Defined Struct. The underlying GUID is preserved, so existing Break/Make nodes keep their connections. Names compare case-insensitively, so a case-only rename is refused by the engine."),
		FMonolithActionHandler::CreateStatic(&HandleRenameStructField),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("User Defined Struct asset path"))
			.Required(TEXT("name"),        TEXT("string"),  TEXT("Current display name of the field"))
			.Required(TEXT("new_name"),    TEXT("string"),  TEXT("New display name"))
			.Optional(TEXT("skip_save"),   TEXT("boolean"), TEXT("Skip the synchronous package save (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_struct_field_type"),
		TEXT("Change the type of a field on an existing User Defined Struct. This is a MIGRATION, not a rename: the field's default value is cleared by the engine and pins of the old type on existing Break/Make nodes are disconnected by the recompile."),
		FMonolithActionHandler::CreateStatic(&HandleSetStructFieldType),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("User Defined Struct asset path"))
			.Required(TEXT("name"),        TEXT("string"),  TEXT("Display name of the field"))
			.Required(TEXT("type"),        TEXT("string"),  TEXT("New type, same grammar as add_variable"))
			.Optional(TEXT("skip_save"),   TEXT("boolean"), TEXT("Skip the synchronous package save (default: false)"), TEXT("false"))
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

	// Defensive: reject malformed paths (e.g. "//Game/...") before they reach the Asset
	// Registry or CreatePackage, which asserts in UObjectGlobals.cpp and kills the editor.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
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

	struct FParsedStructField
	{
		FString Name;
		FString TypeString;
		FString DefaultValue;
		FEdGraphPinType PinType;
	};

	// Validate and parse every field before creating a package or struct. Input
	// validation is all-or-nothing: malformed entries, duplicate names, unknown
	// types, and unresolved named types cannot leave a default-only/partial asset.
	TArray<FParsedStructField> ParsedFields;
	ParsedFields.Reserve(FieldsArray->Num());
	TSet<FName> SeenFieldNames;
	for (int32 InputIndex = 0; InputIndex < FieldsArray->Num(); ++InputIndex)
	{
		const TSharedPtr<FJsonValue>& FieldValue = (*FieldsArray)[InputIndex];
		const TSharedPtr<FJsonObject>* FieldObject = nullptr;
		if (!FieldValue.IsValid()
			|| !FieldValue->TryGetObject(FieldObject)
			|| !FieldObject
			|| !FieldObject->IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Field at index %d must be a JSON object."), InputIndex),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FParsedStructField& ParsedField = ParsedFields.AddDefaulted_GetRef();
		(*FieldObject)->TryGetStringField(TEXT("name"), ParsedField.Name);
		(*FieldObject)->TryGetStringField(TEXT("type"), ParsedField.TypeString);
		(*FieldObject)->TryGetStringField(TEXT("default_value"), ParsedField.DefaultValue);
		if (ParsedField.Name.IsEmpty() || ParsedField.TypeString.IsEmpty())
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Field at index %d requires non-empty string fields 'name' and 'type'."),
					InputIndex),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		const FName FieldName(*ParsedField.Name);
		if (SeenFieldNames.Contains(FieldName))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Duplicate struct field name '%s' at index %d."), *ParsedField.Name, InputIndex),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		SeenFieldNames.Add(FieldName);

		FString TypeError;
		if (!MonolithPinTypeGrammar::TryParsePinType(
			ParsedField.TypeString, ParsedField.PinType, TypeError))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Field '%s' at index %d: %s"), *ParsedField.Name, InputIndex, *TypeError),
				FMonolithJsonUtils::ErrInvalidParams);
		}
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

	for (const FParsedStructField& Field : ParsedFields)
	{
		// The first field replaces the default member created by CreateUserDefinedStruct.
		// Subsequent fields need AddVariable.
		if (FieldIndex > 0)
		{
			bool bAdded = FStructureEditorUtils::AddVariable(Struct, Field.PinType);
			if (!bAdded)
			{
				TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
				FieldResult->SetStringField(TEXT("name"), Field.Name);
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
				FStructureEditorUtils::ChangeVariableType(Struct, VarDesc[0].VarGuid, Field.PinType);
			}
		}

		// Get the VarDesc for the field we just added/modified (it's the last entry for added, or first for index 0)
		TArray<FStructVariableDescription>& VarDesc = FStructureEditorUtils::GetVarDesc(Struct);
		int32 DescIndex = (FieldIndex == 0) ? 0 : VarDesc.Num() - 1;

		if (VarDesc.IsValidIndex(DescIndex))
		{
			FGuid VarGuid = VarDesc[DescIndex].VarGuid;

			// Rename the variable to the desired display name
			FStructureEditorUtils::RenameVariable(Struct, VarGuid, Field.Name);

			// Set default value if provided
			if (!Field.DefaultValue.IsEmpty())
			{
				FStructureEditorUtils::ChangeVariableDefaultValue(Struct, VarGuid, Field.DefaultValue);
			}

			TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
			FieldResult->SetStringField(TEXT("name"), Field.Name);
			FieldResult->SetStringField(TEXT("type"), Field.TypeString);
			FieldResult->SetStringField(TEXT("guid"), VarGuid.ToString());
			if (!Field.DefaultValue.IsEmpty())
			{
				FieldResult->SetStringField(TEXT("default_value"), Field.DefaultValue);
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

	// Defensive: reject malformed paths (e.g. "//Game/...") before they reach the Asset
	// Registry or CreatePackage, which asserts in UObjectGlobals.cpp and kills the editor.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
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

static TArray<TSharedPtr<FJsonValue>> DataTableStringsToJsonValues(const TArray<FString>& Values)
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

	// Reject malformed paths before they reach the Asset Registry or CreatePackage.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
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
	Root->SetArrayField(TEXT("set_fields"), DataTableStringsToJsonValues(SetFields));
	if (SkippedFields.Num() > 0)
	{
		Root->SetArrayField(TEXT("skipped_fields"), DataTableStringsToJsonValues(SkippedFields));
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

	// Reject malformed paths before they reach the Asset Registry or CreatePackage.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	FString ClassName;
	Params->TryGetStringField(TEXT("class_name"), ClassName);
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_name"));
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

	// Reject malformed paths before they reach the Asset Registry or CreatePackage.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_name"));
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

// ============================================================
//  Field-level editing of an existing User Defined Struct
//
//  FStructureEditorUtils already exposed everything needed here; it was only ever
//  reachable through create_user_defined_struct, which runs once at authoring
//  time. Every engine writer called below opens its own FScopedTransaction, so
//  nothing here opens one -- nesting would only widen the undo scope.
// ============================================================

namespace MonolithStructFieldDetail
{
	/**
	 * Load a User Defined Struct AND its description array, or explain why not.
	 *
	 * The array is fetched through GetVarDescPtr rather than GetVarDesc because
	 * GetVarDesc is `CastChecked<UUserDefinedStructEditorData>(Struct->EditorData)`
	 * -- a struct whose editor data failed to load is a FATAL assert there, not a
	 * null return. GetVarDescPtr null-checks EditorData first, which turns that
	 * into an answerable error.
	 *
	 * The returned pointer is to the array OBJECT, which is stable across the
	 * engine writers. Pointers to its ELEMENTS are not: AddVariable reallocates.
	 */
	static UUserDefinedStruct* LoadStruct(const TSharedPtr<FJsonObject>& Params, FString& OutPath,
		TArray<FStructVariableDescription>*& OutDesc, FString& OutError)
	{
		OutError.Reset();
		OutDesc = nullptr;
		OutPath = Params.IsValid() ? Params->GetStringField(TEXT("asset_path")) : FString();
		if (OutPath.IsEmpty())
		{
			OutError = TEXT("Missing required parameter: asset_path");
			return nullptr;
		}

		UUserDefinedStruct* Struct = FMonolithAssetUtils::LoadAssetByPath<UUserDefinedStruct>(OutPath);
		if (!Struct)
		{
			// Distinguish "wrong kind of asset" from "no asset": pointing this at a
			// native struct or a DataTable is a likely mistake worth naming.
			if (UObject* Other = FMonolithAssetUtils::LoadAssetByPath(OutPath))
			{
				OutError = FString::Printf(
					TEXT("Asset at %s is a %s, not a User Defined Struct. Only User Defined Structs have an editable field list; native structs are defined in C++."),
					*OutPath, *Other->GetClass()->GetName());
			}
			else
			{
				OutError = FString::Printf(TEXT("User Defined Struct not found: %s"), *OutPath);
			}
			return nullptr;
		}

		OutDesc = FStructureEditorUtils::GetVarDescPtr(Struct);
		if (!OutDesc)
		{
			OutError = FString::Printf(
				TEXT("User Defined Struct %s has no editor data, so its field list cannot be read or edited. The asset is cooked or damaged."),
				*OutPath);
			return nullptr;
		}
		return Struct;
	}

	/**
	 * Resolve a caller-supplied field name.
	 *
	 * Callers see the FRIENDLY name (Mobility); the serialized VarName carries a
	 * disambiguating suffix (Mobility_36_31089BED...). Match the friendly name
	 * first, then accept the raw VarName so a name copied out of a T3D dump or an
	 * error message still resolves.
	 */
	static const FStructVariableDescription* FindField(const TArray<FStructVariableDescription>& Desc, const FString& FieldName)
	{
		for (const FStructVariableDescription& D : Desc)
		{
			if (D.FriendlyName.Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return &D;
			}
		}
		for (const FStructVariableDescription& D : Desc)
		{
			if (D.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return &D;
			}
		}
		return nullptr;
	}

	/**
	 * True for a member that still carries the name the ENGINE generated for it,
	 * i.e. one that nobody has ever named.
	 *
	 * This matters because a User Defined Struct can never be empty --
	 * RemoveVariable hardcodes bAllowToMakeEmpty = false -- so the engine seeds
	 * every newly created struct with one placeholder bool. That member is added
	 * with an empty name base, which FMemberVariableNameHelper::Generate renders
	 * as "MemberVar_<n>"; RenameVariable afterwards writes the caller's string
	 * verbatim, so a field still matching that shape has never been renamed.
	 * (Identical in 5.7 and 5.8: StructureEditorUtils.cpp:235-263.)
	 *
	 * A caller replaying a get_struct_fields dump into a fresh struct collides
	 * with this member on its first write and CANNOT delete it out of the way
	 * first. Identifying it is what makes such a dump actionable, so it is
	 * flagged on read and named in the collision error.
	 */
	static bool IsUnnamedPlaceholder(const FString& FriendlyName)
	{
		const FString Prefix(TEXT("MemberVar_"));
		if (!FriendlyName.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Suffix = FriendlyName.RightChop(Prefix.Len());
		if (Suffix.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Char : Suffix)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	/** Comma-joined field list, so a no-such-field error can be acted on. */
	static FString DescribeAvailableFields(const TArray<FStructVariableDescription>& Desc)
	{
		TArray<FString> Names;
		for (const FStructVariableDescription& D : Desc)
		{
			Names.Add(D.FriendlyName.IsEmpty() ? D.VarName.ToString() : D.FriendlyName);
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("(none)"));
	}

	/**
	 * Render a pin type in the grammar the writers parse.
	 *
	 * PinTypeToString describes only the TERMINAL half, so it is paired with
	 * ContainerPrefix (the convention the rest of the module uses) -- an array:int
	 * field would otherwise be reported as plain `int`. A map needs both halves
	 * spelled out: "map:" plus the key type alone parses back as "a map with no
	 * value type", which is a hard error in the grammar, so the value terminal is
	 * rebuilt into a pin type and appended.
	 */
	static FString DescribePinType(const FEdGraphPinType& PinType)
	{
		const FString Terminal = MonolithPinTypeGrammar::PinTypeToString(PinType);
		if (PinType.ContainerType != EPinContainerType::Map)
		{
			return MonolithPinTypeGrammar::ContainerPrefix(PinType) + Terminal;
		}

		FEdGraphPinType ValueAsPin;
		ValueAsPin.PinCategory          = PinType.PinValueType.TerminalCategory;
		ValueAsPin.PinSubCategory       = PinType.PinValueType.TerminalSubCategory;
		ValueAsPin.PinSubCategoryObject = PinType.PinValueType.TerminalSubCategoryObject;
		return TEXT("map:") + Terminal + TEXT(":") + MonolithPinTypeGrammar::PinTypeToString(ValueAsPin);
	}

	static TSharedPtr<FJsonObject> DescribeField(const FStructVariableDescription& D)
	{
		const FString Name = D.FriendlyName.IsEmpty() ? D.VarName.ToString() : D.FriendlyName;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		// Reported in the same grammar the writers accept, so get -> add round-trips.
		Obj->SetStringField(TEXT("type"), DescribePinType(D.ToPinType()));
		if (IsUnnamedPlaceholder(Name))
		{
			// Flagged rather than hidden: it IS a real member of the struct, but it
			// is the one a caller replaying this dump has to configure rather than
			// add (see IsUnnamedPlaceholder).
			Obj->SetBoolField(TEXT("placeholder"), true);
		}
		Obj->SetStringField(TEXT("guid"), D.VarGuid.ToString());
		Obj->SetStringField(TEXT("var_name"), D.VarName.ToString());
		if (!D.DefaultValue.IsEmpty()) { Obj->SetStringField(TEXT("default_value"), D.DefaultValue); }
		if (!D.ToolTip.IsEmpty())      { Obj->SetStringField(TEXT("tooltip"), D.ToolTip); }
		return Obj;
	}

	/** Strict token parse -- a bogus type is a caller error, not a silent bool field. */
	static bool ParseFieldType(const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError)
	{
		if (!MonolithPinTypeGrammar::TryParsePinType(TypeStr, OutPinType, OutError))
		{
			OutError = FString::Printf(TEXT("Invalid type '%s': %s"), *TypeStr, *OutError);
			return false;
		}
		return true;
	}

	/**
	 * Assets that reference this struct's package.
	 *
	 * Reported, not acted on. The engine writers call OnStructureChanged, which
	 * recompiles the STRUCT and broadcasts its change event; Blueprints that break
	 * the struct pick the change up when they are next compiled or loaded, not
	 * here. Naming them is what lets a caller audit the blast radius.
	 */
	static void AddDependents(UUserDefinedStruct* Struct, TSharedPtr<FJsonObject>& Root)
	{
		const UPackage* Package = Struct->GetOutermost();
		if (!Package) { return; }

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetIdentifier> Referencers;
		AR.GetReferencers(FAssetIdentifier(Package->GetFName()), Referencers);

		// Capped: a widely-used struct can have hundreds of referencers, and the
		// count is the actionable part.
		const int32 MaxListed = 25;
		TArray<TSharedPtr<FJsonValue>> Listed;
		for (const FAssetIdentifier& Ref : Referencers)
		{
			if (Listed.Num() >= MaxListed) { break; }
			Listed.Add(MakeShared<FJsonValueString>(Ref.ToString()));
		}

		Root->SetNumberField(TEXT("dependent_count"), Referencers.Num());
		Root->SetArrayField(TEXT("dependents"), Listed);
		if (Referencers.Num() > MaxListed)
		{
			Root->SetNumberField(TEXT("dependents_truncated"), Referencers.Num() - MaxListed);
		}
		if (Referencers.Num() > 0)
		{
			Root->SetStringField(TEXT("dependents_note"),
				TEXT("These assets reference this struct. The struct itself was recompiled; the referencing assets were NOT -- they pick the change up when next compiled or loaded."));
		}
	}

	/** Recompile, report, then optionally save. Shared tail for every writer below. */
	static void CommitStruct(UUserDefinedStruct* Struct, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& Root)
	{
		// The engine writers already recompiled via OnStructureChanged; this is the
		// same belt-and-braces pass create_user_defined_struct makes, and it
		// guarantees a compiled struct is what gets written to disk below.
		FStructureEditorUtils::CompileStructure(Struct);
		Root->SetBoolField(TEXT("recompiled"), true);

		const TArray<FStructVariableDescription>* Desc = FStructureEditorUtils::GetVarDescPtr(Struct);
		Root->SetNumberField(TEXT("field_count"), Desc ? Desc->Num() : 0);

		const UPackage* Package = Struct->GetOutermost();
		Root->SetBoolField(TEXT("package_dirtied"), Package ? Package->IsDirty() : false);

		AddDependents(Struct, Root);

		bool bSkipSave = false;
		if (Params.IsValid()) { Params->TryGetBoolField(TEXT("skip_save"), bSkipSave); }
		Root->SetBoolField(TEXT("saved"), bSkipSave ? false : UEditorAssetLibrary::SaveLoadedAsset(Struct, false));
	}
}

// ============================================================
//  get_struct_fields
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleGetStructFields(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStructFieldDetail;

	FString AssetPath, Error;
	TArray<FStructVariableDescription>* Desc = nullptr;
	UUserDefinedStruct* Struct = LoadStruct(Params, AssetPath, Desc, Error);
	if (!Struct) { return FMonolithActionResult::Error(Error); }

	TArray<TSharedPtr<FJsonValue>> Fields;
	FString PlaceholderName;
	for (const FStructVariableDescription& D : *Desc)
	{
		Fields.Add(MakeShared<FJsonValueObject>(DescribeField(D)));
		const FString Name = D.FriendlyName.IsEmpty() ? D.VarName.ToString() : D.FriendlyName;
		if (PlaceholderName.IsEmpty() && IsUnnamedPlaceholder(Name))
		{
			PlaceholderName = Name;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("struct_name"), Struct->GetName());
	Root->SetNumberField(TEXT("field_count"), Fields.Num());
	Root->SetArrayField(TEXT("fields"), Fields);
	if (!PlaceholderName.IsEmpty())
	{
		// Called out at the top level because it changes how this dump can be
		// replayed: every fresh struct carries one of these, so a caller
		// reproducing this schema elsewhere hits it on the first write.
		Root->SetStringField(TEXT("placeholder_field"), PlaceholderName);
		Root->SetStringField(TEXT("placeholder_note"), FString::Printf(
			TEXT("%s has never been named, so it is the member the engine seeds every new struct with (a User Defined Struct can never be empty). When reproducing this schema in another struct, configure that struct's own placeholder with set_struct_field_type / rename_struct_field instead of adding over it, or add the other fields first and then remove_struct_field it."),
			*PlaceholderName));
	}
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  add_struct_field
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleAddStructField(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStructFieldDetail;

	FString AssetPath, Error;
	TArray<FStructVariableDescription>* Desc = nullptr;
	UUserDefinedStruct* Struct = LoadStruct(Params, AssetPath, Desc, Error);
	if (!Struct) { return FMonolithActionResult::Error(Error); }

	const FString FieldName = Params->GetStringField(TEXT("name"));
	const FString TypeStr   = Params->GetStringField(TEXT("type"));
	if (FieldName.IsEmpty() || TypeStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Both name and type are required"));
	}
	if (const FStructVariableDescription* Existing = FindField(*Desc, FieldName))
	{
		// The collision a caller is most likely to hit first, and the one the
		// generic message gives no way out of: they are adding over the
		// never-named member the engine seeds every struct with, which cannot be
		// deleted first because a struct can never be empty.
		if (IsUnnamedPlaceholder(Existing->FriendlyName))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%s on %s is the placeholder member the engine seeds every new struct with, not a field anyone authored -- and it cannot be removed first, because a User Defined Struct can never be empty. Configure it instead of adding over it: set_struct_field_type to give it the type you want, then rename_struct_field to name it. Alternatively add your other fields first and then remove_struct_field it, which only becomes legal once the struct has more than one member."),
				*FieldName, *Struct->GetName()));
		}

		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Field %s already exists on %s. Use rename_struct_field or set_struct_field_type to change it."),
			*FieldName, *Struct->GetName()));
	}

	// Resolve the anchor BEFORE mutating: AddVariable reallocates the description
	// array, so an element pointer taken now would dangle. Copy the GUID instead.
	FGuid AnchorGuid;
	FString AfterName;
	if (Params->TryGetStringField(TEXT("after"), AfterName) && !AfterName.IsEmpty())
	{
		const FStructVariableDescription* Anchor = FindField(*Desc, AfterName);
		if (!Anchor)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("after names a field that does not exist: %s. Available: %s"),
				*AfterName, *DescribeAvailableFields(*Desc)));
		}
		AnchorGuid = Anchor->VarGuid;
	}

	FEdGraphPinType PinType;
	if (!ParseFieldType(TypeStr, PinType, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!FStructureEditorUtils::AddVariable(Struct, PinType))
	{
		// The token parsed, so this is the engine refusing the type as a struct
		// member (CanHaveAMemberVariableOfType) -- e.g. the struct itself, or a
		// set/map key that cannot be hashed.
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("The engine refused type '%s' as a member of %s. A struct cannot contain itself, and set/map keys must be hashable. The engine logs its reason to LogBlueprint."),
			*TypeStr, *Struct->GetName()));
	}

	// AddVariable appends, so the new member is the last description.
	if (Desc->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("AddVariable reported success but the struct has no fields"));
	}
	const FGuid NewGuid = Desc->Last().VarGuid;

	if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, FieldName))
	{
		// Roll the half-made field back rather than leaving a MemberVar_N behind.
		// The struct held at least one field before AddVariable, so the
		// cannot-be-empty refusal cannot bite here.
		FStructureEditorUtils::RemoveVariable(Struct, NewGuid);
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not name the new field %s. Names must be non-empty and unique within the struct (they compare case-insensitively). The partially-created field was removed."),
			*FieldName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	FString DefaultValue;
	if (Params->TryGetStringField(TEXT("default_value"), DefaultValue) && !DefaultValue.IsEmpty())
	{
		// The engine validates the string against the field type and refuses it
		// outright if it does not parse. Report which happened rather than
		// implying the value took.
		const bool bDefaultApplied = FStructureEditorUtils::ChangeVariableDefaultValue(Struct, NewGuid, DefaultValue);
		Root->SetBoolField(TEXT("default_value_applied"), bDefaultApplied);
		if (!bDefaultApplied)
		{
			Root->SetStringField(TEXT("default_value_note"), FString::Printf(
				TEXT("The engine rejected '%s' as a default for type %s; the field keeps the type's own default."),
				*DefaultValue, *TypeStr));
		}
	}

	if (AnchorGuid.IsValid())
	{
		FStructureEditorUtils::MoveVariable(
			Struct, NewGuid, AnchorGuid, FStructureEditorUtils::EMovePosition::PositionBelow);

		// MoveVariable's return value is NOT "is it where you asked": it reports
		// false when no move was needed (ComputeIndicesForMove bails on
		// InitialIndex == NewIndex), which is exactly the case where the anchor was
		// already the last field and AddVariable appended right after it. Read the
		// placement out of the array instead. Reported rather than fatal -- the
		// field exists either way, just possibly not where asked.
		const int32 AnchorIdx = Desc->IndexOfByPredicate(
			[&AnchorGuid](const FStructVariableDescription& D) { return D.VarGuid == AnchorGuid; });
		const int32 NewIdx = Desc->IndexOfByPredicate(
			[&NewGuid](const FStructVariableDescription& D) { return D.VarGuid == NewGuid; });
		Root->SetBoolField(TEXT("positioned_after"),
			AnchorIdx != INDEX_NONE && NewIdx == AnchorIdx + 1);
		Root->SetStringField(TEXT("after"), AfterName);
	}

	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("field"), FieldName);
	Root->SetStringField(TEXT("type"), TypeStr);
	Root->SetStringField(TEXT("guid"), NewGuid.ToString());
	CommitStruct(Struct, Params, Root);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_struct_field
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleRemoveStructField(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStructFieldDetail;

	FString AssetPath, Error;
	TArray<FStructVariableDescription>* Desc = nullptr;
	UUserDefinedStruct* Struct = LoadStruct(Params, AssetPath, Desc, Error);
	if (!Struct) { return FMonolithActionResult::Error(Error); }

	const FString FieldName = Params->GetStringField(TEXT("name"));
	if (FieldName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	const FStructVariableDescription* Target = FindField(*Desc, FieldName);
	if (!Target)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No field named %s on %s. Available: %s"),
			*FieldName, *Struct->GetName(), *DescribeAvailableFields(*Desc)));
	}
	const FGuid TargetGuid = Target->VarGuid;

	// RemoveVariable returns false for BOTH not-found and would-empty-the-struct
	// (bAllowToMakeEmpty is hardcoded false) and distinguishes the two only in a
	// log line. The field was just resolved above, so checking the count here is
	// what turns the second case into an answerable error rather than a bare
	// failure.
	if (Desc->Num() <= 1)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot remove %s: a User Defined Struct cannot be left empty. Add a replacement field first, or delete the struct asset."),
			*FieldName));
	}

	if (!FStructureEditorUtils::RemoveVariable(Struct, TargetGuid))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("RemoveVariable failed for %s"), *FieldName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("removed_field"), FieldName);
	Root->SetStringField(TEXT("guid"), TargetGuid.ToString());
	Root->SetStringField(TEXT("data_loss_note"),
		TEXT("The member and its pins are gone from every Blueprint that breaks this struct once those Blueprints recompile."));
	CommitStruct(Struct, Params, Root);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  rename_struct_field
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleRenameStructField(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStructFieldDetail;

	FString AssetPath, Error;
	TArray<FStructVariableDescription>* Desc = nullptr;
	UUserDefinedStruct* Struct = LoadStruct(Params, AssetPath, Desc, Error);
	if (!Struct) { return FMonolithActionResult::Error(Error); }

	const FString FieldName = Params->GetStringField(TEXT("name"));
	const FString NewName   = Params->GetStringField(TEXT("new_name"));
	if (FieldName.IsEmpty() || NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Both name and new_name are required"));
	}

	const FStructVariableDescription* Target = FindField(*Desc, FieldName);
	if (!Target)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No field named %s on %s. Available: %s"),
			*FieldName, *Struct->GetName(), *DescribeAvailableFields(*Desc)));
	}
	const FGuid TargetGuid     = Target->VarGuid;
	const FString PreviousName = Target->FriendlyName;

	// A collision would otherwise produce two fields that look identical to a
	// caller while remaining distinct by GUID. IsUniqueVariableFriendlyName
	// compares with FString::operator==, which is case-INSENSITIVE, so a
	// case-only rename lands here too and the engine would refuse it.
	if (const FStructVariableDescription* Clash = FindField(*Desc, NewName))
	{
		if (Clash->VarGuid != TargetGuid)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Field %s already exists on %s"), *NewName, *Struct->GetName()));
		}
		if (PreviousName.Equals(NewName, ESearchCase::CaseSensitive))
		{
			TSharedPtr<FJsonObject> NoOp = MakeShared<FJsonObject>();
			NoOp->SetStringField(TEXT("asset_path"), AssetPath);
			NoOp->SetStringField(TEXT("field"), NewName);
			NoOp->SetStringField(TEXT("guid"), TargetGuid.ToString());
			NoOp->SetBoolField(TEXT("changed"), false);
			NoOp->SetStringField(TEXT("note"), TEXT("The field is already named that; nothing was changed."));
			NoOp->SetBoolField(TEXT("success"), true);
			return FMonolithActionResult::Success(NoOp);
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot rename %s to %s: struct field names compare case-insensitively, so the engine treats this as a name that is already taken. Rename through an intermediate name if the casing matters."),
			*PreviousName, *NewName));
	}

	if (!FStructureEditorUtils::RenameVariable(Struct, TargetGuid, NewName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("RenameVariable failed for %s -> %s"), *FieldName, *NewName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("field"), NewName);
	Root->SetStringField(TEXT("previous_name"), PreviousName);
	Root->SetBoolField(TEXT("changed"), true);
	// The GUID is what Break/Make pins bind to, so it surviving is the reason a
	// rename does not disconnect anything.
	Root->SetStringField(TEXT("guid"), TargetGuid.ToString());
	CommitStruct(Struct, Params, Root);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_struct_field_type
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleSetStructFieldType(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStructFieldDetail;

	FString AssetPath, Error;
	TArray<FStructVariableDescription>* Desc = nullptr;
	UUserDefinedStruct* Struct = LoadStruct(Params, AssetPath, Desc, Error);
	if (!Struct) { return FMonolithActionResult::Error(Error); }

	const FString FieldName = Params->GetStringField(TEXT("name"));
	const FString TypeStr   = Params->GetStringField(TEXT("type"));
	if (FieldName.IsEmpty() || TypeStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Both name and type are required"));
	}

	const FStructVariableDescription* Target = FindField(*Desc, FieldName);
	if (!Target)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No field named %s on %s. Available: %s"),
			*FieldName, *Struct->GetName(), *DescribeAvailableFields(*Desc)));
	}

	const FGuid TargetGuid        = Target->VarGuid;
	const FEdGraphPinType OldPin  = Target->ToPinType();
	const FString PreviousType    = DescribePinType(OldPin);
	const FString PreviousDefault = Target->DefaultValue;

	FEdGraphPinType PinType;
	if (!ParseFieldType(TypeStr, PinType, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// ChangeVariableType returns false when the type is UNCHANGED, which is
	// indistinguishable at the call site from a refusal. Answering the no-op here
	// is what lets the failure below be reported as what it actually is.
	if (OldPin == PinType)
	{
		TSharedPtr<FJsonObject> NoOp = MakeShared<FJsonObject>();
		NoOp->SetStringField(TEXT("asset_path"), AssetPath);
		NoOp->SetStringField(TEXT("field"), FieldName);
		NoOp->SetStringField(TEXT("type"), PreviousType);
		NoOp->SetStringField(TEXT("guid"), TargetGuid.ToString());
		NoOp->SetBoolField(TEXT("changed"), false);
		NoOp->SetStringField(TEXT("note"), TEXT("The field already has that type; nothing was changed and no default value was cleared."));
		NoOp->SetBoolField(TEXT("success"), true);
		return FMonolithActionResult::Success(NoOp);
	}

	if (!FStructureEditorUtils::ChangeVariableType(Struct, TargetGuid, PinType))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("The engine refused type '%s' for %s on %s. A struct cannot contain itself, and set/map keys must be hashable. The engine logs its reason to LogBlueprint."),
			*TypeStr, *FieldName, *Struct->GetName()));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("field"), FieldName);
	Root->SetStringField(TEXT("type"), TypeStr);
	Root->SetStringField(TEXT("previous_type"), PreviousType);
	Root->SetStringField(TEXT("guid"), TargetGuid.ToString());
	Root->SetBoolField(TEXT("changed"), true);
	// ChangeVariableType assigns VarDesc->DefaultValue = FString() before setting
	// the new pin type -- the old default is gone, not converted.
	Root->SetBoolField(TEXT("default_value_cleared"), !PreviousDefault.IsEmpty());
	if (!PreviousDefault.IsEmpty())
	{
		Root->SetStringField(TEXT("previous_default_value"), PreviousDefault);
	}
	Root->SetStringField(TEXT("data_loss_note"),
		TEXT("This is a migration, not a rename: the default value is cleared, and pins of the old type on existing Break/Make nodes are disconnected when the dependent Blueprints recompile."));
	CommitStruct(Struct, Params, Root);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}
