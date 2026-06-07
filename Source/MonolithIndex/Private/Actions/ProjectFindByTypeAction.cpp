#include "Actions/ProjectFindByTypeAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectFindByTypeAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetClass;
	// Try asset_type first if present, but don't reject an empty value when the
	// legacy asset_class alias can still provide a value. Only enforce
	// type-correctness (must be a string when supplied).
	if (Params->HasField(TEXT("asset_type")))
	{
		if (!Params->TryGetStringField(TEXT("asset_type"), AssetClass))
		{
			return FMonolithActionResult::Error(TEXT("'asset_type' parameter must be a string"), -32602);
		}
		if (AssetClass.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'asset_type' parameter cannot be empty"), -32602);
		}
	}
	if (AssetClass.IsEmpty() && Params->HasField(TEXT("asset_class")))
	{
		if (!Params->TryGetStringField(TEXT("asset_class"), AssetClass))
		{
			return FMonolithActionResult::Error(TEXT("'asset_class' parameter must be a string"), -32602);
		}
		if (AssetClass.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'asset_class' parameter cannot be empty"), -32602);
		}
	}
	if (AssetClass.IsEmpty() && Params->HasField(TEXT("type")))
	{
		if (!Params->TryGetStringField(TEXT("type"), AssetClass))
		{
			return FMonolithActionResult::Error(TEXT("'type' parameter must be a string"), -32602);
		}
		if (AssetClass.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'type' parameter cannot be empty"), -32602);
		}
	}

	if (AssetClass.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_type' (or 'asset_class'/'type') parameter is required"), -32602);
	}

	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		double OffsetValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("offset"), OffsetValue))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be a number"), -32602);
		}
		Offset = static_cast<int32>(OffsetValue);
	}
	Offset = FMath::Max(0, Offset);

	FString ModuleFilter;
	if (Params->HasField(TEXT("module")) && !Params->TryGetStringField(TEXT("module"), ModuleFilter))
	{
		return FMonolithActionResult::Error(TEXT("'module' parameter must be a string"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	TArray<FIndexedAsset> Assets = Subsystem->FindByType(AssetClass, Limit, Offset);

	if (!ModuleFilter.IsEmpty())
	{
		Assets.RemoveAll([&ModuleFilter](const FIndexedAsset& A) { return A.ModuleName != ModuleFilter; });
		if (Assets.Num() > Limit)
		{
			Assets.SetNum(Limit);
		}
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetsArr;
	AssetsArr.Reserve(Assets.Num());
	for (const FIndexedAsset& Asset : Assets)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package_path"), Asset.PackagePath);
		Entry->SetStringField(TEXT("asset_name"), Asset.AssetName);
		Entry->SetStringField(TEXT("asset_class"), Asset.AssetClass);
		Entry->SetStringField(TEXT("module_name"), Asset.ModuleName);
		Entry->SetNumberField(TEXT("file_size_bytes"), Asset.FileSizeBytes);
		Entry->SetStringField(TEXT("indexed_at"), Asset.IndexedAt);
		AssetsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("assets"), AssetsArr);
	Result->SetNumberField(TEXT("count"), Assets.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("limit"), Limit);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectFindByTypeAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_type"), TEXT("string"),
			TEXT("Asset class name (e.g. Blueprint, Material, StaticMesh, Texture2D). Aliases: asset_class, type."),
			{ TEXT("asset_class"), TEXT("type") })
		.Optional(TEXT("module"), TEXT("string"), TEXT("Filter by plugin/module name (e.g. ExampleInventory)"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results"), TEXT("100"))
		.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset"), TEXT("0"))
		.Build();
}
