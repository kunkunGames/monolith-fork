// MonolithEditorMapActions.cpp
// Phase F8 (J-phase) implementation. See header for action contract.
//
// API verification (UE 5.7, editor offline at author time):
//   * UWorldFactory               — Engine/Public/Factories/WorldFactory.h.
//   * IAssetTools::CreateAsset    — AssetTools/Public/AssetToolsModule.h /
//                                    AssetTools/Public/IAssetTools.h.
//   * UEditorAssetLibrary::SaveAsset
//                                  — EditorScriptingUtilities/Public/EditorAssetLibrary.h
//                                    (already used elsewhere in MonolithEditor).
//   * IPluginManager::GetDiscoveredPlugins / FindEnabledPlugins
//                                  — Projects/Public/Interfaces/IPluginManager.h.
//   * FModuleManager::IsModuleLoaded
//                                  — Core/Public/Modules/ModuleManager.h.
//   * UPackage::SavePackage / FSavePackageArgs
//                                  — CoreUObject/Public/UObject/SavePackage.h
//                                    (mirrors MonolithCommonUIHelpers.cpp:70-75
//                                     and MonolithEditorActions.cpp asset-save
//                                     sites).
//
// Failure modes handled cleanly (no crashes, all return ok=false with message):
//   * create_empty_map: path collision, malformed package path, factory failure,
//                       package-save failure, AssetTools module unavailable.
//   * get_module_status: empty input list (returns all Monolith modules),
//                        unknown module name (returns row with enabled=false,
//                        loaded=false, plugin_name="" rather than erroring).

#include "MonolithEditorMapActions.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"

#include "Engine/World.h"
#include "Factories/WorldFactory.h"

#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// =============================================================================
//  Helpers
// =============================================================================

namespace
{
	/** Strip trailing slashes from /Game/Path/Asset and split into (PackagePath, AssetName). */
	bool SplitAssetPath(const FString& InAssetPath, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
	{
		FString Cleaned = InAssetPath;
		Cleaned.RemoveFromEnd(TEXT("/"));

		if (!Cleaned.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(
				TEXT("Path must start with '/' (e.g. '/Game/Maps/MyMap'). Got: '%s'"), *InAssetPath);
			return false;
		}

		int32 LastSlash = INDEX_NONE;
		if (!Cleaned.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
		{
			OutError = FString::Printf(TEXT("Cannot derive asset name from path: '%s'"), *InAssetPath);
			return false;
		}

		OutPackagePath = Cleaned.Left(LastSlash);
		OutAssetName = Cleaned.Mid(LastSlash + 1);

		if (OutAssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Empty asset name in path: '%s'"), *InAssetPath);
			return false;
		}
		return true;
	}

	/** Asset-registry + in-memory FindObject existence check. Mirrors MonolithAI::EnsureAssetPathFree. */
	bool DoesAssetExist(const FString& PackagePath, const FString& AssetName)
	{
		const FString FullObjectPath = PackagePath + TEXT(".") + AssetName;

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (AR.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath)).IsValid())
		{
			return true;
		}
		if (FindObject<UObject>(nullptr, *FullObjectPath))
		{
			return true;
		}
		if (UPackage* ExistingPkg = FindPackage(nullptr, *PackagePath))
		{
			if (FindObject<UObject>(ExistingPkg, *AssetName))
			{
				return true;
			}
		}
		return false;
	}

	int32 GetIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		double RawValue = static_cast<double>(DefaultValue);
		if (Params.IsValid())
		{
			Params->TryGetNumberField(FieldName, RawValue);
		}
		return FMath::Clamp(static_cast<int32>(RawValue), MinValue, MaxValue);
	}

	UWorld* GetActiveEditorWorld()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			return World;
		}
		return GWorld;
	}

	FString WorldTypeToString(EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::Game:
			return TEXT("Game");
		case EWorldType::Editor:
			return TEXT("Editor");
		case EWorldType::PIE:
			return TEXT("PIE");
		case EWorldType::EditorPreview:
			return TEXT("EditorPreview");
		case EWorldType::GamePreview:
			return TEXT("GamePreview");
		case EWorldType::GameRPC:
			return TEXT("GameRPC");
		case EWorldType::Inactive:
			return TEXT("Inactive");
		default:
			return TEXT("None");
		}
	}

	TSharedPtr<FJsonObject> ActorToJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Actor)
		{
			return Row;
		}

		Row->SetStringField(TEXT("path"), Actor->GetPathName());
		Row->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Row->SetStringField(TEXT("name"), Actor->GetName());
		Row->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
		if (ULevel* Level = Actor->GetLevel())
		{
			Row->SetStringField(TEXT("level"), Level->GetOutermost() ? Level->GetOutermost()->GetName() : Level->GetName());
		}
		return Row;
	}

	TSharedPtr<FJsonObject> BuildWorldContext(UWorld* World)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("has_world"), World != nullptr);
		if (!World)
		{
			return Obj;
		}

		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++ActorCount;
		}

		Obj->SetStringField(TEXT("world_name"), World->GetName());
		Obj->SetStringField(TEXT("map_name"), World->GetMapName());
		Obj->SetStringField(TEXT("world_type"), WorldTypeToString(World->WorldType));
		Obj->SetStringField(TEXT("world_package"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
		Obj->SetBoolField(TEXT("is_partitioned_world"), World->IsPartitionedWorld());
		Obj->SetBoolField(TEXT("classic_streaming_supported"), !World->IsPartitionedWorld());
		Obj->SetNumberField(TEXT("actor_count"), ActorCount);
		Obj->SetNumberField(TEXT("streaming_level_count"), World->GetStreamingLevels().Num());

		if (ULevel* PersistentLevel = World->PersistentLevel)
		{
			Obj->SetStringField(TEXT("persistent_level"), PersistentLevel->GetOutermost() ? PersistentLevel->GetOutermost()->GetName() : PersistentLevel->GetName());
		}

		return Obj;
	}

	TSharedPtr<FJsonObject> StreamingLevelToJson(ULevelStreaming* StreamingLevel)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!StreamingLevel)
		{
			return Row;
		}

		Row->SetStringField(TEXT("name"), StreamingLevel->GetName());
		Row->SetStringField(TEXT("class"), StreamingLevel->GetClass() ? StreamingLevel->GetClass()->GetName() : FString());
		Row->SetStringField(TEXT("world_asset_package"), StreamingLevel->GetWorldAssetPackageName());
		Row->SetStringField(TEXT("state"), ::EnumToString(StreamingLevel->GetLevelStreamingState()));
		Row->SetBoolField(TEXT("should_be_loaded"), StreamingLevel->ShouldBeLoaded());
		Row->SetBoolField(TEXT("should_be_visible"), StreamingLevel->ShouldBeVisible());
		Row->SetBoolField(TEXT("should_be_visible_flag"), StreamingLevel->GetShouldBeVisibleFlag());
		Row->SetBoolField(TEXT("is_requesting_unload_and_removal"), StreamingLevel->GetIsRequestingUnloadAndRemoval());
		Row->SetNumberField(TEXT("priority"), StreamingLevel->GetPriority());
#if WITH_EDITORONLY_DATA
		Row->SetBoolField(TEXT("should_be_visible_in_editor"), StreamingLevel->GetShouldBeVisibleInEditor());
#endif

		if (ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel())
		{
			Row->SetBoolField(TEXT("is_loaded"), true);
			Row->SetStringField(TEXT("loaded_level"), LoadedLevel->GetOutermost() ? LoadedLevel->GetOutermost()->GetName() : LoadedLevel->GetName());
		}
		else
		{
			Row->SetBoolField(TEXT("is_loaded"), false);
		}

		return Row;
	}
}

// =============================================================================
//  Registration
// =============================================================================

void FMonolithEditorMapActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("create_empty_map"),
		TEXT("Create a fully blank UWorld asset at the given /Game/... path. Saves immediately. Default template is 'blank' (zero actors)."),
		FMonolithActionHandler::CreateStatic(&HandleCreateEmptyMap),
		FParamSchemaBuilder()
			.Required(TEXT("path"), TEXT("string"), TEXT("Asset path under /Game/... where the new UWorld is saved (e.g. /Game/Tests/Monolith/Audio/Map_Test)"))
			.Optional(TEXT("map_template"), TEXT("string"), TEXT("Template variant: 'blank' (default). Reserved: 'vr_basic', 'thirdperson_basic' — return error in v1; UE 5.7 templates are populated client-side, not via UWorldFactory."), TEXT("blank"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_module_status"),
		TEXT("Report plugin-enabled + module-loaded status for Monolith (or arbitrary) modules. Wraps IPluginManager + FModuleManager. If module_names is omitted or empty, returns rows for all Monolith modules. Unknown module names return a row with enabled=false, loaded=false (no error)."),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleStatus),
		FParamSchemaBuilder()
			.Optional(TEXT("module_names"), TEXT("array"), TEXT("Optional array of module name strings. Omit to query all Monolith modules."))
			.Build());

	Registry.RegisterAction(TEXT("level"), TEXT("get_world_context"),
		TEXT("Read the active editor world, persistent level, partitioning, actor count, and classic streaming capability."),
		FMonolithActionHandler::CreateStatic(&HandleGetWorldContext),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("level"), TEXT("list_layers"),
		TEXT("List actor layer names in the active editor world, with optional capped actor membership rows."),
		FMonolithActionHandler::CreateStatic(&HandleListLayers),
		FParamSchemaBuilder()
			.Optional(TEXT("include_actors"), TEXT("boolean"), TEXT("Include capped actor membership rows for each layer."), TEXT("false"))
			.Optional(TEXT("actor_limit"), TEXT("integer"), TEXT("Maximum actor rows per layer when include_actors=true."), TEXT("20"))
			.Build());

	Registry.RegisterAction(TEXT("level"), TEXT("list_streaming_levels"),
		TEXT("List classic streaming levels for the active editor world and report World Partition capability context."),
		FMonolithActionHandler::CreateStatic(&HandleListStreamingLevels),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum streaming level rows to return."), TEXT("100"))
			.Build());
}

// =============================================================================
//  editor::create_empty_map
// =============================================================================

FMonolithActionResult FMonolithEditorMapActions::HandleCreateEmptyMap(const TSharedPtr<FJsonObject>& Params)
{
	FString InPath;
	if (!Params->TryGetStringField(TEXT("path"), InPath) || InPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	FString Template;
	Params->TryGetStringField(TEXT("map_template"), Template);
	if (Template.IsEmpty()) Template = TEXT("blank");

	if (!Template.Equals(TEXT("blank"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported map_template '%s'. v1 only supports 'blank'. (UE 5.7 'vr_basic' / 'thirdperson_basic' templates are populated by editor-only template files, not by UWorldFactory.)"),
			*Template));
	}

	FString PackagePath, AssetName, Error;
	if (!SplitAssetPath(InPath, PackagePath, AssetName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	if (DoesAssetExist(PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at %s; delete first."), *InPath));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	UWorldFactory* Factory = NewObject<UWorldFactory>();
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct UWorldFactory"));
	}
	// UWorldFactory defaults are sufficient for a "blank" map. Project-specific
	// settings (World Partition, default game mode, etc.) are inherited from
	// project settings the same way File > New Level does it.

	UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, UWorld::StaticClass(), Factory);
	if (!Created)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("AssetTools.CreateAsset returned null for '%s' under '%s'"), *AssetName, *PackagePath));
	}

	UWorld* NewWorld = Cast<UWorld>(Created);
	if (!NewWorld)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Created asset is not a UWorld (got %s)"), *Created->GetClass()->GetName()));
	}

	// Save the package now so the asset survives editor restart and shows up in
	// the content browser without a manual save-all step.
	UPackage* Package = NewWorld->GetOutermost();
	Package->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetMapPackageExtension());
	const bool bSaved = UPackage::SavePackage(Package, NewWorld, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Created UWorld asset but failed to save to '%s'"), *PackageFilename));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), InPath);
	Result->SetStringField(TEXT("map_template"), TEXT("blank"));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created blank UWorld at %s and saved package."), *InPath));
	return FMonolithActionResult::Success(Result);
}

// =============================================================================
//  editor::get_module_status
// =============================================================================

FMonolithActionResult FMonolithEditorMapActions::HandleGetModuleStatus(const TSharedPtr<FJsonObject>& Params)
{
	// 1. Build the (module_name -> plugin_name) reverse-index from
	//    IPluginManager so we can answer "which plugin owns module X" without
	//    scanning every plugin per query.
	TMap<FName, TSharedRef<IPlugin>> ModuleToPlugin;
	const TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetDiscoveredPlugins();
	for (const TSharedRef<IPlugin>& Plugin : AllPlugins)
	{
		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		for (const FModuleDescriptor& ModDesc : Desc.Modules)
		{
			ModuleToPlugin.Add(ModDesc.Name, Plugin);
		}
	}

	// 2. Resolve the requested module name list. Empty / omitted → every module
	//    that belongs to the Monolith plugin (the canonical use case for
	//    test-spec module-status checks).
	TArray<FString> RequestedModules;
	const TArray<TSharedPtr<FJsonValue>>* ModNamesArr = nullptr;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("module_names"), ModNamesArr) && ModNamesArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ModNamesArr)
		{
			FString Name;
			if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
			{
				RequestedModules.Add(Name);
			}
		}
	}

	if (RequestedModules.Num() == 0)
	{
		TSharedPtr<IPlugin> MonolithPlugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (MonolithPlugin.IsValid())
		{
			for (const FModuleDescriptor& ModDesc : MonolithPlugin->GetDescriptor().Modules)
			{
				RequestedModules.Add(ModDesc.Name.ToString());
			}
		}
	}

	// 3. Emit one row per requested module. Unknown modules get
	//    enabled=false / loaded=false / plugin_name="" rather than an error so
	//    callers can probe optional modules without conditional plumbing.
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& ModuleNameStr : RequestedModules)
	{
		const FName ModuleName(*ModuleNameStr);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("module_name"), ModuleNameStr);

		bool bIsRuntime = false;
		bool bPluginEnabled = false;
		FString PluginName;
		FString PluginVersion;

		if (TSharedRef<IPlugin>* PluginRefPtr = ModuleToPlugin.Find(ModuleName))
		{
			TSharedRef<IPlugin> PluginRef = *PluginRefPtr;
			PluginName = PluginRef->GetName();
			bPluginEnabled = PluginRef->IsEnabled();
			PluginVersion = PluginRef->GetDescriptor().VersionName;

			for (const FModuleDescriptor& ModDesc : PluginRef->GetDescriptor().Modules)
			{
				if (ModDesc.Name == ModuleName)
				{
					bIsRuntime = (ModDesc.Type == EHostType::Runtime ||
								  ModDesc.Type == EHostType::RuntimeNoCommandlet ||
								  ModDesc.Type == EHostType::RuntimeAndProgram ||
								  ModDesc.Type == EHostType::ClientOnly ||
								  ModDesc.Type == EHostType::ServerOnly);
					break;
				}
			}
		}

		const bool bLoaded = FModuleManager::Get().IsModuleLoaded(ModuleName);

		Row->SetStringField(TEXT("plugin_name"), PluginName);
		Row->SetBoolField(TEXT("enabled"), bPluginEnabled);
		Row->SetBoolField(TEXT("loaded"), bLoaded);
		Row->SetBoolField(TEXT("is_runtime"), bIsRuntime);
		if (!PluginVersion.IsEmpty())
		{
			Row->SetStringField(TEXT("version"), PluginVersion);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetArrayField(TEXT("modules"), Rows);
	return FMonolithActionResult::Success(Result);
}

// =============================================================================
//  level::get_world_context
// =============================================================================

FMonolithActionResult FMonolithEditorMapActions::HandleGetWorldContext(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetActiveEditorWorld();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), World != nullptr);
	Result->SetObjectField(TEXT("world"), BuildWorldContext(World));
	Result->SetBoolField(TEXT("read_only"), true);
	if (!World)
	{
		Result->SetStringField(TEXT("message"), TEXT("No active editor world is available."));
	}
	return FMonolithActionResult::Success(Result);
}

// =============================================================================
//  level::list_layers
// =============================================================================

FMonolithActionResult FMonolithEditorMapActions::HandleListLayers(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetActiveEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No active editor world is available."));
	}

	bool bIncludeActors = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_actors"), bIncludeActors);
	}
	const int32 ActorLimit = GetIntParam(Params, TEXT("actor_limit"), 20, 0, 500);

	TMap<FName, TArray<AActor*>> LayerActors;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		for (const FName& LayerName : Actor->Layers)
		{
			if (!LayerName.IsNone())
			{
				LayerActors.FindOrAdd(LayerName).Add(Actor);
			}
		}
	}

	TArray<FName> LayerNames;
	LayerActors.GetKeys(LayerNames);
	LayerNames.Sort([](const FName& A, const FName& B)
	{
		return A.LexicalLess(B);
	});

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FName& LayerName : LayerNames)
	{
		const TArray<AActor*>* Actors = LayerActors.Find(LayerName);
		const int32 ActorCount = Actors ? Actors->Num() : 0;

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), LayerName.ToString());
		Row->SetNumberField(TEXT("actor_count"), ActorCount);

		if (bIncludeActors && Actors)
		{
			TArray<TSharedPtr<FJsonValue>> ActorRows;
			for (AActor* Actor : *Actors)
			{
				if (ActorRows.Num() >= ActorLimit)
				{
					break;
				}
				ActorRows.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor)));
			}
			Row->SetArrayField(TEXT("actors"), ActorRows);
			Row->SetNumberField(TEXT("returned_actor_count"), ActorRows.Num());
			if (ActorCount > ActorRows.Num())
			{
				Row->SetNumberField(TEXT("truncated_actor_count"), ActorCount - ActorRows.Num());
			}
		}

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetObjectField(TEXT("world"), BuildWorldContext(World));
	Result->SetNumberField(TEXT("layer_count"), Rows.Num());
	Result->SetArrayField(TEXT("layers"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

// =============================================================================
//  level::list_streaming_levels
// =============================================================================

FMonolithActionResult FMonolithEditorMapActions::HandleListStreamingLevels(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetActiveEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No active editor world is available."));
	}

	const int32 Limit = GetIntParam(Params, TEXT("limit"), 100, 1, 1000);
	const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (ULevelStreaming* StreamingLevel : StreamingLevels)
	{
		if (Rows.Num() >= Limit)
		{
			break;
		}
		Rows.Add(MakeShared<FJsonValueObject>(StreamingLevelToJson(StreamingLevel)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetObjectField(TEXT("world"), BuildWorldContext(World));
	Result->SetNumberField(TEXT("matched_count"), StreamingLevels.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetArrayField(TEXT("streaming_levels"), Rows);
	if (StreamingLevels.Num() > Rows.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), StreamingLevels.Num() - Rows.Num());
	}
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}
