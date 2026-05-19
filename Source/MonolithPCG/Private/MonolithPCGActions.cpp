#include "MonolithPCGActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithPCG
{
	TArray<FString> GetPcgModuleNames()
	{
		return {
			TEXT("PCG"),
			TEXT("PCGEditor"),
			TEXT("PCGCompute"),
			TEXT("PCGGeometryScriptInterop"),
			TEXT("PCGWaterInterop"),
			TEXT("PCGExternalDataInterop"),
			TEXT("PCGPythonInteropEditor")
		};
	}

	TSharedPtr<FJsonObject> BuildModuleStatusRow(const FString& ModuleName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("module"), ModuleName);
		Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(*ModuleName));
		Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(*ModuleName));
		return Row;
	}

	TSharedPtr<FJsonObject> BuildReflectedTypeRow(const TCHAR* ObjectPath)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetBoolField(TEXT("loaded"), FindObject<UObject>(nullptr, ObjectPath) != nullptr);
		return Row;
	}

	int32 ClampLimit(double Limit)
	{
		return FMath::Clamp(static_cast<int32>(Limit), 1, 500);
	}

	int32 ClampTagLimit(double Limit)
	{
		return FMath::Clamp(static_cast<int32>(Limit), 0, 200);
	}

	bool IsProjectAssetPath(const FString& AssetPath)
	{
		return AssetPath.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
			|| AssetPath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase);
	}

	bool IsPcgGraphLikeAsset(const FAssetData& Asset)
	{
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		return ClassName.Equals(TEXT("PCGGraph"), ESearchCase::IgnoreCase)
			|| ClassName.Equals(TEXT("PCGGraphInstance"), ESearchCase::IgnoreCase)
			|| ClassName.Equals(TEXT("ProceduralVegetationGraph"), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("PCGGraph"), ESearchCase::IgnoreCase);
	}

	bool IsPcgLikeComponent(const UActorComponent* Component)
	{
		if (!Component || !Component->GetClass())
		{
			return false;
		}

		const FString ClassName = Component->GetClass()->GetName();
		const FString ClassPath = Component->GetClass()->GetClassPathName().ToString();
		return ClassName.Equals(TEXT("PCGComponent"), ESearchCase::IgnoreCase)
			|| ClassPath.Contains(TEXT("/Script/PCG."), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
	}

	bool ResolvePcgGraphAssetData(const FString& RawAssetPath, IAssetRegistry& AssetRegistry, FAssetData& OutAssetData, FString& OutError)
	{
		FString AssetPath = RawAssetPath;
		AssetPath.TrimStartAndEndInline();

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("asset_path is required");
			return false;
		}
		if (!IsProjectAssetPath(AssetPath))
		{
			OutError = TEXT("asset_path must be under /Game");
			return false;
		}

		OutAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!OutAssetData.IsValid())
		{
			FString PackageName = AssetPath;
			int32 DotIndex = INDEX_NONE;
			if (PackageName.FindChar(TEXT('.'), DotIndex))
			{
				PackageName = PackageName.Left(DotIndex);
			}

			TArray<FAssetData> PackageAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets, true);
			for (const FAssetData& Candidate : PackageAssets)
			{
				if (IsPcgGraphLikeAsset(Candidate))
				{
					OutAssetData = Candidate;
					break;
				}
			}
		}

		if (!OutAssetData.IsValid())
		{
			OutError = FString::Printf(TEXT("PCG graph-like asset not found: %s"), *AssetPath);
			return false;
		}
		if (!IsPcgGraphLikeAsset(OutAssetData))
		{
			OutError = FString::Printf(TEXT("asset_path does not identify a PCG graph-like asset: %s"), *AssetPath);
			return false;
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> BuildBoundedAssetTagRows(const FAssetData& Asset, int32 TagLimit, int32& OutTagCount)
	{
		TArray<TPair<FString, FString>> Pairs;
		Asset.TagsAndValues.ForEach([&Pairs](TPair<FName, FAssetTagValueRef> TagPair)
		{
			Pairs.Emplace(TagPair.Key.ToString(), TagPair.Value.GetValue());
		});
		Pairs.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
		{
			return A.Key < B.Key;
		});

		OutTagCount = Pairs.Num();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < Pairs.Num() && Rows.Num() < TagLimit; ++Index)
		{
			const TPair<FString, FString>& Pair = Pairs[Index];
			FString Value = Pair.Value;
			const bool bValueTruncated = Value.Len() > 512;
			if (bValueTruncated)
			{
				Value = Value.Left(512);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Pair.Key);
			Row->SetStringField(TEXT("value"), Value);
			Row->SetBoolField(TEXT("value_truncated"), bValueTruncated);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	TSharedPtr<FJsonObject> BuildPcgGraphAssetRow(const FAssetData& Asset, bool bIncludeTags, int32 TagLimit)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), Asset.IsAssetLoaded());
		Row->SetStringField(TEXT("source"), TEXT("asset_registry"));
		Row->SetBoolField(TEXT("read_only"), true);

		if (bIncludeTags)
		{
			int32 TagCount = 0;
			TArray<TSharedPtr<FJsonValue>> Tags = BuildBoundedAssetTagRows(Asset, TagLimit, TagCount);
			Row->SetNumberField(TEXT("tag_count"), TagCount);
			Row->SetNumberField(TEXT("tag_limit"), TagLimit);
			Row->SetBoolField(TEXT("tags_truncated"), TagCount > Tags.Num());
			Row->SetArrayField(TEXT("tags"), Tags);
		}
		return Row;
	}
}

void FMonolithPCGActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("pcg"), TEXT("get_status"),
		TEXT("Report optional PCG module/type availability without loading PCG or mutating the level"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_graph_assets"),
		TEXT("List PCG graph-like assets using AssetRegistry class paths without hard PCG dependencies"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListGraphAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path to scan (must be under /Game)"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("get_graph_asset"),
		TEXT("Inspect bounded AssetRegistry metadata for one PCG graph-like asset without loading PCG or mutating packages"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::GetGraphAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph-like package or object path under /Game"))
			.Optional(TEXT("include_tags"), TEXT("boolean"), TEXT("Include bounded AssetRegistry tag rows"), TEXT("true"))
			.Optional(TEXT("tag_limit"), TEXT("integer"), TEXT("Maximum tag rows to return (0-200)"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_components"),
		TEXT("List PCG-like components in the current editor world using reflected class names"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListComponents),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());
}

FMonolithActionResult FMonolithPCGActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("status"), TEXT("read_only_capability_probe"));
	Result->SetStringField(TEXT("sample_utc"), FDateTime::UtcNow().ToIso8601());
	Result->SetBoolField(TEXT("pcg_namespace_registered"), FMonolithToolRegistry::Get().HasNamespace(TEXT("pcg")));

	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	bool bAnyModuleExists = false;
	bool bAnyModuleLoaded = false;
	for (const FString& ModuleName : MonolithPCG::GetPcgModuleNames())
	{
		TSharedPtr<FJsonObject> Row = MonolithPCG::BuildModuleStatusRow(ModuleName);
		bAnyModuleExists |= Row->GetBoolField(TEXT("exists"));
		bAnyModuleLoaded |= Row->GetBoolField(TEXT("loaded"));
		ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("modules"), ModuleRows);
	Result->SetBoolField(TEXT("available"), bAnyModuleExists);
	Result->SetBoolField(TEXT("loaded"), bAnyModuleLoaded);

	TArray<TSharedPtr<FJsonValue>> ReflectedTypes;
	const TCHAR* TypePaths[] =
	{
		TEXT("/Script/PCG.PCGGraph"),
		TEXT("/Script/PCG.PCGGraphInstance"),
		TEXT("/Script/PCG.PCGComponent"),
		TEXT("/Script/PCG.PCGSettings"),
		TEXT("/Script/PCG.PCGVolume")
	};
	for (const TCHAR* TypePath : TypePaths)
	{
		ReflectedTypes.Add(MakeShared<FJsonValueObject>(MonolithPCG::BuildReflectedTypeRow(TypePath)));
	}
	Result->SetArrayField(TEXT("reflected_types"), ReflectedTypes);

	TArray<TSharedPtr<FJsonValue>> CurrentActions;
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.get_status")));
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.list_graph_assets")));
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.get_graph_asset")));
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.list_components")));
	Result->SetArrayField(TEXT("current_actions"), CurrentActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.get_capabilities")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.list_pcg_node_types")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.validate_pcg_graph")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.execute_pcg")));
	Result->SetArrayField(TEXT("future_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("MonolithPCG owns the pcg namespace while keeping this first milestone AssetRegistry/reflection-only.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Graph mutation, execution, and node parameter editing remain future work.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ListGraphAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.TrimStartAndEndInline();
	while (PackagePath.Len() > 5 && PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath.LeftChopInline(1);
	}
	if (!MonolithPCG::IsProjectAssetPath(PackagePath))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithPCG::ClampLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (!MonolithPCG::IsPcgGraphLikeAsset(Asset))
		{
			continue;
		}

		MatchedCount++;
		if (Rows.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), Asset.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_asset_registry"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("graphs"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::GetGraphAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);

	bool bIncludeTags = true;
	Params->TryGetBoolField(TEXT("include_tags"), bIncludeTags);

	double TagLimitValue = 50.0;
	Params->TryGetNumberField(TEXT("tag_limit"), TagLimitValue);
	const int32 TagLimit = MonolithPCG::ClampTagLimit(TagLimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData;
	FString Error;
	if (!MonolithPCG::ResolvePcgGraphAssetData(AssetPath, AssetRegistry, AssetData, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_asset_registry"));
	Result->SetStringField(TEXT("status"), TEXT("found"));
	Result->SetObjectField(TEXT("graph"), MonolithPCG::BuildPcgGraphAssetRow(AssetData, bIncludeTags, TagLimit));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ListComponents(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithPCG::ClampLimit(LimitValue);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
		Result->SetStringField(TEXT("domain"), TEXT("pcg_world_reflection"));
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetStringField(TEXT("reason"), TEXT("No editor world is available"));
		Result->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Result);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!Actor)
		{
			continue;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!MonolithPCG::IsPcgLikeComponent(Component))
			{
				continue;
			}

			MatchedCount++;
			if (Rows.Num() >= Limit)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
			Row->SetStringField(TEXT("actor_path"), Actor->GetPathName());
			Row->SetStringField(TEXT("component_name"), Component->GetName());
			Row->SetStringField(TEXT("component_path"), Component->GetPathName());
			Row->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
			Row->SetStringField(TEXT("component_class_path"), Component->GetClass()->GetClassPathName().ToString());
			Row->SetBoolField(TEXT("registered"), Component->IsRegistered());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_world_reflection"));
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("components"), Rows);
	return FMonolithActionResult::Success(Result);
}
