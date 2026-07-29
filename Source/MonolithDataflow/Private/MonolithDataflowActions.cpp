#include "MonolithDataflowActions.h"

#include "MonolithDataflowCommon.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dataflow/DataflowObject.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

namespace MonolithDataflow
{
	namespace
	{
		TSharedPtr<FJsonObject> MakeModuleStatus(const TCHAR* ModuleName)
		{
			FModuleManager& ModuleManager = FModuleManager::Get();
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("name"), ModuleName);
			Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
			Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
			return Status;
		}

		TSharedPtr<FJsonObject> MakeDataflowPluginStatus()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Dataflow"));
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("name"), TEXT("Dataflow"));
			Status->SetBoolField(TEXT("installed"), Plugin.IsValid());
			Status->SetBoolField(TEXT("enabled"), Plugin.IsValid() && Plugin->IsEnabled());
			if (Plugin.IsValid())
			{
				Status->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
			}
			return Status;
		}

		TArray<TSharedPtr<FJsonValue>> ImplementedActions()
		{
			const TArray<FString> Names =
			{
				TEXT("dataflow.get_status"),
				TEXT("dataflow.list_assets"),
				TEXT("dataflow.get_dataflow_graph"),
				TEXT("dataflow.list_dataflow_node_types"),
				TEXT("dataflow.get_dataflow_node_schema"),
				TEXT("dataflow.validate_dataflow_graph"),
				TEXT("dataflow.list_dataflow_variables"),
				TEXT("dataflow.list_dataflow_comments")
			};

			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(Names.Num());
			for (const FString& Name : Names)
			{
				Rows.Add(MakeShared<FJsonValueString>(Name));
			}
			return Rows;
		}

		TSharedPtr<FJsonObject> MakeAssetRow(
			const FAssetData& AssetData,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(
				TEXT("object_path"),
				TextBudget.Bound(AssetData.GetObjectPathString(), MaxPathChars));
			Row->SetStringField(
				TEXT("package_name"),
				TextBudget.Bound(AssetData.PackageName.ToString(), MaxPathChars));
			Row->SetStringField(
				TEXT("package_path"),
				TextBudget.Bound(AssetData.PackagePath.ToString(), MaxPathChars));
			Row->SetStringField(
				TEXT("asset_name"),
				TextBudget.Bound(AssetData.AssetName.ToString(), MaxNameChars));
			Row->SetStringField(
				TEXT("asset_class"),
				TextBudget.Bound(AssetData.AssetClassPath.GetAssetName().ToString(), MaxNameChars));
			Row->SetStringField(
				TEXT("asset_class_path"),
				TextBudget.Bound(AssetData.AssetClassPath.ToString(), MaxPathChars));
			Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
			return Row;
		}
	}
}

void FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("get_status"),
		TEXT("Report the exact read-only Dataflow inspection surface, engine-module state, and optional Dataflow plugin state."),
		FMonolithActionHandler::CreateStatic(&GetStatus),
		FParamSchemaBuilder().Build(),
		TEXT("Diagnostics"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("list_assets"),
		TEXT("Enumerate a bounded slice of exact UDataflow AssetRegistry rows below one canonical /Game package directory without loading assets."),
		FMonolithActionHandler::CreateStatic(&ListAssets),
		FParamSchemaBuilder()
			.Optional(
				TEXT("package_path"),
				TEXT("string"),
				TEXT("Canonical package directory: /Game or below /Game/."),
				TEXT("/Game"))
			.Optional(
				TEXT("limit"),
				TEXT("integer"),
				TEXT("Maximum asset rows; invalid values are rejected."),
				TEXT("100"))
			.Range(TEXT("limit"), 1, MonolithDataflow::MaxAssetRows)
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("get_dataflow_graph"),
		TEXT("Read a bounded UDataflow graph snapshot with explicit nested truncation and package-dirty postconditions."),
		FMonolithActionHandler::CreateStatic(&GetDataflowGraph),
		FParamSchemaBuilder()
			.Required(
				TEXT("asset_path"),
				TEXT("string"),
				TEXT("Exact canonical UDataflow object path below /Game/."))
			.Optional(
				TEXT("node_limit"),
				TEXT("integer"),
				TEXT("Maximum node rows; invalid values are rejected."),
				TEXT("128"))
			.Range(TEXT("node_limit"), 1, MonolithDataflow::MaxGraphNodes)
			.Optional(
				TEXT("connection_limit"),
				TEXT("integer"),
				TEXT("Maximum connection rows; invalid values are rejected."),
				TEXT("1000"))
			.Range(TEXT("connection_limit"), 1, MonolithDataflow::MaxGraphConnections)
			.Optional(
				TEXT("pin_limit"),
				TEXT("integer"),
				TEXT("Maximum input and output pin rows per node."),
				TEXT("128"))
			.Range(TEXT("pin_limit"), 1, MonolithDataflow::MaxPinsPerOwner)
			.Optional(
				TEXT("property_limit"),
				TEXT("integer"),
				TEXT("Maximum editable property rows per node."),
				TEXT("128"))
			.Range(TEXT("property_limit"), 1, MonolithDataflow::MaxPropertiesPerOwner)
			.Optional(
				TEXT("include_properties"),
				TEXT("boolean"),
				TEXT("Include bounded editable property snapshots."),
				TEXT("false"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("list_dataflow_node_types"),
		TEXT("List registered Dataflow factory types with exact counts and bounded optional pin schemas."),
		FMonolithActionHandler::CreateStatic(&ListDataflowNodeTypes),
		FParamSchemaBuilder()
			.Optional(
				TEXT("filter"),
				TEXT("string"),
				TEXT("Substring filter across type, display name, category, and tags."))
			.Optional(
				TEXT("common_only"),
				TEXT("boolean"),
				TEXT("Exclude deprecated and experimental node types."),
				TEXT("true"))
			.Optional(
				TEXT("limit"),
				TEXT("integer"),
				TEXT("Maximum node-type rows; invalid values are rejected."),
				TEXT("200"))
			.Range(TEXT("limit"), 1, MonolithDataflow::MaxNodeTypes)
			.Optional(
				TEXT("include_pins"),
				TEXT("boolean"),
				TEXT("Include bounded default-node pin schemas."),
				TEXT("false"))
			.Optional(
				TEXT("pin_limit"),
				TEXT("integer"),
				TEXT("Maximum input and output pin rows per returned type."),
				TEXT("64"))
			.Range(TEXT("pin_limit"), 1, MonolithDataflow::MaxPinsPerOwner)
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("get_dataflow_node_schema"),
		TEXT("Read one case-exact registered Dataflow node type and bounded default pin/property schemas."),
		FMonolithActionHandler::CreateStatic(&GetDataflowNodeSchema),
		FParamSchemaBuilder()
			.Required(
				TEXT("type_name"),
				TEXT("string"),
				TEXT("Case-exact registered Dataflow factory type name."))
			.Optional(
				TEXT("include_properties"),
				TEXT("boolean"),
				TEXT("Include bounded editable default properties."),
				TEXT("true"))
			.Optional(
				TEXT("pin_limit"),
				TEXT("integer"),
				TEXT("Maximum input and output pin rows."),
				TEXT("256"))
			.Range(TEXT("pin_limit"), 1, MonolithDataflow::MaxPinsPerOwner)
			.Optional(
				TEXT("property_limit"),
				TEXT("integer"),
				TEXT("Maximum editable default property rows."),
				TEXT("256"))
			.Range(TEXT("property_limit"), 1, MonolithDataflow::MaxPropertiesPerOwner)
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("validate_dataflow_graph"),
		TEXT("Validate bounded node and connection slices without mutation and never claim validity for an incomplete scan."),
		FMonolithActionHandler::CreateStatic(&ValidateDataflowGraph),
		FParamSchemaBuilder()
			.Required(
				TEXT("asset_path"),
				TEXT("string"),
				TEXT("Exact canonical UDataflow object path below /Game/."))
			.Optional(
				TEXT("node_scan_limit"),
				TEXT("integer"),
				TEXT("Maximum graph node entries to validate."),
				TEXT("10000"))
			.Range(TEXT("node_scan_limit"), 1, MonolithDataflow::MaxNodeScan)
			.Optional(
				TEXT("connection_scan_limit"),
				TEXT("integer"),
				TEXT("Maximum graph connection entries to validate."),
				TEXT("50000"))
			.Range(TEXT("connection_scan_limit"), 1, MonolithDataflow::MaxConnectionScan)
			.Optional(
				TEXT("issue_limit"),
				TEXT("integer"),
				TEXT("Maximum issue rows to return while preserving the observed issue count."),
				TEXT("500"))
			.Range(TEXT("issue_limit"), 1, MonolithDataflow::MaxValidationIssues)
			.Build(),
		TEXT("Validation"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("list_dataflow_variables"),
		TEXT("List bounded UDataflow property-bag descriptors and bounded scalar values, explicitly omitting unbounded container or struct serialization."),
		FMonolithActionHandler::CreateStatic(&ListDataflowVariables),
		FParamSchemaBuilder()
			.Required(
				TEXT("asset_path"),
				TEXT("string"),
				TEXT("Exact canonical UDataflow object path below /Game/."))
			.Optional(
				TEXT("limit"),
				TEXT("integer"),
				TEXT("Maximum variable rows; invalid values are rejected."),
				TEXT("200"))
			.Range(TEXT("limit"), 1, MonolithDataflow::MaxVariables)
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(
		TEXT("dataflow"),
		TEXT("list_dataflow_comments"),
		TEXT("List bounded Dataflow comment boxes and bounded membership hints with an explicit comparison-work budget."),
		FMonolithActionHandler::CreateStatic(&ListDataflowComments),
		FParamSchemaBuilder()
			.Required(
				TEXT("asset_path"),
				TEXT("string"),
				TEXT("Exact canonical UDataflow object path below /Game/."))
			.Optional(
				TEXT("comment_limit"),
				TEXT("integer"),
				TEXT("Maximum comment rows."),
				TEXT("200"))
			.Range(TEXT("comment_limit"), 1, MonolithDataflow::MaxComments)
			.Optional(
				TEXT("node_limit"),
				TEXT("integer"),
				TEXT("Maximum contained-node rows per comment."),
				TEXT("128"))
			.Range(TEXT("node_limit"), 1, MonolithDataflow::MaxCommentNodes)
			.Optional(
				TEXT("graph_node_scan_limit"),
				TEXT("integer"),
				TEXT("Maximum editor graph entries considered for comments and membership."),
				TEXT("5000"))
			.Range(
				TEXT("graph_node_scan_limit"),
				1,
				MonolithDataflow::MaxCommentGraphNodeScan)
			.Build(),
		TEXT("Dataflow"));
}

FMonolithActionResult FMonolithDataflowActions::GetStatus(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FStrictParamReader Reader(Params);
	if (!Reader.RejectUnknown({}))
	{
		return InvalidParams(Reader.GetError());
	}

	TArray<TSharedPtr<FJsonValue>> Modules;
	for (const TCHAR* ModuleName :
		{
			TEXT("DataflowCore"),
			TEXT("DataflowEngine"),
			TEXT("DataflowEditor"),
			TEXT("DataflowEnginePlugin"),
			TEXT("DataflowNodes"),
			TEXT("GeometryCollectionEngine"),
			TEXT("ChaosCaching")
		})
	{
		Modules.Add(MakeShared<FJsonValueObject>(MakeModuleStatus(ModuleName)));
	}

	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetBoolField(TEXT("asset_registry_discovery"), true);
	Capabilities->SetBoolField(TEXT("graph_inspection"), true);
	Capabilities->SetBoolField(TEXT("node_type_inspection"), true);
	Capabilities->SetBoolField(TEXT("graph_validation"), true);
	Capabilities->SetBoolField(TEXT("variable_inspection"), true);
	Capabilities->SetBoolField(TEXT("comment_inspection"), true);
	Capabilities->SetBoolField(TEXT("authoring"), false);
	Capabilities->SetBoolField(TEXT("evaluation"), false);
	Capabilities->SetBoolField(TEXT("regeneration"), false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_inspection"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetNumberField(TEXT("action_count"), 8);
	Result->SetBoolField(TEXT("engine_dataflow_modules_linked"), true);
	Result->SetBoolField(TEXT("optional_plugin_module_loading_performed"), false);
	Result->SetObjectField(TEXT("dataflow_plugin"), MakeDataflowPluginStatus());
	Result->SetArrayField(TEXT("modules"), Modules);
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions());
	Result->SetObjectField(TEXT("capabilities"), Capabilities);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString PackagePath;
	int32 Limit = 100;
	FStrictParamReader Reader(Params);
	if (!Reader.OptionalString(TEXT("package_path"), PackagePath, TEXT("/Game"), MaxPathChars)
		|| !Reader.OptionalInt(TEXT("limit"), Limit, 100, 1, MaxAssetRows)
		|| !Reader.RejectUnknown({TEXT("package_path"), TEXT("limit")}))
	{
		return InvalidParams(Reader.GetError());
	}

	FString PackageError;
	if (!ValidateGamePackagePath(PackagePath, PackageError))
	{
		return InvalidParams(PackageError);
	}

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.ClassPaths.Add(UDataflow::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FOutputBudget TextBudget;
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Limit);
	bool bSawExtra = false;
	int32 ObservedMatchCount = 0;
	const bool bEnumerated = AssetRegistry.EnumerateAssets(
		Filter,
		[&](const FAssetData& AssetData)
		{
			++ObservedMatchCount;
			if (Rows.Num() >= Limit)
			{
				bSawExtra = true;
				return false;
			}
			if (!TextBudget.TryReserveRow())
			{
				bSawExtra = true;
				return false;
			}
			Rows.Add(MakeShared<FJsonValueObject>(MakeAssetRow(AssetData, TextBudget)));
			return true;
		});
	if (!bEnumerated)
	{
		return ErrorWithCode(
			TEXT("asset_registry_enumeration_failed"),
			FString::Printf(
				TEXT("AssetRegistry rejected the Dataflow filter for '%s'"),
				*PackagePath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_assets"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetStringField(TEXT("asset_class"), UDataflow::StaticClass()->GetClassPathName().ToString());
	Result->SetStringField(TEXT("ordering"), TEXT("asset_registry_enumeration"));
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("observed_match_count"), ObservedMatchCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), bSawExtra);
	Result->SetBoolField(TEXT("count_complete"), !bSawExtra);
	if (!bSawExtra)
	{
		Result->SetNumberField(TEXT("total_count"), ObservedMatchCount);
	}
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetBoolField(TEXT("assets_loaded_by_action"), false);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}
