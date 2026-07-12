#include "MonolithChaosFractureActions.h"

#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Modules/ModuleManager.h"

namespace MonolithChaosFracture
{
	TArray<FString> GetModuleNames()
	{
		return {
			TEXT("GeometryCollectionEngine"),
			TEXT("GeometryCollectionEditor"),
			TEXT("GeometryCollectionNodes"),
			TEXT("GeometryCollectionTracks"),
			TEXT("GeometryCollectionSequencer"),
			TEXT("GeometryCollectionDepNodes"),
			TEXT("FractureEngine"),
			TEXT("FractureEditor"),
			TEXT("ChaosSolverEngine"),
			TEXT("ChaosSolverEditor"),
			TEXT("FieldSystemEngine"),
			TEXT("FieldSystemEditor")
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

	bool IsGeometryCollectionLikeAsset(const FAssetData& Asset)
	{
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		return ClassName.Equals(TEXT("GeometryCollection"), ESearchCase::IgnoreCase)
			|| ClassName.Equals(TEXT("GeometryCollectionCache"), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("GeometryCollection"), ESearchCase::IgnoreCase);
	}

	bool IsGeometryCollectionLikeComponent(const UActorComponent* Component)
	{
		if (!Component || !Component->GetClass())
		{
			return false;
		}

		const FString ClassName = Component->GetClass()->GetName();
		const FString ClassPath = Component->GetClass()->GetClassPathName().ToString();
		return ClassName.Equals(TEXT("GeometryCollectionComponent"), ESearchCase::IgnoreCase)
			|| ClassPath.Contains(TEXT("/Script/GeometryCollectionEngine."), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("GeometryCollection"), ESearchCase::IgnoreCase);
	}
}

void FMonolithChaosFractureActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("chaos_fracture"), TEXT("get_status"),
		TEXT("Report optional Geometry Collection / Fracture module and reflected type availability without mutating assets"),
		FMonolithActionHandler::CreateStatic(&FMonolithChaosFractureActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("chaos_fracture"), TEXT("list_geometry_collection_assets"),
		TEXT("List Geometry Collection-like assets using AssetRegistry class paths without loading Fracture modules"),
		FMonolithActionHandler::CreateStatic(&FMonolithChaosFractureActions::ListGeometryCollectionAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path to scan (must be under /Game)"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Range(TEXT("limit"), 1, 500)
			.Build());

	Registry.RegisterAction(TEXT("chaos_fracture"), TEXT("list_geometry_collection_components"),
		TEXT("List Geometry Collection-like components in the current editor world using reflected class names"),
		FMonolithActionHandler::CreateStatic(&FMonolithChaosFractureActions::ListGeometryCollectionComponents),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Range(TEXT("limit"), 1, 500)
			.Build());
}

FMonolithActionResult FMonolithChaosFractureActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("chaos_fracture"));
	Result->SetStringField(TEXT("status"), TEXT("read_only_capability_probe"));
	Result->SetStringField(TEXT("sample_utc"), FDateTime::UtcNow().ToIso8601());
	Result->SetBoolField(TEXT("chaos_fracture_namespace_registered"), FMonolithToolRegistry::Get().HasNamespace(TEXT("chaos_fracture")));

	const TArray<FString> ModuleNames = MonolithChaosFracture::GetModuleNames();
	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	ModuleRows.Reserve(ModuleNames.Num());
	bool bAnyModuleExists = false;
	bool bAnyModuleLoaded = false;
	for (const FString& ModuleName : ModuleNames)
	{
		TSharedPtr<FJsonObject> Row = MonolithChaosFracture::BuildModuleStatusRow(ModuleName);
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
		TEXT("/Script/GeometryCollectionEngine.GeometryCollection"),
		TEXT("/Script/GeometryCollectionEngine.GeometryCollectionComponent"),
		TEXT("/Script/GeometryCollectionEngine.GeometryCollectionActor"),
		TEXT("/Script/GeometryCollectionEngine.GeometryCollectionCache"),
		TEXT("/Script/FractureEditor.FractureModeSettings")
	};
	ReflectedTypes.Reserve(UE_ARRAY_COUNT(TypePaths));
	for (const TCHAR* TypePath : TypePaths)
	{
		ReflectedTypes.Add(MakeShared<FJsonValueObject>(MonolithChaosFracture::BuildReflectedTypeRow(TypePath)));
	}
	Result->SetArrayField(TEXT("reflected_types"), ReflectedTypes);

	TArray<TSharedPtr<FJsonValue>> CurrentActions;
	CurrentActions.Reserve(3);
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.get_status")));
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.list_geometry_collection_assets")));
	CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.list_geometry_collection_components")));
	Result->SetArrayField(TEXT("current_actions"), CurrentActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Reserve(5);
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.get_fracture_info")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.validate_fracture_asset")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.fracture_uniform")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.fracture_voronoi")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("chaos_fracture.fracture_slice")));
	Result->SetArrayField(TEXT("future_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Reserve(2);
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This first milestone reports visibility only; it does not load Fracture modules, run fracture tools, or mutate Geometry Collections.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Destructive fracture recipes must remain future work with duplicate-target defaults, confirm=true, transactions, and operation limits.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChaosFractureActions::ListGeometryCollectionAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	if (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), LimitValue))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param 'limit': must be a number"));
	}
	const int32 Limit = MonolithChaosFracture::ClampLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(Assets.Num(), Limit));
	int32 MatchedCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (!MonolithChaosFracture::IsGeometryCollectionLikeAsset(Asset))
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
	Result->SetStringField(TEXT("namespace"), TEXT("chaos_fracture"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChaosFractureActions::ListGeometryCollectionComponents(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 100.0;
	if (Params->HasField(TEXT("limit")) && !Params->TryGetNumberField(TEXT("limit"), LimitValue))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param 'limit': must be a number"));
	}
	const int32 Limit = MonolithChaosFracture::ClampLimit(LimitValue);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("chaos_fracture"));
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetStringField(TEXT("reason"), TEXT("No editor world is available"));
		Result->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Result);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Limit);
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
			if (!MonolithChaosFracture::IsGeometryCollectionLikeComponent(Component))
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
	Result->SetStringField(TEXT("namespace"), TEXT("chaos_fracture"));
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("components"), Rows);
	return FMonolithActionResult::Success(Result);
}
