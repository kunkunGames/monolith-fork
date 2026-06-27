#include "MonolithLevelSequenceActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "LevelSequence.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 MaxReplayRows = 500;
	constexpr int32 MaxSavedReplayFileRows = 500;

	FString NormalizeJsonPath(FString Path)
	{
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString SavedRelativePath(const FString& Path)
	{
		const FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
		FString RelativePath = AbsolutePath;
		if (FPaths::MakePathRelativeTo(RelativePath, *SavedRoot))
		{
			return NormalizeJsonPath(RelativePath);
		}
		return TEXT("<outside_project_saved>");
	}

	void NormalizeReplayFilesystemPath(FString& Path)
	{
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}
	}

	TArray<FString> GetReplaySearchRoots()
	{
		const FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		return {
			FPaths::Combine(SavedRoot, TEXT("Demos")),
			FPaths::Combine(SavedRoot, TEXT("Replays")),
			FPaths::Combine(SavedRoot, TEXT("Replay"))
		};
	}

	bool IsSameOrUnderDirectory(const FString& CandidatePath, const FString& DirectoryPath)
	{
		return CandidatePath == DirectoryPath || CandidatePath.StartsWith(DirectoryPath + TEXT("/"));
	}

	bool TryGetReplayRootForPath(const FString& AbsolutePath, FString& OutReplayRoot)
	{
		FString NormalizedPath = FPaths::ConvertRelativePathToFull(AbsolutePath);
		NormalizeReplayFilesystemPath(NormalizedPath);

		for (const FString& Root : GetReplaySearchRoots())
		{
			FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Root);
			NormalizeReplayFilesystemPath(NormalizedRoot);
			if (IsSameOrUnderDirectory(NormalizedPath, NormalizedRoot))
			{
				OutReplayRoot = NormalizedRoot;
				return true;
			}
		}

		return false;
	}

	bool TryResolveSavedReplayPath(const FString& RawSavedRelativePath, FString& OutAbsolutePath, FString& OutReplayRoot, FString& OutError)
	{
		FString SavedRelativePathParam = RawSavedRelativePath;
		SavedRelativePathParam.TrimStartAndEndInline();
		FPaths::NormalizeFilename(SavedRelativePathParam);

		if (SavedRelativePathParam.IsEmpty())
		{
			OutError = TEXT("saved_relative_path is required");
			return false;
		}

		if (!FPaths::IsRelative(SavedRelativePathParam)
			|| SavedRelativePathParam.Contains(TEXT(":"))
			|| SavedRelativePathParam.StartsWith(TEXT("//"))
			|| SavedRelativePathParam == TEXT("..")
			|| SavedRelativePathParam.StartsWith(TEXT("../"))
			|| SavedRelativePathParam.Contains(TEXT("/../"))
			|| SavedRelativePathParam.EndsWith(TEXT("/..")))
		{
			OutError = TEXT("saved_relative_path must be a relative path returned by list_saved_replays");
			return false;
		}

		FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), SavedRelativePathParam));
		NormalizeReplayFilesystemPath(Candidate);
		if (!TryGetReplayRootForPath(Candidate, OutReplayRoot))
		{
			OutError = TEXT("saved_relative_path must resolve under Saved/Demos, Saved/Replays, or Saved/Replay");
			return false;
		}
		if (Candidate == OutReplayRoot)
		{
			OutError = TEXT("saved_relative_path must identify a replay container or file returned in a list_saved_replays replay row");
			return false;
		}

		if (!IFileManager::Get().DirectoryExists(*Candidate) && IFileManager::Get().FileSize(*Candidate) < 0)
		{
			OutError = FString::Printf(TEXT("saved replay path not found: %s"), *SavedRelativePathParam);
			return false;
		}

		OutAbsolutePath = Candidate;
		return true;
	}

	void GatherReplayFiles(const FString& DirectoryPath, bool bIncludeNestedFiles, TArray<FString>& OutFiles)
	{
		if (bIncludeNestedFiles)
		{
			IFileManager::Get().FindFilesRecursive(OutFiles, *DirectoryPath, TEXT("*"), true, false);
		}
		else
		{
			TArray<FString> FileNames;
			IFileManager::Get().FindFiles(FileNames, *(DirectoryPath / TEXT("*")), true, false);
			for (const FString& FileName : FileNames)
			{
				OutFiles.Add(FPaths::Combine(DirectoryPath, FileName));
			}
		}
		OutFiles.Sort();
	}

	FString WorldTypeToString(EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::None: return TEXT("None");
		case EWorldType::Game: return TEXT("Game");
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview: return TEXT("GamePreview");
		case EWorldType::GameRPC: return TEXT("GameRPC");
		case EWorldType::Inactive: return TEXT("Inactive");
		default: return TEXT("Unknown");
		}
	}

	TSharedPtr<FJsonObject> MakeWorldJson(const TCHAR* Label, const UWorld* World)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("label"), Label);
		Json->SetBoolField(TEXT("available"), World != nullptr);
		if (World)
		{
			Json->SetStringField(TEXT("name"), World->GetName());
			Json->SetStringField(TEXT("world_type"), WorldTypeToString(World->WorldType));
			Json->SetBoolField(TEXT("is_game_world"), World->IsGameWorld());
			Json->SetBoolField(TEXT("is_paused"), World->IsPaused());
		}
		return Json;
	}

	TSharedPtr<FJsonObject> MakeReplayRootJson(const FString& Root)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), FPaths::GetPathLeaf(Root));
		Json->SetStringField(TEXT("saved_relative_path"), SavedRelativePath(Root));
		Json->SetBoolField(TEXT("exists"), IFileManager::Get().DirectoryExists(*Root));
		return Json;
	}

	TSharedPtr<FJsonObject> MakeSavedReplayFileRow(const FString& File)
	{
		const int64 FileBytes = IFileManager::Get().FileSize(*File);
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*File);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("kind"), TEXT("replay_file"));
		Row->SetStringField(TEXT("name"), FPaths::GetCleanFilename(File));
		Row->SetStringField(TEXT("extension"), FPaths::GetExtension(File).ToLower());
		Row->SetStringField(TEXT("saved_relative_path"), SavedRelativePath(File));
		Row->SetNumberField(TEXT("size_bytes"), static_cast<double>(FileBytes));
		Row->SetStringField(TEXT("modified_utc"), Timestamp.ToIso8601());
		return Row;
	}

	TSharedPtr<FJsonObject> MakeSavedReplayContainerRow(const FString& DirectoryPath, const TArray<FString>& ContainedFiles)
	{
		int64 TotalBytes = 0;
		for (const FString& File : ContainedFiles)
		{
			const int64 FileBytes = IFileManager::Get().FileSize(*File);
			if (FileBytes > 0)
			{
				TotalBytes += FileBytes;
			}
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("kind"), TEXT("replay_container"));
		Row->SetStringField(TEXT("name"), FPaths::GetPathLeaf(DirectoryPath));
		Row->SetStringField(TEXT("saved_relative_path"), SavedRelativePath(DirectoryPath));
		Row->SetNumberField(TEXT("file_count"), static_cast<double>(ContainedFiles.Num()));
		Row->SetNumberField(TEXT("total_size_bytes"), static_cast<double>(TotalBytes));
		return Row;
	}

	void AddReplayRootStatusRows(TArray<TSharedPtr<FJsonValue>>& OutRows)
	{
		TArray<FString> SearchRoots = GetReplaySearchRoots();
		OutRows.Reserve(OutRows.Num() + SearchRoots.Num());
		for (const FString& Root : SearchRoots)
		{
			OutRows.Add(MakeShared<FJsonValueObject>(MakeReplayRootJson(Root)));
		}
	}

	void AddReplayContainerRows(
		const FString& Root,
		int32 Limit,
		TSet<FString>& SeenPaths,
		TArray<TSharedPtr<FJsonValue>>& OutRows)
	{
		if (!IFileManager::Get().DirectoryExists(*Root) || OutRows.Num() >= Limit)
		{
			return;
		}

		TArray<FString> DirectoryNames;
		IFileManager::Get().FindFiles(DirectoryNames, *(Root / TEXT("*")), false, true);
		DirectoryNames.Sort();

		for (const FString& DirectoryName : DirectoryNames)
		{
			if (OutRows.Num() >= Limit)
			{
				return;
			}

			const FString DirectoryPath = FPaths::Combine(Root, DirectoryName);
			FString Normalized = FPaths::ConvertRelativePathToFull(DirectoryPath);
			FPaths::NormalizeDirectoryName(Normalized);
			if (SeenPaths.Contains(Normalized))
			{
				continue;
			}
			SeenPaths.Add(Normalized);

			TArray<FString> ContainedFiles;
			GatherReplayFiles(DirectoryPath, true, ContainedFiles);
			OutRows.Add(MakeShared<FJsonValueObject>(MakeSavedReplayContainerRow(DirectoryPath, ContainedFiles)));
		}
	}

	void AddReplayFileRows(
		const FString& Root,
		bool bIncludeNestedFiles,
		int32 Limit,
		TSet<FString>& SeenPaths,
		TArray<TSharedPtr<FJsonValue>>& OutRows)
	{
		if (!IFileManager::Get().DirectoryExists(*Root) || OutRows.Num() >= Limit)
		{
			return;
		}

		TArray<FString> Files;
		GatherReplayFiles(Root, bIncludeNestedFiles, Files);

		for (const FString& File : Files)
		{
			if (OutRows.Num() >= Limit)
			{
				return;
			}

			FString Normalized = FPaths::ConvertRelativePathToFull(File);
			FPaths::NormalizeFilename(Normalized);
			if (SeenPaths.Contains(Normalized))
			{
				continue;
			}
			SeenPaths.Add(Normalized);

			OutRows.Add(MakeShared<FJsonValueObject>(MakeSavedReplayFileRow(File)));
		}
	}

	bool IsClassOrSuperClassNamed(const UClass* Class, const TCHAR* ExpectedName)
	{
		for (const UClass* It = Class; It; It = It->GetSuperClass())
		{
			if (It->GetName() == ExpectedName)
			{
				return true;
			}
		}
		return false;
	}

	UClass* FindAnimMixerClass(const TCHAR* ClassPath)
	{
		return FindObject<UClass>(nullptr, ClassPath);
	}

	TSharedPtr<FJsonObject> MakeModuleStatusJson(const TCHAR* ModuleName)
	{
		const FName ModuleFName(ModuleName);
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), ModuleName);
		Json->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
		Json->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(ModuleFName));
		return Json;
	}

	int32 GetReflectedArrayCount(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return -1;
		}

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Object->GetClass(), FName(PropertyName));
		if (!ArrayProperty)
		{
			return -1;
		}

		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	}

	int32 GetReflectedMapCount(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return -1;
		}

		FMapProperty* MapProperty = FindFProperty<FMapProperty>(Object->GetClass(), FName(PropertyName));
		if (!MapProperty)
		{
			return -1;
		}

		FScriptMapHelper Helper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	}

	TArray<UObject*> GetReflectedObjectArray(UObject* Object, const TCHAR* PropertyName)
	{
		TArray<UObject*> Values;
		if (!Object)
		{
			return Values;
		}

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Object->GetClass(), FName(PropertyName));
		const FObjectPropertyBase* InnerObjectProperty = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
		if (!ArrayProperty || !InnerObjectProperty)
		{
			return Values;
		}

		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		Values.Reserve(Helper.Num());
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			UObject* Value = InnerObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index));
			if (Value)
			{
				Values.Add(Value);
			}
		}
		return Values;
	}

	UObject* GetReflectedObjectProperty(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return nullptr;
		}

		FObjectPropertyBase* ObjectProperty = FindFProperty<FObjectPropertyBase>(Object->GetClass(), FName(PropertyName));
		return ObjectProperty ? ObjectProperty->GetObjectPropertyValue_InContainer(Object) : nullptr;
	}

	FString GetReflectedTextProperty(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return FString();
		}

		FTextProperty* TextProperty = FindFProperty<FTextProperty>(Object->GetClass(), FName(PropertyName));
		if (!TextProperty)
		{
			return FString();
		}

		const FText* TextValue = TextProperty->ContainerPtrToValuePtr<FText>(Object);
		return TextValue ? TextValue->ToString() : FString();
	}

	TSharedPtr<FJsonObject> MakeAnimMixerTrackSummary(UMovieSceneTrack* Track)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("available"), Track != nullptr);
		if (Track)
		{
			Json->SetStringField(TEXT("class"), Track->GetClass()->GetName());
			Json->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
			Json->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
		}
		return Json;
	}

	TSharedPtr<FJsonObject> MakeAnimMixerLayerJson(UObject* Layer, int32 LayerIndex)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("layer_index"), LayerIndex);
		Json->SetStringField(TEXT("class"), Layer ? Layer->GetClass()->GetName() : TEXT("<null>"));

		const FString DisplayName = GetReflectedTextProperty(Layer, TEXT("DisplayName"));
		if (DisplayName.IsEmpty())
		{
			Json->SetField(TEXT("display_name"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Json->SetStringField(TEXT("display_name"), DisplayName);
		}

		const int32 SectionCount = GetReflectedArrayCount(Layer, TEXT("Sections"));
		if (SectionCount >= 0)
		{
			Json->SetNumberField(TEXT("section_count"), SectionCount);
		}
		else
		{
			Json->SetField(TEXT("section_count"), MakeShared<FJsonValueNull>());
		}

		UMovieSceneTrack* ChildTrack = Cast<UMovieSceneTrack>(GetReflectedObjectProperty(Layer, TEXT("ChildTrack")));
		Json->SetBoolField(TEXT("has_child_track"), ChildTrack != nullptr);
		Json->SetObjectField(TEXT("child_track"), MakeAnimMixerTrackSummary(ChildTrack));
		return Json;
	}

	TSharedPtr<FJsonObject> MakeAnimMixerTrackJson(
		UMovieSceneTrack* Track,
		const FString& Context,
		const FString& BindingGuid,
		const FString& BindingName,
		bool bIncludeLayers)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("context"), Context);
		if (BindingGuid.IsEmpty())
		{
			Json->SetField(TEXT("binding_guid"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Json->SetStringField(TEXT("binding_guid"), BindingGuid);
		}
		if (BindingName.IsEmpty())
		{
			Json->SetField(TEXT("binding_name"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Json->SetStringField(TEXT("binding_name"), BindingName);
		}

		Json->SetStringField(TEXT("track_class"), Track ? Track->GetClass()->GetName() : TEXT("<null>"));
		Json->SetStringField(TEXT("display_name"), Track ? Track->GetDisplayName().ToString() : FString());
		Json->SetNumberField(TEXT("section_count"), Track ? Track->GetAllSections().Num() : 0);

		const TArray<UObject*> Layers = GetReflectedObjectArray(Track, TEXT("Layers"));
		Json->SetNumberField(TEXT("layer_count"), Layers.Num());
		const int32 ChildTrackCount = GetReflectedMapCount(Track, TEXT("ChildTracks"));
		Json->SetNumberField(TEXT("child_track_count"), ChildTrackCount >= 0 ? ChildTrackCount : 0);

		if (bIncludeLayers)
		{
			TArray<TSharedPtr<FJsonValue>> LayerRows;
			LayerRows.Reserve(Layers.Num());
			for (int32 Index = 0; Index < Layers.Num(); ++Index)
			{
				LayerRows.Add(MakeShared<FJsonValueObject>(MakeAnimMixerLayerJson(Layers[Index], Index)));
			}
			Json->SetArrayField(TEXT("layers"), LayerRows);
		}
		return Json;
	}

	FString ResolveMovieSceneBindingName(UMovieScene* MovieScene, const FGuid& BindingGuid)
	{
		if (!MovieScene)
		{
			return FString();
		}

		if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid))
		{
			return Possessable->GetName();
		}
		if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
		{
			return Spawnable->GetName();
		}
		return FString();
	}

	void AddAnimMixerTrackIfMatched(
		UMovieSceneTrack* Track,
		const FString& Context,
		const FString& BindingGuid,
		const FString& BindingName,
		bool bIncludeLayers,
		TArray<TSharedPtr<FJsonValue>>& OutTracks)
	{
		if (!Track || !IsClassOrSuperClassNamed(Track->GetClass(), TEXT("MovieSceneAnimationMixerTrack")))
		{
			return;
		}

		OutTracks.Add(MakeShared<FJsonValueObject>(
			MakeAnimMixerTrackJson(Track, Context, BindingGuid, BindingName, bIncludeLayers)));
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithLevelSequenceActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("level_sequence"), TEXT("ping"),
		TEXT("Smoke test — returns {status:ok, module:MonolithLevelSequence} when the module is loaded."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::Ping),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("get_replay_status"),
		TEXT("Report editor/PIE replay readiness and the project-local Saved replay/demo folders inspected by Monolith. Read-only; runtime recording/playback controls remain future guarded work."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::GetReplayStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_saved_replays"),
		TEXT("List project-local saved replay/demo containers and optional file metadata from Saved/Demos, Saved/Replays, and Saved/Replay. File bytes are never returned."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListSavedReplays),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return, clamped to 1..500. Default: 100."))
			.Range(TEXT("limit"), 1, 500)
			.Optional(TEXT("include_files"), TEXT("boolean"), TEXT("When true, include bounded replay/demo file metadata rows in addition to top-level replay containers. Default: false."))
			.Optional(TEXT("include_nested_files"), TEXT("boolean"), TEXT("When include_files is true, include files under replay container subdirectories. Default: true."))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("get_saved_replay"),
		TEXT("Inspect one project-local saved replay/demo container or file by Saved-relative path. Metadata only; file bytes are never returned."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::GetSavedReplay),
		FParamSchemaBuilder()
			.Required(TEXT("saved_relative_path"), TEXT("string"), TEXT("Saved-relative path returned by list_saved_replays, such as Demos/MyReplay"))
			.Optional(TEXT("include_files"), TEXT("boolean"), TEXT("For replay containers, include bounded child file metadata. Default: true."))
			.Optional(TEXT("include_nested_files"), TEXT("boolean"), TEXT("When include_files is true, include files recursively under the container. Default: true."))
			.Optional(TEXT("file_limit"), TEXT("integer"), TEXT("Maximum child file rows to return, clamped to 0..500. Default: 100."))
			.Range(TEXT("file_limit"), 0, 500)
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_directors"),
		TEXT("List all Level Sequences that have a Director Blueprint, with director name and function/variable counts. Optional asset_path_filter is a glob pattern (* and ?) matched against ls_path."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListDirectors),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob pattern to filter ls_path (e.g., \"/MyModule/*\")"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("get_director_info"),
		TEXT("Get summary information for a single Level Sequence Director: function counts grouped by kind (user / custom_event / sequencer_endpoint), variable count, event-binding counts (total + resolved), and a sample of up to 10 functions for quick orientation."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::GetDirectorInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Full Level Sequence asset path (e.g., \"/Game/Cinematics/LS_Intro.LS_Intro\")"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_director_functions"),
		TEXT("List a Director's own functions, optionally filtered by kind. Inherited base-class methods and compiler-generated dispatchers are not indexed (own-functions only, matching blueprint_query convention)."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListDirectorFunctions),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Full Level Sequence asset path"))
			.Optional(TEXT("kind"), TEXT("string"), TEXT("Filter: \"user\" | \"custom_event\" | \"sequencer_endpoint\" | \"event\" (alias for custom_event+sequencer_endpoint) | \"all\" (default)"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_event_bindings"),
		TEXT("List all event-track bindings inside one Level Sequence, grouped by binding GUID. Each binding entry describes a Possessable (existing level actor), Spawnable (template-spawned), or master track (no GUID), and lists the sections that fire Director functions."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListEventBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Full Level Sequence asset path"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("find_director_function_callers"),
		TEXT("Cross-sequence reverse lookup: given a Director function name, return every event-track section across the project that fires it, with LS path and binding context. Optional asset_path_filter is a glob (* and ?) to narrow the search."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::FindDirectorFunctionCallers),
		FParamSchemaBuilder()
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Exact function name to search (case-sensitive). Examples: \"Start\", \"SequenceEvent__ENTRYPOINTLS_Foo_DirectorBP_0\""))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob pattern (* and ?) restricting matches to LS paths matching this pattern"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_director_variables"),
		TEXT("List a Director's variables (name + K2-schema-formatted type) in declaration order. Variables come from DirBP->NewVariables and follow the same own-only convention as functions (no inherited base-class properties)."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListDirectorVariables),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Full Level Sequence asset path"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_bindings"),
		TEXT("List ALL bindings inside a Level Sequence regardless of event tracks. UE 5.7 stores modern Spawnables as Possessables inside UMovieScene while their real identity (UMovieSceneSpawnableActorBinding etc.) lives on UMovieSceneSequence::GetBindingReferences(); list_event_bindings sees only event-bound rows and would miss them. Each row reports kind (possessable/spawnable/replaceable/custom), bound class, and — for custom bindings — the exact UCLASS name and pretty label. Optional kind filter narrows the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Full Level Sequence asset path"))
			.Optional(TEXT("kind"), TEXT("string"), TEXT("Filter: possessable | spawnable | replaceable | custom | all (default)"))
			.Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("get_anim_mixer_status"),
		TEXT("Report whether Epic's UE 5.8 Experimental MovieSceneAnimMixer plugin/modules/classes are visible. Reflection-only; UE 5.7 builds never hard-link the optional plugin."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::GetAnimMixerStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("level_sequence"), TEXT("list_anim_mixer_tracks"),
		TEXT("Load one Level Sequence and list reflected Sequencer Anim Mixer tracks/layers when the optional MovieSceneAnimMixer plugin is present. Returns track_count=0 safely on UE 5.7 or when no mixer tracks exist."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelSequenceActions::ListAnimMixerTracks),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full Level Sequence asset path (e.g., \"/Game/Cinematics/LS_Intro.LS_Intro\")"))
			.Optional(TEXT("include_layers"), TEXT("boolean"), TEXT("Include reflected layer rows with section and child-track summaries. Default: true."))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("list_bindings"),
		{ TEXT("possessable"), TEXT("spawnable"), TEXT("custom binding"), TEXT("bound actor"), TEXT("object binding"), TEXT("MovieSceneSpawnableActorBinding") },
		{ TEXT("get_bindings"), TEXT("list_objects"), TEXT("list_possessables"), TEXT("list_spawnables") },
		{ TEXT("what actors are bound in this level sequence"), TEXT("list every possessable and spawnable in the cutscene"), TEXT("show the custom bindings for this sequence") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("list_directors"),
		{ TEXT("director blueprint"), TEXT("sequence directors"), TEXT("cutscene logic"), TEXT("sequencer scripting"), TEXT("find sequences with directors") },
		{ TEXT("list_director_blueprints"), TEXT("find_directors"), TEXT("list_sequence_directors") },
		{ TEXT("which level sequences have a director blueprint"), TEXT("find all cinematics with director scripting"), TEXT("list sequencer directors across the project") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("get_director_info"),
		{ TEXT("director summary"), TEXT("director blueprint overview"), TEXT("function counts"), TEXT("event binding counts"), TEXT("director variables") },
		{ TEXT("describe_director"), TEXT("director_summary"), TEXT("inspect_director") },
		{ TEXT("summarize the director blueprint for this level sequence"), TEXT("how many functions and variables does this sequence director have"), TEXT("show an overview of the cutscene director") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("list_event_bindings"),
		{ TEXT("event track"), TEXT("event section"), TEXT("fires director function"), TEXT("trigger track"), TEXT("FMovieSceneEvent"), TEXT("event key") },
		{ TEXT("list_events"), TEXT("list_event_tracks"), TEXT("get_event_bindings") },
		{ TEXT("which event tracks fire director functions in this sequence"), TEXT("list the event keys that call cutscene logic"), TEXT("show sequencer event track bindings") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("find_director_function_callers"),
		{ TEXT("reverse lookup"), TEXT("who calls this function"), TEXT("cross sequence search"), TEXT("event references"), TEXT("find callers"), TEXT("where is this fired") },
		{ TEXT("find_callers"), TEXT("who_fires_function"), TEXT("find_event_references"), TEXT("find_function_usages") },
		{ TEXT("which level sequences call this director function"), TEXT("find every event track that fires this function across the project"), TEXT("where is this cutscene function triggered") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("level_sequence"), TEXT("list_saved_replays"),
		{ TEXT("demo recording"), TEXT("saved demos"), TEXT("replay files"), TEXT("gameplay recording"), TEXT("Saved Demos folder") },
		{ TEXT("list_demos"), TEXT("list_recordings"), TEXT("get_replays") },
		{ TEXT("list the saved replays on disk"), TEXT("what demo recordings are in the Saved folder"), TEXT("show recorded gameplay replay files") });
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithLevelSequenceActions::Ping(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("module"), TEXT("MonolithLevelSequence"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::GetReplayStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("level_sequence"));
	Result->SetStringField(TEXT("domain"), TEXT("replay_saved_inspection"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("runtime_controls_available"), false);
	Result->SetBoolField(TEXT("saved_replay_listing_available"), true);

	TArray<TSharedPtr<FJsonValue>> Worlds;
	Worlds.Reserve(2);
	const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	Worlds.Add(MakeShared<FJsonValueObject>(MakeWorldJson(TEXT("editor_world"), EditorWorld)));
	Worlds.Add(MakeShared<FJsonValueObject>(MakeWorldJson(TEXT("play_world"), PlayWorld)));
	Result->SetArrayField(TEXT("world_contexts"), Worlds);
	Result->SetBoolField(TEXT("pie_active"), PlayWorld != nullptr);

	TArray<TSharedPtr<FJsonValue>> Roots;
	AddReplayRootStatusRows(Roots);
	Result->SetArrayField(TEXT("replay_roots"), Roots);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Reserve(3);
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("level_sequence.get_replay_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("level_sequence.list_saved_replays")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("level_sequence.get_saved_replay")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> PlannedActions;
	PlannedActions.Reserve(4);
	PlannedActions.Add(MakeShared<FJsonValueString>(TEXT("replay.start_recording")));
	PlannedActions.Add(MakeShared<FJsonValueString>(TEXT("replay.stop_recording")));
	PlannedActions.Add(MakeShared<FJsonValueString>(TEXT("replay.play")));
	PlannedActions.Add(MakeShared<FJsonValueString>(TEXT("replay.delete_replay")));
	Result->SetArrayField(TEXT("planned_guarded_runtime_actions"), PlannedActions);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListSavedReplays(const TSharedPtr<FJsonObject>& Params)
{
	double RequestedLimit = 100.0;
	Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
	const int32 Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, MaxReplayRows);

	bool bIncludeFiles = false;
	Params->TryGetBoolField(TEXT("include_files"), bIncludeFiles);

	bool bIncludeNestedFiles = true;
	Params->TryGetBoolField(TEXT("include_nested_files"), bIncludeNestedFiles);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Limit);
	TSet<FString> SeenPaths;

	for (const FString& Root : GetReplaySearchRoots())
	{
		AddReplayContainerRows(Root, Limit, SeenPaths, Rows);
		if (bIncludeFiles)
		{
			AddReplayFileRows(Root, bIncludeNestedFiles, Limit, SeenPaths, Rows);
		}
		if (Rows.Num() >= Limit)
		{
			break;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Roots;
	AddReplayRootStatusRows(Roots);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("level_sequence"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("include_files"), bIncludeFiles);
	Result->SetBoolField(TEXT("include_nested_files"), bIncludeNestedFiles);
	Result->SetBoolField(TEXT("truncated"), Rows.Num() >= Limit);
	Result->SetArrayField(TEXT("replay_roots"), Roots);
	Result->SetArrayField(TEXT("replays"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::GetSavedReplay(const TSharedPtr<FJsonObject>& Params)
{
	FString SavedRelativePathParam;
	if (!Params->TryGetStringField(TEXT("saved_relative_path"), SavedRelativePathParam))
	{
		return FMonolithActionResult::Error(TEXT("saved_relative_path is required"));
	}

	bool bIncludeFiles = true;
	Params->TryGetBoolField(TEXT("include_files"), bIncludeFiles);

	bool bIncludeNestedFiles = true;
	Params->TryGetBoolField(TEXT("include_nested_files"), bIncludeNestedFiles);

	double FileLimitValue = 100.0;
	Params->TryGetNumberField(TEXT("file_limit"), FileLimitValue);
	const int32 FileLimit = FMath::Clamp(static_cast<int32>(FileLimitValue), 0, MaxSavedReplayFileRows);

	FString ResolvedPath;
	FString ReplayRoot;
	FString Error;
	if (!TryResolveSavedReplayPath(SavedRelativePathParam, ResolvedPath, ReplayRoot, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const bool bIsDirectory = IFileManager::Get().DirectoryExists(*ResolvedPath);

	TArray<FString> Files;
	TSharedPtr<FJsonObject> ReplayRow;
	if (bIsDirectory)
	{
		GatherReplayFiles(ResolvedPath, bIncludeNestedFiles, Files);
		ReplayRow = MakeSavedReplayContainerRow(ResolvedPath, Files);
	}
	else
	{
		ReplayRow = MakeSavedReplayFileRow(ResolvedPath);
	}

	TArray<TSharedPtr<FJsonValue>> FileRows;
	if (bIsDirectory && bIncludeFiles)
	{
		const int32 ReturnedCount = FMath::Min(FileLimit, Files.Num());
		FileRows.Reserve(ReturnedCount);
		for (int32 Index = 0; Index < ReturnedCount; ++Index)
		{
			FileRows.Add(MakeShared<FJsonValueObject>(MakeSavedReplayFileRow(Files[Index])));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("level_sequence"));
	Result->SetStringField(TEXT("domain"), TEXT("replay_saved_inspection"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetStringField(TEXT("saved_relative_path"), SavedRelativePath(ResolvedPath));
	Result->SetStringField(TEXT("replay_root"), SavedRelativePath(ReplayRoot));
	Result->SetObjectField(TEXT("replay"), ReplayRow);
	Result->SetBoolField(TEXT("include_files"), bIncludeFiles);
	Result->SetBoolField(TEXT("include_nested_files"), bIncludeNestedFiles);
	Result->SetNumberField(TEXT("file_limit"), FileLimit);
	Result->SetNumberField(TEXT("returned_file_count"), FileRows.Num());
	Result->SetBoolField(TEXT("files_truncated"), bIsDirectory && bIncludeFiles && Files.Num() > FileRows.Num());
	if (bIsDirectory && bIncludeFiles)
	{
		Result->SetArrayField(TEXT("files"), FileRows);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListDirectors(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	// Optional glob filter; convert * -> %, ? -> _ for SQL LIKE.
	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	FString SQL = TEXT("SELECT ls_path, director_bp_name, function_count, variable_count "
	                   "FROM level_sequence_directors");
	FString LikePattern;
	if (!PathFilter.IsEmpty())
	{
		LikePattern = PathFilter
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"), TEXT("\\%"))
			.Replace(TEXT("_"), TEXT("\\_"))
			.Replace(TEXT("*"), TEXT("%"))
			.Replace(TEXT("?"), TEXT("_"));
		SQL += TEXT(" WHERE ls_path LIKE ? ESCAPE '\\'");
	}
	SQL += TEXT(" ORDER BY ls_path");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_directors SQL"));
	}
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(1, LikePattern);
	}

	TArray<TSharedPtr<FJsonValue>> Directors;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString LsPath, DirName;
		int64 FuncCount = 0, VarCount = 0;
		Stmt.GetColumnValueByIndex(0, LsPath);
		Stmt.GetColumnValueByIndex(1, DirName);
		Stmt.GetColumnValueByIndex(2, FuncCount);
		Stmt.GetColumnValueByIndex(3, VarCount);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("ls_path"), LsPath);
		Row->SetStringField(TEXT("director_bp_name"), DirName);
		Row->SetNumberField(TEXT("function_count"), FuncCount);
		Row->SetNumberField(TEXT("variable_count"), VarCount);
		Directors.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("directors"), Directors);
	Result->SetNumberField(TEXT("count"), Directors.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::GetDirectorInfo(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// 1) Look up director row.
	int64 DirectorId = -1, LsAssetId = -1, FuncCount = 0, VarCount = 0;
	FString DirName;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id, ls_asset_id, director_bp_name, function_count, variable_count FROM level_sequence_directors WHERE ls_path = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare director lookup SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetPath);
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.Destroy();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No Level Sequence Director indexed for path '%s' (the LS may have no Director Blueprint, or the path is wrong — expected the full object path, e.g. '/Module/.../File.File')"),
				*AssetPath));
		}
		Stmt.GetColumnValueByIndex(0, DirectorId);
		Stmt.GetColumnValueByIndex(1, LsAssetId);
		Stmt.GetColumnValueByIndex(2, DirName);
		Stmt.GetColumnValueByIndex(3, FuncCount);
		Stmt.GetColumnValueByIndex(4, VarCount);
		Stmt.Destroy();
	}

	// 2) Function breakdown by kind (one row per kind, count).
	TMap<FString, int64> KindCounts;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT kind, count(*) FROM level_sequence_director_functions "
			"WHERE director_id = ? GROUP BY kind")))
		{
			Stmt.SetBindingValueByIndex(1, DirectorId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Kind;
				int64 Count = 0;
				Stmt.GetColumnValueByIndex(0, Kind);
				Stmt.GetColumnValueByIndex(1, Count);
				KindCounts.Add(Kind, Count);
			}
		}
		Stmt.Destroy();
	}

	// 3) Event-binding counts.
	int64 BindingsTotal = 0, BindingsResolved = 0;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT count(*), "
			"sum(CASE WHEN fires_function_id IS NOT NULL THEN 1 ELSE 0 END) "
			"FROM level_sequence_event_bindings WHERE ls_asset_id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, LsAssetId);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByIndex(0, BindingsTotal);
				Stmt.GetColumnValueByIndex(1, BindingsResolved);
			}
		}
		Stmt.Destroy();
	}

	// 4) Sample of up to 10 functions (user first, then custom_event, then sequencer_endpoint).
	TArray<TSharedPtr<FJsonValue>> SampleFns;
	SampleFns.Reserve(10);
	{
		FSQLitePreparedStatement Stmt;
		// CASE-WHEN gives explicit kind ordering (user before custom_event before sequencer_endpoint).
		if (Stmt.Create(*RawDB, TEXT("SELECT name, kind FROM level_sequence_director_functions "
			"WHERE director_id = ? "
			"ORDER BY CASE kind "
			"  WHEN 'user' THEN 0 "
			"  WHEN 'custom_event' THEN 1 "
			"  WHEN 'sequencer_endpoint' THEN 2 "
			"  ELSE 3 END, name LIMIT 10")))
		{
			Stmt.SetBindingValueByIndex(1, DirectorId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Name, Kind;
				Stmt.GetColumnValueByIndex(0, Name);
				Stmt.GetColumnValueByIndex(1, Kind);

				auto Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), Name);
				Obj->SetStringField(TEXT("kind"), Kind);
				SampleFns.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		Stmt.Destroy();
	}

	auto FunctionBreakdown = MakeShared<FJsonObject>();
	for (const TPair<FString, int64>& Pair : KindCounts)
	{
		FunctionBreakdown->SetNumberField(Pair.Key, Pair.Value);
	}

	auto BindingsObj = MakeShared<FJsonObject>();
	BindingsObj->SetNumberField(TEXT("total"), BindingsTotal);
	BindingsObj->SetNumberField(TEXT("resolved"), BindingsResolved);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ls_path"), AssetPath);
	Result->SetStringField(TEXT("director_bp_name"), DirName);
	Result->SetNumberField(TEXT("function_count"), FuncCount);
	Result->SetObjectField(TEXT("function_breakdown"), FunctionBreakdown);
	Result->SetNumberField(TEXT("variable_count"), VarCount);
	Result->SetObjectField(TEXT("event_bindings"), BindingsObj);
	Result->SetArrayField(TEXT("sample_functions"), SampleFns);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListDirectorFunctions(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString KindFilter;
	Params->TryGetStringField(TEXT("kind"), KindFilter);
	KindFilter = KindFilter.ToLower();

	// Build WHERE clause for kind filter.
	FString WhereKind;
	if (KindFilter.IsEmpty() || KindFilter == TEXT("all"))
	{
		WhereKind = TEXT("");
	}
	else if (KindFilter == TEXT("user") || KindFilter == TEXT("custom_event") || KindFilter == TEXT("sequencer_endpoint"))
	{
		WhereKind = FString::Printf(TEXT(" AND f.kind = '%s'"), *KindFilter);
	}
	else if (KindFilter == TEXT("event"))
	{
		WhereKind = TEXT(" AND f.kind IN ('custom_event', 'sequencer_endpoint')");
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown kind '%s'. Valid: user, custom_event, sequencer_endpoint, event, all"), *KindFilter));
	}

	// Probe the director exists so we can distinguish "no Director / wrong path"
	// from "Director with no functions", and get the function count for Reserve().
	bool bDirectorKnown = false;
	int64 FunctionCount = 0;
	{
		FSQLitePreparedStatement Probe;
		Probe.Create(*RawDB, TEXT("SELECT function_count FROM level_sequence_directors WHERE ls_path = ?"));
		Probe.SetBindingValueByIndex(1, AssetPath);
		if (Probe.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			bDirectorKnown = true;
			Probe.GetColumnValueByIndex(0, FunctionCount);
		}
		Probe.Destroy();
	}
	if (!bDirectorKnown)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Level Sequence Director indexed for path '%s'"), *AssetPath));
	}

	// JOIN against directors so we can resolve by ls_path in one go.
	const FString SQL = FString::Printf(
		TEXT("SELECT f.name, f.kind, f.signature_json "
			 "FROM level_sequence_director_functions f "
			 "JOIN level_sequence_directors d ON f.director_id = d.id "
			 "WHERE d.ls_path = ?%s "
			 "ORDER BY CASE f.kind "
			 "  WHEN 'user' THEN 0 "
			 "  WHEN 'custom_event' THEN 1 "
			 "  WHEN 'sequencer_endpoint' THEN 2 "
			 "  ELSE 3 END, f.name"),
		*WhereKind);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_director_functions SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetPath);

	TArray<TSharedPtr<FJsonValue>> Rows;
	if (WhereKind.IsEmpty() && FunctionCount > 0)
	{
		Rows.Reserve(static_cast<int32>(FunctionCount));
	}
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Name, Kind, SigRaw;
		Stmt.GetColumnValueByIndex(0, Name);
		Stmt.GetColumnValueByIndex(1, Kind);
		Stmt.GetColumnValueByIndex(2, SigRaw);

		// Parse signature_json (stored as a JSON array string) into nested JSON value.
		TSharedPtr<FJsonValue> SigValue;
		if (!SigRaw.IsEmpty())
		{
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SigRaw);
			FJsonSerializer::Deserialize(Reader, SigValue);
		}
		if (!SigValue.IsValid())
		{
			SigValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("kind"), Kind);
		Obj->SetField(TEXT("signature"), SigValue);
		Rows.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ls_path"), AssetPath);
	Result->SetStringField(TEXT("kind_filter"), KindFilter.IsEmpty() ? TEXT("all") : KindFilter);
	Result->SetArrayField(TEXT("functions"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListEventBindings(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// Sanity-check the LS exists in the directors table; helps distinguish
	// "no Director / wrong path" from "Director exists but has no event-tracks".
	bool bDirectorKnown = false;
	{
		FSQLitePreparedStatement Probe;
		Probe.Create(*RawDB, TEXT("SELECT 1 FROM level_sequence_directors WHERE ls_path = ?"));
		Probe.SetBindingValueByIndex(1, AssetPath);
		bDirectorKnown = (Probe.Step() == ESQLitePreparedStatementStepResult::Row);
		Probe.Destroy();
	}
	if (!bDirectorKnown)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Level Sequence Director indexed for path '%s'"), *AssetPath));
	}

	// JOIN bindings against the director (for ls_asset_id) and LEFT JOIN against
	// functions (some bindings may not resolve — fires_function_id is NULL).
	const FString SQL = TEXT(
		"SELECT b.binding_guid, b.binding_name, b.binding_kind, b.bound_class, "
		"       b.section_kind, b.fires_function_name, "
		"       f.kind, f.signature_json "
		"FROM level_sequence_event_bindings b "
		"JOIN level_sequence_directors d ON d.ls_asset_id = b.ls_asset_id "
		"LEFT JOIN level_sequence_director_functions f ON f.id = b.fires_function_id "
		"WHERE d.ls_path = ? "
		"ORDER BY CASE b.binding_kind "
		"  WHEN 'master' THEN 0 "
		"  WHEN 'possessable' THEN 1 "
		"  WHEN 'spawnable' THEN 2 "
		"  ELSE 3 END, b.binding_name, b.id");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_event_bindings SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetPath);

	// Group rows by binding_guid (NULL guid → one master group).
	struct FBindingAccum
	{
		FString Guid;
		FString Name;
		FString Kind;
		FString BoundClass;
		TArray<TSharedPtr<FJsonValue>> Sections;
	};
	TMap<FString, FBindingAccum> Bindings;
	TArray<FString> BindingOrder;   // preserves SQL ORDER BY

	int32 TotalSections = 0;
	int32 ResolvedSections = 0;

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString BGuid, BName, BKind, BClass, SectionKind, FiresName, ResolvedKind, ResolvedSig;
		Stmt.GetColumnValueByIndex(0, BGuid);
		Stmt.GetColumnValueByIndex(1, BName);
		Stmt.GetColumnValueByIndex(2, BKind);
		Stmt.GetColumnValueByIndex(3, BClass);
		Stmt.GetColumnValueByIndex(4, SectionKind);
		Stmt.GetColumnValueByIndex(5, FiresName);
		Stmt.GetColumnValueByIndex(6, ResolvedKind);
		Stmt.GetColumnValueByIndex(7, ResolvedSig);

		// Group key. Master tracks have NULL guid → bucket them under literal "<master>".
		const FString GroupKey = BGuid.IsEmpty() ? FString(TEXT("<master>")) : BGuid;
		FBindingAccum* Acc = Bindings.Find(GroupKey);
		if (!Acc)
		{
			FBindingAccum New;
			New.Guid = BGuid;
			New.Name = BName;
			New.Kind = BKind;
			New.BoundClass = BClass;
			New.Sections.Reserve(4);
			Bindings.Add(GroupKey, MoveTemp(New));
			BindingOrder.Add(GroupKey);
			Acc = Bindings.Find(GroupKey);
		}

		auto SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_kind"), SectionKind);
		if (FiresName.IsEmpty())
		{
			SectionObj->SetField(TEXT("fires_function_name"), MakeShared<FJsonValueNull>());
		}
		else
		{
			SectionObj->SetStringField(TEXT("fires_function_name"), FiresName);
		}

		// Resolved function info (null when fires_function_id was NULL).
		if (!ResolvedKind.IsEmpty())
		{
			TSharedPtr<FJsonValue> SigValue;
			if (!ResolvedSig.IsEmpty())
			{
				TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ResolvedSig);
				FJsonSerializer::Deserialize(Reader, SigValue);
			}
			if (!SigValue.IsValid())
			{
				SigValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
			}

			auto Resolved = MakeShared<FJsonObject>();
			Resolved->SetStringField(TEXT("kind"), ResolvedKind);
			Resolved->SetField(TEXT("signature"), SigValue);
			SectionObj->SetObjectField(TEXT("resolved"), Resolved);
			++ResolvedSections;
		}
		else
		{
			SectionObj->SetField(TEXT("resolved"), MakeShared<FJsonValueNull>());
		}

		Acc->Sections.Add(MakeShared<FJsonValueObject>(SectionObj));
		++TotalSections;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> BindingsArr;
	BindingsArr.Reserve(BindingOrder.Num());
	for (const FString& Key : BindingOrder)
	{
		const FBindingAccum& Acc = Bindings.FindChecked(Key);
		auto BObj = MakeShared<FJsonObject>();

		if (Acc.Guid.IsEmpty())
		{
			BObj->SetField(TEXT("binding_guid"), MakeShared<FJsonValueNull>());
		}
		else
		{
			BObj->SetStringField(TEXT("binding_guid"), Acc.Guid);
		}
		if (Acc.Name.IsEmpty())
		{
			BObj->SetField(TEXT("binding_name"), MakeShared<FJsonValueNull>());
		}
		else
		{
			BObj->SetStringField(TEXT("binding_name"), Acc.Name);
		}
		BObj->SetStringField(TEXT("binding_kind"), Acc.Kind.IsEmpty() ? TEXT("unknown") : Acc.Kind);
		if (Acc.BoundClass.IsEmpty())
		{
			BObj->SetField(TEXT("bound_class"), MakeShared<FJsonValueNull>());
		}
		else
		{
			BObj->SetStringField(TEXT("bound_class"), Acc.BoundClass);
		}
		BObj->SetArrayField(TEXT("sections"), Acc.Sections);
		BObj->SetNumberField(TEXT("section_count"), Acc.Sections.Num());

		BindingsArr.Add(MakeShared<FJsonValueObject>(BObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ls_path"), AssetPath);
	Result->SetArrayField(TEXT("bindings"), BindingsArr);
	Result->SetNumberField(TEXT("binding_count"), BindingsArr.Num());
	Result->SetNumberField(TEXT("section_count"), TotalSections);
	Result->SetNumberField(TEXT("resolved_section_count"), ResolvedSections);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::FindDirectorFunctionCallers(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("function_name is required"));
	}

	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	// Build SQL with optional ls_path filter (glob → LIKE).
	FString PathClause;
	FString LikePattern;
	if (!PathFilter.IsEmpty())
	{
		LikePattern = PathFilter
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"), TEXT("\\%"))
			.Replace(TEXT("_"), TEXT("\\_"))
			.Replace(TEXT("*"), TEXT("%"))
			.Replace(TEXT("?"), TEXT("_"));
		PathClause = TEXT(" AND d.ls_path LIKE ? ESCAPE '\\'");
	}

	const FString SQL = FString::Printf(TEXT(
		"SELECT d.ls_path, b.binding_guid, b.binding_name, b.binding_kind, b.bound_class, "
		"       b.section_kind, f.kind "
		"FROM level_sequence_event_bindings b "
		"JOIN level_sequence_directors d ON d.ls_asset_id = b.ls_asset_id "
		"LEFT JOIN level_sequence_director_functions f ON f.id = b.fires_function_id "
		"WHERE b.fires_function_name = ?%s "
		"ORDER BY d.ls_path, CASE b.binding_kind "
		"  WHEN 'master' THEN 0 "
		"  WHEN 'possessable' THEN 1 "
		"  WHEN 'spawnable' THEN 2 "
		"  ELSE 3 END, b.binding_name, b.id"),
		*PathClause);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_director_function_callers SQL"));
	}
	Stmt.SetBindingValueByIndex(1, FunctionName);
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(2, LikePattern);
	}

	TArray<TSharedPtr<FJsonValue>> Callers;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString LsPath, BGuid, BName, BKind, BClass, SectionKind, ResolvedKind;
		Stmt.GetColumnValueByIndex(0, LsPath);
		Stmt.GetColumnValueByIndex(1, BGuid);
		Stmt.GetColumnValueByIndex(2, BName);
		Stmt.GetColumnValueByIndex(3, BKind);
		Stmt.GetColumnValueByIndex(4, BClass);
		Stmt.GetColumnValueByIndex(5, SectionKind);
		Stmt.GetColumnValueByIndex(6, ResolvedKind);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("ls_path"), LsPath);
		if (BGuid.IsEmpty())  Row->SetField(TEXT("binding_guid"), MakeShared<FJsonValueNull>()); else Row->SetStringField(TEXT("binding_guid"), BGuid);
		if (BName.IsEmpty())  Row->SetField(TEXT("binding_name"), MakeShared<FJsonValueNull>()); else Row->SetStringField(TEXT("binding_name"), BName);
		Row->SetStringField(TEXT("binding_kind"), BKind.IsEmpty() ? TEXT("unknown") : BKind);
		if (BClass.IsEmpty()) Row->SetField(TEXT("bound_class"), MakeShared<FJsonValueNull>()); else Row->SetStringField(TEXT("bound_class"), BClass);
		Row->SetStringField(TEXT("section_kind"), SectionKind);
		Row->SetBoolField(TEXT("resolved"), !ResolvedKind.IsEmpty());

		Callers.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("function_name"), FunctionName);
	if (PathFilter.IsEmpty())
	{
		Result->SetField(TEXT("asset_path_filter"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Result->SetStringField(TEXT("asset_path_filter"), PathFilter);
	}
	Result->SetArrayField(TEXT("callers"), Callers);
	Result->SetNumberField(TEXT("count"), Callers.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListDirectorVariables(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// Probe the director exists so we can distinguish "no Director / wrong path"
	// from "Director with no variables".
	bool bDirectorKnown = false;
	int64 VariableCount = 0;
	{
		FSQLitePreparedStatement Probe;
		Probe.Create(*RawDB, TEXT("SELECT variable_count FROM level_sequence_directors WHERE ls_path = ?"));
		Probe.SetBindingValueByIndex(1, AssetPath);
		if (Probe.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			bDirectorKnown = true;
			Probe.GetColumnValueByIndex(0, VariableCount);
		}
		Probe.Destroy();
	}
	if (!bDirectorKnown)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Level Sequence Director indexed for path '%s'"), *AssetPath));
	}

	// ORDER BY v.id preserves insertion order, which mirrors DirBP->NewVariables
	// declaration order in the editor — more useful than alphabetical.
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, TEXT(
		"SELECT v.name, v.type "
		"FROM level_sequence_director_variables v "
		"JOIN level_sequence_directors d ON v.director_id = d.id "
		"WHERE d.ls_path = ? "
		"ORDER BY v.id")))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_director_variables SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetPath);

	TArray<TSharedPtr<FJsonValue>> Rows;
	if (VariableCount > 0)
	{
		Rows.Reserve(static_cast<int32>(VariableCount));
	}
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Name, Type;
		Stmt.GetColumnValueByIndex(0, Name);
		Stmt.GetColumnValueByIndex(1, Type);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("type"), Type);
		Rows.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ls_path"), AssetPath);
	Result->SetArrayField(TEXT("variables"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListBindings(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!IndexSS || !IndexSS->GetDatabase())
	{
		return FMonolithActionResult::Error(TEXT("Index database not ready"));
	}

	FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw SQLite database not available"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString KindFilter;
	Params->TryGetStringField(TEXT("kind"), KindFilter);
	KindFilter = KindFilter.ToLower();
	const bool bFilterByKind = !KindFilter.IsEmpty() && KindFilter != TEXT("all");

	if (bFilterByKind && KindFilter != TEXT("possessable") && KindFilter != TEXT("spawnable") && KindFilter != TEXT("replaceable") && KindFilter != TEXT("custom"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown kind '%s'. Valid: possessable, spawnable, replaceable, custom, all"), *KindFilter));
	}

	// level_sequence_bindings stores ls_asset_id (the AssetId at index time) but
	// callers query by ls_path. The directors table provides the bridge. For LS
	// without a Director, fall back to looking up assets.path → assets.id.
	int64 LsAssetId = -1;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT ls_asset_id FROM level_sequence_directors WHERE ls_path = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetPath);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByIndex(0, LsAssetId);
			}
			Stmt.Destroy();
		}
	}
	if (LsAssetId <= 0)
	{
		// Fallback for LS without a Director — resolve via core assets table.
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT id FROM assets WHERE path = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetPath);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByIndex(0, LsAssetId);
			}
			Stmt.Destroy();
		}
	}
	if (LsAssetId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Level Sequence indexed for path '%s'"), *AssetPath));
	}

	int64 BindingCount = 0;
	{
		FString CountSQL = TEXT("SELECT COUNT(*) FROM level_sequence_bindings WHERE ls_asset_id = ?");
		if (bFilterByKind)
		{
			CountSQL += TEXT(" AND kind = ?");
		}
		FSQLitePreparedStatement CountStmt;
		if (CountStmt.Create(*RawDB, *CountSQL))
		{
			CountStmt.SetBindingValueByIndex(1, LsAssetId);
			if (bFilterByKind)
			{
				CountStmt.SetBindingValueByIndex(2, KindFilter);
			}
			if (CountStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				CountStmt.GetColumnValueByIndex(0, BindingCount);
			}
			CountStmt.Destroy();
		}
	}

	FString SQL = TEXT(
		"SELECT binding_guid, binding_index, name, kind, bound_class, "
		"       custom_binding_class, custom_binding_pretty, track_count "
		"FROM level_sequence_bindings "
		"WHERE ls_asset_id = ?");
	if (bFilterByKind)
	{
		SQL += TEXT(" AND kind = ?");
	}
	SQL += TEXT(
		" ORDER BY CASE kind "
		"  WHEN 'possessable' THEN 0 "
		"  WHEN 'spawnable' THEN 1 "
		"  WHEN 'replaceable' THEN 2 "
		"  WHEN 'custom' THEN 3 "
		"  ELSE 4 END, name, binding_guid, binding_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_bindings SQL"));
	}
	Stmt.SetBindingValueByIndex(1, LsAssetId);
	if (bFilterByKind)
	{
		Stmt.SetBindingValueByIndex(2, KindFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	if (BindingCount > 0)
	{
		Rows.Reserve(static_cast<int32>(BindingCount));
	}
	TMap<FString, int32> KindCounts;

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString BGuid, Name, Kind, BoundClass, CustomClass, CustomPretty;
		int64 BindingIndex = 0, TrackCount = 0;
		Stmt.GetColumnValueByIndex(0, BGuid);
		Stmt.GetColumnValueByIndex(1, BindingIndex);
		Stmt.GetColumnValueByIndex(2, Name);
		Stmt.GetColumnValueByIndex(3, Kind);
		Stmt.GetColumnValueByIndex(4, BoundClass);
		Stmt.GetColumnValueByIndex(5, CustomClass);
		Stmt.GetColumnValueByIndex(6, CustomPretty);
		Stmt.GetColumnValueByIndex(7, TrackCount);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("binding_guid"), BGuid);
		Obj->SetNumberField(TEXT("binding_index"), static_cast<double>(BindingIndex));
		if (Name.IsEmpty())         Obj->SetField(TEXT("name"), MakeShared<FJsonValueNull>());        else Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("kind"), Kind);
		if (BoundClass.IsEmpty())   Obj->SetField(TEXT("bound_class"), MakeShared<FJsonValueNull>()); else Obj->SetStringField(TEXT("bound_class"), BoundClass);
		if (CustomClass.IsEmpty())  Obj->SetField(TEXT("custom_binding_class"), MakeShared<FJsonValueNull>());  else Obj->SetStringField(TEXT("custom_binding_class"), CustomClass);
		if (CustomPretty.IsEmpty()) Obj->SetField(TEXT("custom_binding_pretty"), MakeShared<FJsonValueNull>()); else Obj->SetStringField(TEXT("custom_binding_pretty"), CustomPretty);
		Obj->SetNumberField(TEXT("track_count"), static_cast<double>(TrackCount));

		Rows.Add(MakeShared<FJsonValueObject>(Obj));
		KindCounts.FindOrAdd(Kind)++;
	}
	Stmt.Destroy();

	auto KindBreakdown = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : KindCounts)
	{
		KindBreakdown->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ls_path"), AssetPath);
	if (bFilterByKind)
	{
		Result->SetStringField(TEXT("kind_filter"), KindFilter);
	}
	else
	{
		Result->SetField(TEXT("kind_filter"), MakeShared<FJsonValueNull>());
	}
	Result->SetArrayField(TEXT("bindings"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetObjectField(TEXT("kind_counts"), KindBreakdown);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::GetAnimMixerStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MovieSceneAnimMixer"));

	auto Modules = MakeShared<FJsonObject>();
	Modules->SetObjectField(TEXT("MovieSceneAnimMixer"), MakeModuleStatusJson(TEXT("MovieSceneAnimMixer")));
	Modules->SetObjectField(TEXT("MovieSceneAnimMixerEditor"), MakeModuleStatusJson(TEXT("MovieSceneAnimMixerEditor")));
	Modules->SetObjectField(TEXT("MovieSceneAnimMixerScripting"), MakeModuleStatusJson(TEXT("MovieSceneAnimMixerScripting")));

	UClass* TrackClass = FindAnimMixerClass(TEXT("/Script/MovieSceneAnimMixer.MovieSceneAnimationMixerTrack"));
	UClass* LayerClass = FindAnimMixerClass(TEXT("/Script/MovieSceneAnimMixer.MovieSceneAnimationMixerLayer"));
	UClass* SectionClass = FindAnimMixerClass(TEXT("/Script/MovieSceneAnimMixer.MovieSceneAnimMixerSection"));
	UClass* TransitionClass = FindAnimMixerClass(TEXT("/Script/MovieSceneAnimMixer.MovieSceneAnimMixerTransition"));

	auto Classes = MakeShared<FJsonObject>();
	Classes->SetBoolField(TEXT("MovieSceneAnimationMixerTrack"), TrackClass != nullptr);
	Classes->SetBoolField(TEXT("MovieSceneAnimationMixerLayer"), LayerClass != nullptr);
	Classes->SetBoolField(TEXT("MovieSceneAnimMixerSection"), SectionClass != nullptr);
	Classes->SetBoolField(TEXT("MovieSceneAnimMixerTransition"), TransitionClass != nullptr);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("level_sequence"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetStringField(TEXT("plugin_name"), TEXT("MovieSceneAnimMixer"));
	Result->SetStringField(TEXT("engine_reference"), TEXT("UE_5.8 Engine/Plugins/Experimental/MovieSceneAnimMixer"));
	Result->SetBoolField(TEXT("hard_dependency"), false);
	Result->SetBoolField(TEXT("plugin_available"), Plugin.IsValid());
	Result->SetBoolField(TEXT("plugin_enabled"), Plugin.IsValid() ? Plugin->IsEnabled() : false);
	Result->SetObjectField(TEXT("modules"), Modules);
	Result->SetObjectField(TEXT("classes"), Classes);
	Result->SetBoolField(TEXT("track_class_loaded"), TrackClass != nullptr);
	Result->SetBoolField(TEXT("layer_class_loaded"), LayerClass != nullptr);
	Result->SetStringField(TEXT("message"), TrackClass
		? TEXT("Sequencer Anim Mixer classes are loaded; list_anim_mixer_tracks can inspect matching Level Sequence tracks.")
		: TEXT("Sequencer Anim Mixer classes are not loaded; UE 5.7-compatible reflection path will return no mixer tracks."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceActions::ListAnimMixerTracks(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	bool bIncludeLayers = true;
	Params->TryGetBoolField(TEXT("include_layers"), bIncludeLayers);

	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *AssetPath);
	if (!Sequence)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load Level Sequence '%s'"), *AssetPath));
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Level Sequence '%s' has no MovieScene"), *AssetPath));
	}

	UClass* TrackClass = FindAnimMixerClass(TEXT("/Script/MovieSceneAnimMixer.MovieSceneAnimationMixerTrack"));
	TArray<TSharedPtr<FJsonValue>> Tracks;
	const UMovieScene* ConstMovieScene = MovieScene;

	int32 MaxTracks = ConstMovieScene->GetTracks().Num();
	for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
	{
		MaxTracks += Binding.GetTracks().Num();
	}
	Tracks.Reserve(MaxTracks);

	for (UMovieSceneTrack* Track : ConstMovieScene->GetTracks())
	{
		AddAnimMixerTrackIfMatched(
			Track,
			TEXT("root_track"),
			FString(),
			FString(),
			bIncludeLayers,
			Tracks);
	}

	for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
	{
		const FGuid BindingObjectGuid = Binding.GetObjectGuid();
		const FString BindingGuid = BindingObjectGuid.ToString(EGuidFormats::Digits);
		const FString BindingName = ResolveMovieSceneBindingName(MovieScene, BindingObjectGuid);
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			AddAnimMixerTrackIfMatched(
				Track,
				TEXT("binding_track"),
				BindingGuid,
				BindingName,
				bIncludeLayers,
				Tracks);
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("include_layers"), bIncludeLayers);
	Result->SetBoolField(TEXT("anim_mixer_track_class_loaded"), TrackClass != nullptr);
	Result->SetArrayField(TEXT("tracks"), Tracks);
	Result->SetNumberField(TEXT("track_count"), Tracks.Num());
	if (!TrackClass)
	{
		Result->SetStringField(TEXT("message"), TEXT("MovieSceneAnimationMixerTrack is not loaded; returning zero reflected tracks on the UE 5.7-compatible path."));
	}
	else if (Tracks.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), TEXT("No Sequencer Anim Mixer tracks found in this Level Sequence."));
	}
	else
	{
		Result->SetStringField(TEXT("message"), TEXT("Sequencer Anim Mixer tracks reflected successfully."));
	}
	return FMonolithActionResult::Success(Result);
}
