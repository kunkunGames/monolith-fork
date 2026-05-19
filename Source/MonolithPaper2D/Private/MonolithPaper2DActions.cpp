#include "MonolithPaper2DActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

namespace MonolithPaper2D
{
	struct FTagRow
	{
		FString Name;
		FString Value;
		bool bValueTruncated = false;
	};

	static int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	static int32 ClampTagLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 0, 200);
	}

	static bool IsProjectAssetPath(const FString& AssetPath)
	{
		return AssetPath == TEXT("/Game") || AssetPath.StartsWith(TEXT("/Game/"));
	}

	static bool IsPaper2DAssetClass(const FAssetData& AssetData)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		return ClassName == TEXT("PaperSprite")
			|| ClassName == TEXT("PaperFlipbook")
			|| ClassName == TEXT("PaperTileSet")
			|| ClassName == TEXT("PaperTileMap");
	}

	static TSharedPtr<FJsonObject> BuildPaper2DAssetRow(const FAssetData& AssetData)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		return Row;
	}

	static bool ResolvePaper2DAssetData(IAssetRegistry& AssetRegistry, const FString& RawAssetPath, FAssetData& OutAssetData, FString& OutError)
	{
		FString AssetPath = RawAssetPath;
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty() || !IsProjectAssetPath(AssetPath))
		{
			OutError = TEXT("asset_path must be under /Game");
			return false;
		}

		FAssetData DirectAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (DirectAsset.IsValid())
		{
			if (!IsPaper2DAssetClass(DirectAsset))
			{
				OutError = FString::Printf(TEXT("asset_path does not identify a Paper2D asset: %s"), *AssetPath);
				return false;
			}

			OutAssetData = DirectAsset;
			return true;
		}

		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName.LeftInline(DotIndex);
		}

		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets, true);
		for (const FAssetData& PackageAsset : PackageAssets)
		{
			if (IsPaper2DAssetClass(PackageAsset))
			{
				OutAssetData = PackageAsset;
				return true;
			}
		}

		OutError = PackageAssets.Num() > 0
			? FString::Printf(TEXT("asset_path does not identify a Paper2D asset: %s"), *AssetPath)
			: FString::Printf(TEXT("Paper2D asset not found: %s"), *AssetPath);
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildBoundedPaper2DTagRows(const FAssetData& AssetData, int32 TagLimit, int32& OutTagCount, bool& bOutTagsTruncated)
	{
		constexpr int32 MaxTagValueLength = 512;

		TArray<FTagRow> TagRows;
		AssetData.EnumerateTags([&TagRows](TPair<FName, FAssetTagValueRef> Pair)
		{
			FTagRow Row;
			Row.Name = Pair.Key.ToString();
			Row.Value = Pair.Value.AsString();
			if (Row.Value.Len() > MaxTagValueLength)
			{
				Row.Value.LeftInline(MaxTagValueLength);
				Row.bValueTruncated = true;
			}
			TagRows.Add(MoveTemp(Row));
		});

		TagRows.Sort([](const FTagRow& A, const FTagRow& B)
		{
			return A.Name < B.Name;
		});

		OutTagCount = TagRows.Num();
		const int32 ReturnedCount = FMath::Min(TagLimit, TagRows.Num());
		bOutTagsTruncated = TagRows.Num() > ReturnedCount;

		TArray<TSharedPtr<FJsonValue>> JsonRows;
		JsonRows.Reserve(ReturnedCount);
		for (int32 Index = 0; Index < ReturnedCount; ++Index)
		{
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), TagRows[Index].Name);
			Row->SetStringField(TEXT("value"), TagRows[Index].Value);
			Row->SetBoolField(TEXT("value_truncated"), TagRows[Index].bValueTruncated);
			JsonRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		return JsonRows;
	}

	static TSharedPtr<FJsonObject> MakeModuleStatus(const TCHAR* ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		auto Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("name"), ModuleName);
		Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
		Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
		return Status;
	}
}

void FMonolithPaper2DActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("paper2d"), TEXT("get_status"),
		TEXT("Report Paper2D plugin/module availability and the Monolith-native first milestone for texture-atlas adjacent Paper2D discovery. Read-only; no Paper2D hard dependency."),
		FMonolithActionHandler::CreateStatic(&FMonolithPaper2DActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("paper2d"), TEXT("list_assets"),
		TEXT("List Paper2D asset metadata under /Game using AssetRegistry only: PaperSprite, PaperFlipbook, PaperTileSet, and PaperTileMap. Does not load or mutate assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithPaper2DActions::ListAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content path to scan, under /Game. Default: /Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum returned rows, clamped to 1..500. Default: 100."))
			.Build());

	Registry.RegisterAction(TEXT("paper2d"), TEXT("get_asset"),
		TEXT("Inspect bounded AssetRegistry metadata for one Paper2D asset under /Game. Does not load Paper2D modules or mutate assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithPaper2DActions::GetAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Paper2D package or object path under /Game"))
			.Optional(TEXT("include_tags"), TEXT("boolean"), TEXT("Include bounded AssetRegistry tag rows. Default: true."))
			.Optional(TEXT("tag_limit"), TEXT("integer"), TEXT("Maximum tag rows to return, clamped to 0..200. Default: 50."))
			.Build());
}

FMonolithActionResult FMonolithPaper2DActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("namespace"), TEXT("paper2d"));
	ResultJson->SetStringField(TEXT("domain"), TEXT("paper2d_discovery"));
	ResultJson->SetStringField(TEXT("mode"), TEXT("read_only"));
	ResultJson->SetBoolField(TEXT("hard_dependency"), false);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(MonolithPaper2D::MakeModuleStatus(TEXT("Paper2D"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithPaper2D::MakeModuleStatus(TEXT("Paper2DEditor"))));
	ResultJson->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> AssetClasses;
	AssetClasses.Add(MakeShared<FJsonValueString>(TEXT("PaperSprite")));
	AssetClasses.Add(MakeShared<FJsonValueString>(TEXT("PaperFlipbook")));
	AssetClasses.Add(MakeShared<FJsonValueString>(TEXT("PaperTileSet")));
	AssetClasses.Add(MakeShared<FJsonValueString>(TEXT("PaperTileMap")));
	ResultJson->SetArrayField(TEXT("asset_classes"), AssetClasses);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.list_assets")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.get_asset")));
	ResultJson->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.get_sprite")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.get_flipbook")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.slice_sprite_sheet")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("paper2d.create_flipbook")));
	ResultJson->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("MonolithPaper2D owns the paper2d namespace so clients can route optional Paper2D workflows directly.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("No Paper2D classes are included or loaded; AssetRegistry metadata is used for discovery.")));
	ResultJson->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithPaper2DActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.TrimStartAndEndInline();
	while (PackagePath.Len() > 5 && PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath.LeftChopInline(1);
	}
	if (PackagePath != TEXT("/Game") && !PackagePath.StartsWith(TEXT("/Game/")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithPaper2D::ClampLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Paper2D"), TEXT("PaperSprite")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Paper2D"), TEXT("PaperFlipbook")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Paper2D"), TEXT("PaperTileSet")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Paper2D"), TEXT("PaperTileMap")));

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (const FAssetData& AssetData : Assets)
	{
		if (!MonolithPaper2D::IsPaper2DAssetClass(AssetData))
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

		Rows.Add(MakeShared<FJsonValueObject>(MonolithPaper2D::BuildPaper2DAssetRow(AssetData)));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("namespace"), TEXT("paper2d"));
	ResultJson->SetStringField(TEXT("domain"), TEXT("paper2d_discovery"));
	ResultJson->SetStringField(TEXT("package_path"), PackagePath);
	ResultJson->SetNumberField(TEXT("matched_count"), MatchedCount);
	ResultJson->SetNumberField(TEXT("returned_count"), Rows.Num());
	ResultJson->SetNumberField(TEXT("limit"), Limit);
	ResultJson->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	ResultJson->SetObjectField(TEXT("class_counts"), CountsJson);
	ResultJson->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithPaper2DActions::GetAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required and must be under /Game"));
	}
	AssetPath.TrimStartAndEndInline();

	bool bIncludeTags = true;
	Params->TryGetBoolField(TEXT("include_tags"), bIncludeTags);

	double TagLimitValue = 50.0;
	Params->TryGetNumberField(TEXT("tag_limit"), TagLimitValue);
	const int32 TagLimit = MonolithPaper2D::ClampTagLimit(TagLimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData;
	FString ErrorMessage;
	if (!MonolithPaper2D::ResolvePaper2DAssetData(AssetRegistry, AssetPath, AssetData, ErrorMessage))
	{
		return FMonolithActionResult::Error(ErrorMessage);
	}

	int32 TagCount = 0;
	bool bTagsTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Tags;
	if (bIncludeTags)
	{
		Tags = MonolithPaper2D::BuildBoundedPaper2DTagRows(AssetData, TagLimit, TagCount, bTagsTruncated);
	}
	else
	{
		bool bIgnoredTruncated = false;
		MonolithPaper2D::BuildBoundedPaper2DTagRows(AssetData, 0, TagCount, bIgnoredTruncated);
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("namespace"), TEXT("paper2d"));
	ResultJson->SetStringField(TEXT("domain"), TEXT("paper2d_discovery"));
	ResultJson->SetStringField(TEXT("requested_path"), AssetPath);
	ResultJson->SetObjectField(TEXT("asset"), MonolithPaper2D::BuildPaper2DAssetRow(AssetData));
	ResultJson->SetBoolField(TEXT("tags_included"), bIncludeTags);
	ResultJson->SetNumberField(TEXT("tag_count"), TagCount);
	ResultJson->SetNumberField(TEXT("returned_tag_count"), Tags.Num());
	ResultJson->SetNumberField(TEXT("tag_limit"), TagLimit);
	ResultJson->SetBoolField(TEXT("tags_truncated"), bIncludeTags && bTagsTruncated);
	if (bIncludeTags)
	{
		ResultJson->SetArrayField(TEXT("tags"), Tags);
	}
	return FMonolithActionResult::Success(ResultJson);
}
