#include "MonolithEditorSelectionActions.h"

#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "MonolithParamSchema.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"

namespace MonolithEditorSelection
{
static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Vector)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	Array.Add(MakeShared<FJsonValueNumber>(Vector.X));
	Array.Add(MakeShared<FJsonValueNumber>(Vector.Y));
	Array.Add(MakeShared<FJsonValueNumber>(Vector.Z));
	return Array;
}

static TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	const FRotator Rotation = Transform.Rotator();
	Obj->SetArrayField(TEXT("location"), VectorToJsonArray(Transform.GetLocation()));
	Obj->SetArrayField(TEXT("rotation"), VectorToJsonArray(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
	Obj->SetArrayField(TEXT("scale"), VectorToJsonArray(Transform.GetScale3D()));
	return Obj;
}

static FString NormalizeClassFilter(FString Filter)
{
	Filter.TrimStartAndEndInline();
	if (Filter.StartsWith(TEXT("class'")) && Filter.EndsWith(TEXT("'")))
	{
		Filter = Filter.Mid(6, Filter.Len() - 7);
	}
	return Filter;
}

static void GetSelectedAssetData(TArray<FAssetData>& OutAssets, TArray<FString>* OutFolders)
{
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	IContentBrowserSingleton& ContentBrowser = ContentBrowserModule.Get();

	ContentBrowser.GetSelectedAssets(OutAssets);
	if (OutFolders)
	{
		ContentBrowser.GetSelectedFolders(*OutFolders);
	}
}

static TArray<UObject*> GetAssetsForEditor(UAssetEditorSubsystem* AssetEditorSubsystem, IAssetEditorInstance* Editor)
{
	TArray<UObject*> Result;
	if (!AssetEditorSubsystem || !Editor)
	{
		return Result;
	}

	for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
	{
		if (!Asset)
		{
			continue;
		}

		const TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
		if (Editors.Contains(Editor))
		{
			Result.Add(Asset);
		}
	}

	return Result;
}
}

void FMonolithEditorSelectionActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("editor"), TEXT("get_selected_actors"),
		TEXT("Get stable metadata for actors selected in the Level Viewport or World Outliner"),
		FMonolithActionHandler::CreateStatic(&HandleGetSelectedActors),
		FParamSchemaBuilder()
			.Optional(TEXT("include_components"), TEXT("bool"), TEXT("Include selected actor component metadata"), TEXT("false"))
			.Optional(TEXT("filter_class"), TEXT("string"), TEXT("Actor class short name or full class path filter"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_selected_assets"),
		TEXT("Get FAssetData-derived metadata for assets selected in the Content Browser"),
		FMonolithActionHandler::CreateStatic(&HandleGetSelectedAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("filter_class"), TEXT("string"), TEXT("Asset class short name or full class path filter"))
			.Optional(TEXT("include_folders"), TEXT("bool"), TEXT("Include selected Content Browser folders separately"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_active_asset_editor"),
		TEXT("Get the active or unambiguous open asset editor with explicit fallback source"),
		FMonolithActionHandler::CreateStatic(&HandleGetActiveAssetEditor),
		FParamSchemaBuilder()
			.Optional(TEXT("fallback_to_selection"), TEXT("bool"), TEXT("Use a single selected Content Browser asset if editor focus is ambiguous"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleGetSelectedActors(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bIncludeComponents = false;
	FString FilterClass;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
		Params->TryGetStringField(TEXT("filter_class"), FilterClass);
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		return FMonolithActionResult::Error(TEXT("Selected actor set not available"));
	}

	TArray<TSharedPtr<FJsonValue>> ActorArray;
	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!Actor || !MatchesClassFilter(Actor->GetClass(), FilterClass))
		{
			continue;
		}

		ActorArray.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor, bIncludeComponents)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("selected_count"), ActorArray.Num());
	Result->SetArrayField(TEXT("actors"), ActorArray);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleGetSelectedAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterClass;
	bool bIncludeFolders = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("filter_class"), FilterClass);
		Params->TryGetBoolField(TEXT("include_folders"), bIncludeFolders);
	}

	TArray<FAssetData> SelectedAssets;
	TArray<FString> SelectedFolders;
	MonolithEditorSelection::GetSelectedAssetData(SelectedAssets, bIncludeFolders ? &SelectedFolders : nullptr);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (!AssetData.IsValid())
		{
			continue;
		}

		const bool bMatchesClass =
			MatchesClassFilter(AssetData.AssetClassPath, FilterClass)
			|| MatchesClassFilter(AssetData.GetClass(EResolveClass::No), FilterClass);

		if (!bMatchesClass)
		{
			continue;
		}

		AssetArray.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData, GetAssetVirtualPath(AssetData))));
	}

	TArray<TSharedPtr<FJsonValue>> FolderArray;
	if (bIncludeFolders)
	{
		for (const FString& Folder : SelectedFolders)
		{
			TSharedPtr<FJsonObject> FolderObj = MakeShared<FJsonObject>();
			FolderObj->SetStringField(TEXT("path"), Folder);
			FolderObj->SetStringField(TEXT("virtual_path"), Folder);
			FolderObj->SetBoolField(TEXT("is_asset"), false);
			FolderObj->SetBoolField(TEXT("is_folder"), true);
			FolderArray.Add(MakeShared<FJsonValueObject>(FolderObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("selected_count"), AssetArray.Num());
	Result->SetNumberField(TEXT("folder_count"), FolderArray.Num());
	Result->SetArrayField(TEXT("assets"), AssetArray);
	Result->SetArrayField(TEXT("folders"), FolderArray);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleGetActiveAssetEditor(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bFallbackToSelection = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("fallback_to_selection"), bFallbackToSelection);
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("AssetEditorSubsystem not available"));
	}

	const TArray<IAssetEditorInstance*> OpenEditors = AssetEditorSubsystem->GetAllOpenEditors();
	IAssetEditorInstance* ChosenEditor = nullptr;
	FString Source;

	if (OpenEditors.Num() == 1)
	{
		ChosenEditor = OpenEditors[0];
		Source = TEXT("single_open_editor");
	}
	else if (OpenEditors.Num() > 1)
	{
		double BestActivationTime = 0.0;
		bool bAmbiguousActivation = false;
		for (IAssetEditorInstance* Editor : OpenEditors)
		{
			if (!Editor)
			{
				continue;
			}

			const double ActivationTime = Editor->GetLastActivationTime();
			if (ActivationTime > BestActivationTime)
			{
				BestActivationTime = ActivationTime;
				ChosenEditor = Editor;
				bAmbiguousActivation = false;
			}
			else if (ActivationTime > 0.0 && FMath::IsNearlyEqual(ActivationTime, BestActivationTime))
			{
				bAmbiguousActivation = true;
			}
		}

		if (ChosenEditor && BestActivationTime > 0.0 && !bAmbiguousActivation)
		{
			Source = TEXT("focused_editor");
		}
		else
		{
			ChosenEditor = nullptr;
		}
	}

	if (ChosenEditor)
	{
		const TArray<UObject*> EditedAssets = MonolithEditorSelection::GetAssetsForEditor(AssetEditorSubsystem, ChosenEditor);
		if (EditedAssets.Num() > 0)
		{
			return FMonolithActionResult::Success(ObjectToAssetEditorJson(
				EditedAssets[0],
				Source,
				ChosenEditor->GetEditorName().ToString()));
		}
	}

	if (bFallbackToSelection)
	{
		TArray<FAssetData> SelectedAssets;
		MonolithEditorSelection::GetSelectedAssetData(SelectedAssets, nullptr);
		if (SelectedAssets.Num() == 1)
		{
			UObject* SelectedAsset = SelectedAssets[0].GetAsset();
			if (SelectedAsset)
			{
				return FMonolithActionResult::Success(ObjectToAssetEditorJson(
					SelectedAsset,
					TEXT("selected_asset_fallback"),
					FString()));
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> OpenEditorArray;
	for (IAssetEditorInstance* Editor : OpenEditors)
	{
		const TArray<UObject*> EditedAssets = MonolithEditorSelection::GetAssetsForEditor(AssetEditorSubsystem, Editor);
		OpenEditorArray.Add(MakeShared<FJsonValueObject>(OpenEditorSummaryToJson(Editor, EditedAssets)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("has_active_editor"), false);
	Result->SetStringField(TEXT("source"), OpenEditors.Num() > 1 ? TEXT("ambiguous_open_editors") : TEXT("none"));
	Result->SetNumberField(TEXT("open_editor_count"), OpenEditors.Num());
	Result->SetArrayField(TEXT("open_editors"), OpenEditorArray);
	return FMonolithActionResult::Success(Result);
}

bool FMonolithEditorSelectionActions::MatchesClassFilter(const UClass* Class, const FString& Filter)
{
	const FString NormalizedFilter = MonolithEditorSelection::NormalizeClassFilter(Filter);
	if (NormalizedFilter.IsEmpty())
	{
		return true;
	}

	if (!Class)
	{
		return false;
	}

	for (const UClass* Candidate = Class; Candidate; Candidate = Candidate->GetSuperClass())
	{
		if (Candidate->GetName().Equals(NormalizedFilter, ESearchCase::IgnoreCase)
			|| GetClassPath(Candidate).Equals(NormalizedFilter, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

bool FMonolithEditorSelectionActions::MatchesClassFilter(const FTopLevelAssetPath& ClassPath, const FString& Filter)
{
	const FString NormalizedFilter = MonolithEditorSelection::NormalizeClassFilter(Filter);
	if (NormalizedFilter.IsEmpty())
	{
		return true;
	}

	return ClassPath.GetAssetName().ToString().Equals(NormalizedFilter, ESearchCase::IgnoreCase)
		|| ClassPath.ToString().Equals(NormalizedFilter, ESearchCase::IgnoreCase);
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::ActorToJson(AActor* Actor, bool bIncludeComponents)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Actor)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Obj->SetStringField(TEXT("name"), Actor->GetName());
	Obj->SetStringField(TEXT("object_path"), GetObjectPath(Actor));
	Obj->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
	Obj->SetStringField(TEXT("class_path"), GetClassPath(Actor->GetClass()));
	Obj->SetObjectField(TEXT("transform"), MonolithEditorSelection::TransformToJson(Actor->GetActorTransform()));
	Obj->SetStringField(TEXT("world_type"), GetWorldTypeString(Actor->GetWorld()));

	if (ULevel* Level = Actor->GetLevel())
	{
		Obj->SetStringField(TEXT("level_path"), Level->GetOutermost() ? Level->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("level_name"), Level->GetName());
	}
	else
	{
		Obj->SetStringField(TEXT("level_path"), FString());
		Obj->SetStringField(TEXT("level_name"), FString());
	}

	TArray<TSharedPtr<FJsonValue>> ComponentArray;
	if (bIncludeComponents)
	{
		TInlineComponentArray<UActorComponent*> Components(Actor);
		for (UActorComponent* Component : Components)
		{
			ComponentArray.Add(MakeShared<FJsonValueObject>(ComponentToJson(Component)));
		}
	}
	Obj->SetArrayField(TEXT("components"), ComponentArray);

	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::ComponentToJson(UActorComponent* Component)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Component)
	{
		return Obj;
	}

	AActor* Owner = Component->GetOwner();
	Obj->SetStringField(TEXT("name"), Component->GetName());
	Obj->SetStringField(TEXT("object_path"), GetObjectPath(Component));
	Obj->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetName() : FString());
	Obj->SetStringField(TEXT("class_path"), GetClassPath(Component->GetClass()));
	Obj->SetStringField(TEXT("owner_actor_path"), GetObjectPath(Owner));
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::AssetDataToJson(const FAssetData& AssetData, const FString& VirtualPath)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
	Obj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
	Obj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
	Obj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
	Obj->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
	Obj->SetStringField(TEXT("class_path"), AssetData.AssetClassPath.ToString());
	Obj->SetBoolField(TEXT("is_asset"), true);
	Obj->SetBoolField(TEXT("is_folder"), false);
	Obj->SetStringField(TEXT("virtual_path"), VirtualPath);
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::ObjectToAssetEditorJson(UObject* Asset, const FString& Source, const FString& EditorName)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("has_active_editor"), Asset != nullptr);
	Result->SetStringField(TEXT("asset_path"), Asset && Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString());
	Result->SetStringField(TEXT("object_path"), GetObjectPath(Asset));
	Result->SetStringField(TEXT("class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
	Result->SetStringField(TEXT("class_path"), Asset ? GetClassPath(Asset->GetClass()) : FString());
	Result->SetStringField(TEXT("editor_name"), EditorName);
	Result->SetBoolField(TEXT("is_dirty"), Asset && Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false);
	Result->SetStringField(TEXT("source"), Source);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::OpenEditorSummaryToJson(IAssetEditorInstance* Editor, const TArray<UObject*>& EditedAssets)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("editor_name"), Editor ? Editor->GetEditorName().ToString() : FString());
	Obj->SetNumberField(TEXT("last_activation_time"), Editor ? Editor->GetLastActivationTime() : 0.0);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (UObject* Asset : EditedAssets)
	{
		AssetArray.Add(MakeShared<FJsonValueString>(GetObjectPath(Asset)));
	}
	Obj->SetArrayField(TEXT("assets"), AssetArray);

	return Obj;
}

FString FMonolithEditorSelectionActions::GetClassPath(const UClass* Class)
{
	return Class ? Class->GetClassPathName().ToString() : FString();
}

FString FMonolithEditorSelectionActions::GetObjectPath(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

FString FMonolithEditorSelectionActions::GetWorldTypeString(const UWorld* World)
{
	if (!World)
	{
		return TEXT("unknown");
	}

	switch (World->WorldType)
	{
	case EWorldType::None: return TEXT("none");
	case EWorldType::Game: return TEXT("game");
	case EWorldType::Editor: return TEXT("editor");
	case EWorldType::PIE: return TEXT("pie");
	case EWorldType::EditorPreview: return TEXT("editor_preview");
	case EWorldType::GamePreview: return TEXT("game_preview");
	case EWorldType::GameRPC: return TEXT("game_rpc");
	case EWorldType::Inactive: return TEXT("inactive");
	default: return TEXT("unknown");
	}
}

FString FMonolithEditorSelectionActions::GetAssetVirtualPath(const FAssetData& AssetData)
{
	const FString PackagePath = AssetData.PackagePath.ToString();
	if (PackagePath.StartsWith(TEXT("/Game")))
	{
		return FString::Printf(TEXT("/All%s"), *PackagePath);
	}
	return PackagePath;
}
