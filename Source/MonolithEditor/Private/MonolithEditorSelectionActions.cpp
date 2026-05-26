#include "MonolithEditorSelectionActions.h"

#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

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

static int32 GetBoundedIntParam(const TSharedPtr<FJsonObject>& Params, const FString& Name, int32 DefaultValue, int32 MinValue, int32 MaxValue)
{
	double NumericValue = static_cast<double>(DefaultValue);
	if (Params.IsValid())
	{
		Params->TryGetNumberField(Name, NumericValue);
	}
	return FMath::Clamp(static_cast<int32>(NumericValue), MinValue, MaxValue);
}

static TArray<FString> GetStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& ArrayName, const FString& ScalarName)
{
	TArray<FString> Result;
	if (!Params.IsValid())
	{
		return Result;
	}

	FString ScalarValue;
	if (Params->TryGetStringField(ScalarName, ScalarValue) && !ScalarValue.IsEmpty())
	{
		Result.Add(ScalarValue);
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
	if (Params->TryGetArrayField(ArrayName, ArrayValues) && ArrayValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ArrayValues)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				Result.Add(StringValue);
			}
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

	Registry.RegisterAction(TEXT("editor"), TEXT("describe_current_selection"),
		TEXT("Return compact Monolith context summaries for selected actors, assets, and folders without creating prompt/session state"),
		FMonolithActionHandler::CreateStatic(&HandleDescribeCurrentSelection),
		FParamSchemaBuilder()
			.Optional(TEXT("include_metadata"), TEXT("bool"), TEXT("Include bounded metadata objects for each summarized item"), TEXT("false"))
			.Optional(TEXT("include_components"), TEXT("bool"), TEXT("Include selected actor component metadata when include_metadata is true"), TEXT("false"))
			.Optional(TEXT("include_folders"), TEXT("bool"), TEXT("Include selected Content Browser folders as folder context entries"), TEXT("false"))
			.Optional(TEXT("max_items"), TEXT("number"), TEXT("Maximum actor/asset/folder summaries to return, clamped to 1..200"), TEXT("50"))
			.Build(),
		TEXT("Context"));

	Registry.RegisterAction(TEXT("editor"), TEXT("describe_asset_context"),
		TEXT("Return compact Monolith context summaries for explicit asset paths"),
		FMonolithActionHandler::CreateStatic(&HandleDescribeAssetContext),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Single asset path, package path, or object path to summarize"))
			.Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Asset paths, package paths, or object paths to summarize"))
			.Optional(TEXT("include_metadata"), TEXT("bool"), TEXT("Include bounded FAssetData metadata in each returned item"), TEXT("false"))
			.Optional(TEXT("max_items"), TEXT("number"), TEXT("Maximum asset summaries to return, clamped to 1..200"), TEXT("50"))
			.Build(),
		TEXT("Context"));

	Registry.RegisterAction(TEXT("editor"), TEXT("describe_actor_context"),
		TEXT("Return compact Monolith context summaries for explicit actor paths, names, or editor labels"),
		FMonolithActionHandler::CreateStatic(&HandleDescribeActorContext),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_path"), TEXT("string"), TEXT("Single actor object path, internal name, or editor label to summarize"))
			.Optional(TEXT("actor_paths"), TEXT("array"), TEXT("Actor object paths, internal names, or editor labels to summarize"))
			.Optional(TEXT("include_metadata"), TEXT("bool"), TEXT("Include bounded actor metadata in each returned item"), TEXT("false"))
			.Optional(TEXT("include_components"), TEXT("bool"), TEXT("Include actor component metadata when include_metadata is true"), TEXT("false"))
			.Optional(TEXT("max_items"), TEXT("number"), TEXT("Maximum actor summaries to return, clamped to 1..200"), TEXT("50"))
			.Build(),
		TEXT("Context"));

	Registry.RegisterAction(TEXT("editor"), TEXT("list_context_entrypoints"),
		TEXT("Report Monolith editor context entrypoints, supported object types, and action-level implementation status"),
		FMonolithActionHandler::CreateStatic(&HandleListContextEntrypoints),
		FParamSchemaBuilder().Build(),
		TEXT("Context"));
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
	ActorArray.Reserve(SelectedActors->Num());
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
	AssetArray.Reserve(SelectedAssets.Num());
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
		FolderArray.Reserve(SelectedFolders.Num());
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
	OpenEditorArray.Reserve(OpenEditors.Num());
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

FMonolithActionResult FMonolithEditorSelectionActions::HandleDescribeCurrentSelection(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bIncludeMetadata = false;
	bool bIncludeComponents = false;
	bool bIncludeFolders = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
		Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
		Params->TryGetBoolField(TEXT("include_folders"), bIncludeFolders);
	}
	const int32 MaxItems = MonolithEditorSelection::GetBoundedIntParam(Params, TEXT("max_items"), 50, 1, 200);

	TArray<TSharedPtr<FJsonValue>> Items;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	int32 ActorSelectionCount = 0;
	int32 AssetSelectionCount = 0;
	int32 FolderSelectionCount = 0;

	if (USelection* SelectedActors = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*SelectedActors); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (!Actor)
			{
				continue;
			}

			++ActorSelectionCount;
			if (Items.Num() >= MaxItems)
			{
				Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("actor"), GetObjectPath(Actor), TEXT("max_items_exceeded"))));
				continue;
			}

			Items.Add(MakeShared<FJsonValueObject>(ActorToContextJson(Actor, bIncludeMetadata, bIncludeComponents)));
		}
	}

	TArray<FAssetData> SelectedAssets;
	TArray<FString> SelectedFolders;
	MonolithEditorSelection::GetSelectedAssetData(SelectedAssets, bIncludeFolders ? &SelectedFolders : nullptr);
	AssetSelectionCount = SelectedAssets.Num();
	FolderSelectionCount = SelectedFolders.Num();

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (!AssetData.IsValid())
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("asset"), TEXT("invalid_asset_data"), TEXT("invalid"))));
			continue;
		}

		if (Items.Num() >= MaxItems)
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("asset"), AssetData.GetObjectPathString(), TEXT("max_items_exceeded"))));
			continue;
		}

		Items.Add(MakeShared<FJsonValueObject>(AssetDataToContextJson(AssetData, bIncludeMetadata, GetAssetVirtualPath(AssetData))));
	}

	if (bIncludeFolders)
	{
		for (const FString& Folder : SelectedFolders)
		{
			if (Items.Num() >= MaxItems)
			{
				Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("folder"), Folder, TEXT("max_items_exceeded"))));
				continue;
			}

			Items.Add(MakeShared<FJsonValueObject>(FolderToContextJson(Folder)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source"), TEXT("current_editor_selection"));
	Result->SetNumberField(TEXT("item_count"), Items.Num());
	Result->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	Result->SetNumberField(TEXT("selected_actor_count"), ActorSelectionCount);
	Result->SetNumberField(TEXT("selected_asset_count"), AssetSelectionCount);
	Result->SetNumberField(TEXT("selected_folder_count"), FolderSelectionCount);
	Result->SetBoolField(TEXT("metadata_included"), bIncludeMetadata);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetArrayField(TEXT("skipped"), Skipped);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleDescribeAssetContext(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> AssetPaths = MonolithEditorSelection::GetStringArrayParam(Params, TEXT("asset_paths"), TEXT("asset_path"));
	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_path or asset_paths is required"));
	}

	bool bIncludeMetadata = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	}
	const int32 MaxItems = MonolithEditorSelection::GetBoundedIntParam(Params, TEXT("max_items"), 50, 1, 200);

	TArray<TSharedPtr<FJsonValue>> Items;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	Items.Reserve(FMath::Min(AssetPaths.Num(), MaxItems));
	Skipped.Reserve(FMath::Max(0, AssetPaths.Num() - MaxItems));
	for (const FString& AssetPath : AssetPaths)
	{
		if (Items.Num() >= MaxItems)
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("asset"), AssetPath, TEXT("max_items_exceeded"))));
			continue;
		}

		const FAssetData AssetData = ResolveAssetData(AssetPath);
		if (!AssetData.IsValid())
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("asset"), AssetPath, TEXT("not_found"))));
			continue;
		}

		Items.Add(MakeShared<FJsonValueObject>(AssetDataToContextJson(AssetData, bIncludeMetadata, GetAssetVirtualPath(AssetData))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source"), TEXT("explicit_asset_paths"));
	Result->SetNumberField(TEXT("item_count"), Items.Num());
	Result->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	Result->SetBoolField(TEXT("metadata_included"), bIncludeMetadata);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetArrayField(TEXT("skipped"), Skipped);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleDescribeActorContext(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	TArray<FString> ActorReferences = MonolithEditorSelection::GetStringArrayParam(Params, TEXT("actor_paths"), TEXT("actor_path"));
	if (ActorReferences.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("actor_path or actor_paths is required"));
	}

	bool bIncludeMetadata = false;
	bool bIncludeComponents = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
		Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
	}
	const int32 MaxItems = MonolithEditorSelection::GetBoundedIntParam(Params, TEXT("max_items"), 50, 1, 200);

	TArray<TSharedPtr<FJsonValue>> Items;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	Items.Reserve(FMath::Min(ActorReferences.Num(), MaxItems));
	Skipped.Reserve(FMath::Max(0, ActorReferences.Num() - MaxItems));
	for (const FString& ActorReference : ActorReferences)
	{
		if (Items.Num() >= MaxItems)
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("actor"), ActorReference, TEXT("max_items_exceeded"))));
			continue;
		}

		AActor* Actor = FindActorByReference(ActorReference);
		if (!Actor)
		{
			Skipped.Add(MakeShared<FJsonValueObject>(SkippedContextToJson(TEXT("actor"), ActorReference, TEXT("not_found"))));
			continue;
		}

		Items.Add(MakeShared<FJsonValueObject>(ActorToContextJson(Actor, bIncludeMetadata, bIncludeComponents)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source"), TEXT("explicit_actor_references"));
	Result->SetNumberField(TEXT("item_count"), Items.Num());
	Result->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	Result->SetBoolField(TEXT("metadata_included"), bIncludeMetadata);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetArrayField(TEXT("skipped"), Skipped);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSelectionActions::HandleListContextEntrypoints(const TSharedPtr<FJsonObject>& /*Params*/)
{
	TArray<TSharedPtr<FJsonValue>> Entrypoints;

	auto AddEntrypoint = [&Entrypoints](const FString& Id, const FString& Action, const FString& ObjectTypes, bool bAvailable, const FString& Status)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), Id);
		Entry->SetStringField(TEXT("namespace"), TEXT("editor"));
		Entry->SetStringField(TEXT("action"), Action);
		Entry->SetStringField(TEXT("object_types"), ObjectTypes);
		Entry->SetBoolField(TEXT("available"), bAvailable);
		Entry->SetStringField(TEXT("status"), Status);
		Entrypoints.Add(MakeShared<FJsonValueObject>(Entry));
	};

	AddEntrypoint(TEXT("current_selection"), TEXT("describe_current_selection"), TEXT("actors, assets, folders"), true, TEXT("action_available"));
	AddEntrypoint(TEXT("explicit_asset_paths"), TEXT("describe_asset_context"), TEXT("assets"), true, TEXT("action_available"));
	AddEntrypoint(TEXT("explicit_actor_references"), TEXT("describe_actor_context"), TEXT("actors"), true, TEXT("action_available"));
	AddEntrypoint(TEXT("selected_graph_nodes"), TEXT("blueprint.describe_selected_graph_nodes"), TEXT("blueprint graph nodes"), false, TEXT("requires_graph_editor_adapter"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetStringField(TEXT("settings_gate"), TEXT("UMonolithSettings.bEnableEditor"));
	Result->SetNumberField(TEXT("entrypoint_count"), Entrypoints.Num());
	Result->SetArrayField(TEXT("entrypoints"), Entrypoints);
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
		ComponentArray.Reserve(Components.Num());
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

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::ActorToContextJson(AActor* Actor, bool bIncludeMetadata, bool bIncludeComponents)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Actor)
	{
		return Obj;
	}

	const FString Label = Actor->GetActorLabel();
	const FString ObjectPath = GetObjectPath(Actor);
	const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

	Obj->SetStringField(TEXT("type"), TEXT("actor"));
	Obj->SetStringField(TEXT("display_name"), Label);
	Obj->SetStringField(TEXT("reference"), ObjectPath);
	Obj->SetStringField(TEXT("class"), ClassName);
	Obj->SetStringField(TEXT("class_path"), GetClassPath(Actor->GetClass()));
	Obj->SetStringField(TEXT("summary"), FString::Printf(TEXT("Actor '%s' (%s) at %s"), *Label, *ClassName, *ObjectPath));
	Obj->SetStringField(TEXT("suggested_action"), TEXT("editor.describe_actor_context"));
	if (bIncludeMetadata)
	{
		Obj->SetObjectField(TEXT("metadata"), ActorToJson(Actor, bIncludeComponents));
	}
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::AssetDataToContextJson(const FAssetData& AssetData, bool bIncludeMetadata, const FString& VirtualPath)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	const FString ObjectPath = AssetData.GetObjectPathString();
	const FString AssetName = AssetData.AssetName.ToString();
	const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

	Obj->SetStringField(TEXT("type"), TEXT("asset"));
	Obj->SetStringField(TEXT("display_name"), AssetName);
	Obj->SetStringField(TEXT("reference"), ObjectPath);
	Obj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
	Obj->SetStringField(TEXT("class"), ClassName);
	Obj->SetStringField(TEXT("class_path"), AssetData.AssetClassPath.ToString());
	Obj->SetStringField(TEXT("virtual_path"), VirtualPath);
	Obj->SetStringField(TEXT("summary"), FString::Printf(TEXT("Asset '%s' (%s) at %s"), *AssetName, *ClassName, *ObjectPath));
	Obj->SetStringField(TEXT("suggested_action"), TEXT("editor.describe_asset_context"));
	if (bIncludeMetadata)
	{
		Obj->SetObjectField(TEXT("metadata"), AssetDataToJson(AssetData, VirtualPath));
	}
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::FolderToContextJson(const FString& FolderPath)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("folder"));
	Obj->SetStringField(TEXT("display_name"), FPaths::GetCleanFilename(FolderPath));
	Obj->SetStringField(TEXT("reference"), FolderPath);
	Obj->SetStringField(TEXT("virtual_path"), FolderPath);
	Obj->SetStringField(TEXT("summary"), FString::Printf(TEXT("Content Browser folder '%s'"), *FolderPath));
	Obj->SetStringField(TEXT("suggested_action"), TEXT("editor.describe_current_selection"));
	return Obj;
}

TSharedPtr<FJsonObject> FMonolithEditorSelectionActions::SkippedContextToJson(const FString& Type, const FString& Reference, const FString& Reason)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), Type);
	Obj->SetStringField(TEXT("reference"), Reference);
	Obj->SetStringField(TEXT("reason"), Reason);
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
	AssetArray.Reserve(EditedAssets.Num());
	for (UObject* Asset : EditedAssets)
	{
		AssetArray.Add(MakeShared<FJsonValueString>(GetObjectPath(Asset)));
	}
	Obj->SetArrayField(TEXT("assets"), AssetArray);

	return Obj;
}

AActor* FMonolithEditorSelectionActions::FindActorByReference(const FString& Reference)
{
	if (Reference.IsEmpty() || !GEditor)
	{
		return nullptr;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		if (Actor->GetPathName() == Reference
			|| Actor->GetName() == Reference
			|| Actor->GetActorNameOrLabel() == Reference
			|| Actor->GetActorLabel() == Reference)
		{
			return Actor;
		}
	}

	return nullptr;
}

FAssetData FMonolithEditorSelectionActions::ResolveAssetData(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return FAssetData();
	}

	FString Resolved = FMonolithAssetUtils::ResolveAssetPath(AssetPath);
	FString ObjectPath = Resolved;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = FString::Printf(TEXT("%s.%s"), *Resolved, *FPaths::GetCleanFilename(Resolved));
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid() && ObjectPath != AssetPath)
	{
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	}
	return AssetData;
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
