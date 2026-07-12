#include "MonolithNDisplayActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

namespace MonolithNDisplay
{
	int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	bool IsConfigAssetClass(const FAssetData& AssetData)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();
		return ClassPath.Contains(TEXT("DisplayCluster"))
			|| ClassName.Contains(TEXT("DisplayCluster"))
			|| ClassName.Contains(TEXT("nDisplay"))
			|| ClassName.Contains(TEXT("NDisplay"));
	}

	TSharedPtr<FJsonObject> MakeModuleStatus(const TCHAR* ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		auto Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("name"), ModuleName);
		Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
		Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
		return Status;
	}
}

void FMonolithNDisplayActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("ndisplay"), TEXT("get_status"),
		TEXT("Report read-only nDisplay/DisplayCluster config authoring support without hard DisplayCluster dependencies."),
		FMonolithActionHandler::CreateStatic(&FMonolithNDisplayActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("ndisplay"), TEXT("list_config_assets"),
		TEXT("List nDisplay/DisplayCluster config-like assets under /Game using AssetRegistry metadata only. Does not load, save, or mutate configs."),
		FMonolithActionHandler::CreateStatic(&FMonolithNDisplayActions::ListConfigAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content package path under /Game"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum config assets to return, clamped to 1..500"), TEXT("100"))
			.Range(TEXT("limit"), 1, 500)
			.Build());
}

FMonolithActionResult FMonolithNDisplayActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("ndisplay"));
	Result->SetStringField(TEXT("domain"), TEXT("config_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(MonolithNDisplay::MakeModuleStatus(TEXT("DisplayCluster"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithNDisplay::MakeModuleStatus(TEXT("DisplayClusterConfiguration"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithNDisplay::MakeModuleStatus(TEXT("DisplayClusterConfigurator"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithNDisplay::MakeModuleStatus(TEXT("DisplayClusterProjection"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithNDisplay::MakeModuleStatus(TEXT("DisplayClusterEditor"))));
	Result->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.list_config_assets")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.get_config")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.list_nodes")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.list_viewports")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.get_projection_policy")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("ndisplay.save_config")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This first milestone is read-only and uses module reflection plus AssetRegistry metadata only.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Config loading, node/viewport mutation, projection policy editing, and saving remain future work gated by DisplayCluster API compatibility and explicit confirmation.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithNDisplayActions::ListConfigAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithNDisplay::ClampLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (const FAssetData& AssetData : Assets)
	{
		if (!MonolithNDisplay::IsConfigAssetClass(AssetData))
		{
			continue;
		}

		MatchedCount++;
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassCounts.FindOrAdd(ClassName)++;

		if (Rows.Num() >= Limit)
		{
			continue;
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), ClassName);
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("ndisplay"));
	Result->SetStringField(TEXT("domain"), TEXT("config_discovery"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}
