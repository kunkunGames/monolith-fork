#include "MonolithDataflowActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

namespace MonolithDataflow
{
	int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	bool IsDataflowAssetClass(const FAssetData& AssetData)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();
		return ClassPath.Contains(TEXT("/Script/Dataflow"))
			|| ClassName.Contains(TEXT("Dataflow"));
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

void FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("dataflow"), TEXT("get_status"),
		TEXT("Report read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("dataflow"), TEXT("list_assets"),
		TEXT("List Dataflow asset metadata under /Game using AssetRegistry only. Does not load, evaluate, regenerate, or mutate assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ListAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content package path under /Game"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return, clamped to 1..500"), TEXT("100"))
			.Build());
}

FMonolithActionResult FMonolithDataflowActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowCore"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowEngine"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowEditor"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowNodes"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("GeometryCollectionEngine"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("ChaosCaching"))));
	Result->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_assets")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_dataflow_graph")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_node_types")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.can_connect_dataflow")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.evaluate_dataflow_terminal")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.regenerate_dataflow")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("MonolithDataflow owns the dataflow namespace; this first milestone remains AssetRegistry/module-status only.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("No Dataflow headers are included and no Dataflow assets are loaded; graph serialization, mutation, evaluation, and regeneration remain future optional dataflow work.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithDataflow::ClampLimit(LimitValue);

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
		if (!MonolithDataflow::IsDataflowAssetClass(AssetData))
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
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_discovery"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}
