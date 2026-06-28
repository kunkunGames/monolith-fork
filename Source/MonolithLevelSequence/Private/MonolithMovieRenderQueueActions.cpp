#include "MonolithMovieRenderQueueActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_MONOLITH_MRQ
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "MoviePipelineEditorBlueprintLibrary.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineSetting.h"
#include "MovieRenderPipelineSettings.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#endif

namespace
{
constexpr TCHAR MovieRenderNamespace[] = TEXT("movie_render");

FMonolithActionResult OptionalDepUnavailable()
{
	return FMonolithActionResult::Error(
		TEXT("Movie Render Pipeline is unavailable. Enable the built-in Movie Render Pipeline plugin and rebuild Monolith."),
		FMonolithJsonUtils::ErrOptionalDepUnavailable);
}

FString NormalizePackagePath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();
	if (!Path.IsEmpty() && !Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
	}
	return Path;
}

#if WITH_MONOLITH_MRQ
UMoviePipelineQueueSubsystem* GetMovieRenderSubsystem()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
}

UMoviePipelineQueue* GetCurrentQueue(FString& OutError)
{
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (!Subsystem)
	{
		OutError = TEXT("Movie Render Queue editor subsystem is not available");
		return nullptr;
	}

	UMoviePipelineQueue* Queue = Subsystem->GetQueue();
	if (!Queue)
	{
		OutError = TEXT("Movie Render Queue is not available");
		return nullptr;
	}

	return Queue;
}

FMonolithActionResult ErrorInvalidParams(const FString& Message)
{
	return FMonolithActionResult::Error(Message, FMonolithJsonUtils::ErrInvalidParams);
}

TSharedPtr<FJsonObject> SettingToJson(const UMoviePipelineSetting* Setting)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Setting)
	{
		return Json;
	}

	UClass* SettingClass = Setting->GetClass();
	Json->SetStringField(TEXT("class_name"), SettingClass ? SettingClass->GetName() : FString());
	Json->SetStringField(TEXT("display_name"), SettingClass ? SettingClass->GetDisplayNameText().ToString() : FString());
	Json->SetBoolField(TEXT("is_enabled"), Setting->IsEnabled());
	return Json;
}

TSharedPtr<FJsonObject> JobToJson(const UMoviePipelineExecutorJob* Job, int32 Index)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Job)
	{
		Json->SetNumberField(TEXT("index"), Index);
		Json->SetBoolField(TEXT("valid"), false);
		return Json;
	}

	Json->SetNumberField(TEXT("index"), Index);
	Json->SetBoolField(TEXT("valid"), true);
	Json->SetStringField(TEXT("sequence"), Job->Sequence.GetAssetPathString());
	Json->SetStringField(TEXT("map"), Job->Map.GetAssetPathString());
	Json->SetStringField(TEXT("author"), Job->Author);
	Json->SetStringField(TEXT("job_name"), Job->JobName);
	Json->SetStringField(TEXT("comment"), Job->Comment);
	Json->SetBoolField(TEXT("is_enabled"), Job->IsEnabled());
	Json->SetBoolField(TEXT("is_consumed"), Job->IsConsumed());
	Json->SetStringField(TEXT("user_data"), Job->UserData);
	Json->SetBoolField(TEXT("is_graph_config"), Job->IsUsingGraphConfiguration());
	Json->SetNumberField(TEXT("status_progress"), Job->GetStatusProgress());
	Json->SetStringField(TEXT("status_message"), Job->GetStatusMessage());

	TArray<TSharedPtr<FJsonValue>> SettingsJson;
	if (const UMoviePipelineConfigBase* Config = Job->GetConfiguration())
	{
		TArray<UMoviePipelineSetting*> Settings = Config->GetUserSettings();
		SettingsJson.Reserve(Settings.Num());
		for (const UMoviePipelineSetting* Setting : Settings)
		{
			SettingsJson.Add(MakeShared<FJsonValueObject>(SettingToJson(Setting)));
		}
	}
	Json->SetArrayField(TEXT("settings"), SettingsJson);

	return Json;
}

TSharedPtr<FJsonObject> QueueToJson(UMoviePipelineQueue* Queue, UMoviePipelineQueueSubsystem* Subsystem)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Queue)
	{
		TArray<TSharedPtr<FJsonValue>> EmptyJobs;
		Json->SetBoolField(TEXT("valid"), false);
		Json->SetNumberField(TEXT("count"), 0);
		Json->SetArrayField(TEXT("jobs"), EmptyJobs);
		return Json;
	}

	TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
	TArray<TSharedPtr<FJsonValue>> JobsJson;
	JobsJson.Reserve(Jobs.Num());
	for (int32 Index = 0; Index < Jobs.Num(); ++Index)
	{
		JobsJson.Add(MakeShared<FJsonValueObject>(JobToJson(Jobs[Index], Index + 1)));
	}

	Json->SetBoolField(TEXT("valid"), true);
	Json->SetNumberField(TEXT("count"), Jobs.Num());
	Json->SetArrayField(TEXT("jobs"), JobsJson);
	Json->SetBoolField(TEXT("is_rendering"), Subsystem ? Subsystem->IsRendering() : false);
	Json->SetBoolField(TEXT("is_dirty"), Queue->IsDirty());
	if (UMoviePipelineQueue* Origin = Queue->GetQueueOrigin())
	{
		Json->SetStringField(TEXT("queue_origin"), Origin->GetPathName());
	}
	return Json;
}

bool GetOneBasedIndex(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 Max, int32& OutZeroBased, FString& OutError)
{
	double OneBasedDouble = 0.0;
	if (Params->HasField(FieldName) && !Params->TryGetNumberField(FieldName, OneBasedDouble))
	{
		OutError = FString::Printf(TEXT("Invalid type for '%s', expected a number."), FieldName);
		return false;
	}
	if (!Params->HasField(FieldName))
	{
		OutError = FString::Printf(TEXT("%s is required"), FieldName);
		return false;
	}
	int32 OneBased = static_cast<int32>(OneBasedDouble);
	if (OneBased < 1 || OneBased > Max)
	{
		OutError = FString::Printf(TEXT("%s %d is out of range (1-%d)"), FieldName, OneBased, Max);
		return false;
	}
	OutZeroBased = OneBased - 1;
	return true;
}

UClass* ResolveExecutorClass(const FString& ExecutorClassPath, FString& OutError)
{
	if (!ExecutorClassPath.IsEmpty())
	{
		UClass* ExplicitClass = LoadClass<UMoviePipelineExecutorBase>(nullptr, *ExecutorClassPath);
		if (!ExplicitClass)
		{
			OutError = FString::Printf(TEXT("executor_class could not be loaded as a UMoviePipelineExecutorBase: %s"), *ExecutorClassPath);
			return nullptr;
		}
		return ExplicitClass;
	}

	const UMovieRenderPipelineProjectSettings* ProjectSettings = GetDefault<UMovieRenderPipelineProjectSettings>();
	if (!ProjectSettings)
	{
		OutError = TEXT("Movie Render Pipeline project settings are unavailable");
		return nullptr;
	}

	UClass* DefaultClass = ProjectSettings->DefaultLocalExecutor.TryLoadClass<UMoviePipelineExecutorBase>();
	if (!DefaultClass)
	{
		OutError = TEXT("DefaultLocalExecutor is not configured in Movie Render Pipeline project settings");
		return nullptr;
	}

	return DefaultClass;
}

FString ObjectPathForPackage(const FString& PackagePath)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	return PackagePath + TEXT(".") + AssetName;
}
#endif
}

void FMonolithMovieRenderQueueActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(MovieRenderNamespace, TEXT("get_queue"),
		TEXT("Return the current Movie Render Queue jobs, dirty state, render state, and per-job settings."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::GetQueue),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("load_queue"),
		TEXT("Load a saved UMoviePipelineQueue asset into the editor Movie Render Queue. Refuses to run while rendering."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::LoadQueue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Queue asset object path or package path, for example /Game/Cinematics/Q_Render.Q_Render or /Game/Cinematics/Q_Render"))
			.Optional(TEXT("prompt_on_dirty"), TEXT("boolean"), TEXT("Whether MRQ should prompt before replacing a dirty queue; unattended runs auto-accept."), TEXT("false"))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("save_queue"),
		TEXT("Save the current editor Movie Render Queue as a UMoviePipelineQueue asset after validating the package path."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::SaveQueue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Destination package path, for example /Game/Cinematics/Q_Render"))
			.Optional(TEXT("allow_overwrite"), TEXT("boolean"), TEXT("Allow replacing an existing queue asset at asset_path."), TEXT("false"))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("add_job"),
		TEXT("Add a Movie Render Queue job for a Level Sequence. Optionally clears existing jobs and overrides map/metadata."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::AddJob),
		FParamSchemaBuilder()
			.Required(TEXT("sequence_path"), TEXT("string"), TEXT("Level Sequence object path, for example /Game/Cinematics/LS_Intro.LS_Intro"))
			.Optional(TEXT("clear_existing"), TEXT("boolean"), TEXT("Delete existing queue jobs before adding this job."), TEXT("false"))
			.Optional(TEXT("map_path"), TEXT("string"), TEXT("Optional world object path override, for example /Game/Maps/M_Main.M_Main"))
			.Optional(TEXT("job_name"), TEXT("string"), TEXT("Optional job display name override."))
			.Optional(TEXT("author"), TEXT("string"), TEXT("Optional author metadata."))
			.Optional(TEXT("comment"), TEXT("string"), TEXT("Optional job comment."))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Whether the new job is enabled."), TEXT("true"))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("duplicate_job"),
		TEXT("Duplicate a queue job by 1-based index and return the duplicated job."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::DuplicateJob),
		FParamSchemaBuilder()
			.Required(TEXT("index"), TEXT("integer"), TEXT("1-based queue job index to duplicate."))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("delete_job"),
		TEXT("Delete a queue job by 1-based index."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::DeleteJob),
		FParamSchemaBuilder()
			.Required(TEXT("index"), TEXT("integer"), TEXT("1-based queue job index to delete."))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("delete_all_jobs"),
		TEXT("Remove all jobs from the current Movie Render Queue. Refuses to run while rendering."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::DeleteAllJobs),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("set_job_index"),
		TEXT("Move a queue job from one 1-based index to another 1-based index."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::SetJobIndex),
		FParamSchemaBuilder()
			.Required(TEXT("index"), TEXT("integer"), TEXT("Current 1-based queue job index."))
			.Required(TEXT("new_index"), TEXT("integer"), TEXT("New 1-based queue job index."))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("list_settings"),
		TEXT("List available UMoviePipelineSetting classes, optionally filtered by class or display name."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::ListSettings),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Case-insensitive filter applied to class name and display name."))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("render_queue"),
		TEXT("Start rendering the current Movie Render Queue with the local MRQ executor. Requires confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::RenderQueue),
		FParamSchemaBuilder()
			.Required(TEXT("confirm"), TEXT("boolean"), TEXT("Must be true to launch a potentially expensive MRQ render."))
			.Optional(TEXT("executor_class"), TEXT("string"), TEXT("Optional UMoviePipelineExecutorBase class path. Defaults to MRQ DefaultLocalExecutor."))
			.Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("is_rendering"),
		TEXT("Return whether Movie Render Queue is currently rendering."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::IsRendering),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("render_progress"),
		TEXT("Return current MRQ executor progress plus queue job status/progress rows."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::RenderProgress),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(MovieRenderNamespace, TEXT("cancel_render"),
		TEXT("Cancel the active Movie Render Queue render. cancel_all=true cancels all remaining jobs."),
		FMonolithActionHandler::CreateStatic(&FMonolithMovieRenderQueueActions::CancelRender),
		FParamSchemaBuilder()
			.Optional(TEXT("cancel_all"), TEXT("boolean"), TEXT("Cancel all remaining jobs instead of only the current job."), TEXT("true"))
			.Build());
}

FMonolithActionResult FMonolithMovieRenderQueueActions::GetQueue(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(QueueToJson(Queue, Subsystem));
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::LoadQueue(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue editor subsystem is not available"));
	}
	if (Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot load a queue while Movie Render Queue is rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	FString AssetPath;
	if (Params->HasField(TEXT("asset_path")) && !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return ErrorInvalidParams(TEXT("Invalid type for 'asset_path', expected a string."));
	}
	if (!Params->HasField(TEXT("asset_path")) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		return ErrorInvalidParams(TEXT("asset_path is required"));
	}

	AssetPath = NormalizePackagePath(AssetPath);
	if (!AssetPath.Contains(TEXT(".")))
	{
		AssetPath = ObjectPathForPackage(AssetPath);
	}

	UMoviePipelineQueue* LoadedQueue = LoadObject<UMoviePipelineQueue>(nullptr, *AssetPath);
	if (!LoadedQueue)
	{
		return ErrorInvalidParams(FString::Printf(TEXT("Movie Render Queue asset not found: %s"), *AssetPath));
	}

	bool bPromptOnDirty = false;
	if (Params->HasField(TEXT("prompt_on_dirty")) && !Params->TryGetBoolField(TEXT("prompt_on_dirty"), bPromptOnDirty))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'prompt_on_dirty\', expected a boolean."));
	}
	if (!Subsystem->LoadQueue(LoadedQueue, bPromptOnDirty))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Movie Render Queue asset: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = QueueToJson(Subsystem->GetQueue(), Subsystem);
	Result->SetStringField(TEXT("loaded_asset"), AssetPath);
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::SaveQueue(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	FString Error;
	UMoviePipelineQueue* CurrentQueue = GetCurrentQueue(Error);
	if (!CurrentQueue)
	{
		return FMonolithActionResult::Error(Error);
	}
	if (CurrentQueue->GetJobs().Num() == 0)
	{
		return ErrorInvalidParams(TEXT("Cannot save an empty Movie Render Queue"));
	}

	FString AssetPath;
	if (Params->HasField(TEXT("asset_path")) && !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return ErrorInvalidParams(TEXT("Invalid type for 'asset_path', expected a string."));
	}
	if (!Params->HasField(TEXT("asset_path")) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		return ErrorInvalidParams(TEXT("asset_path is required"));
	}

	AssetPath = NormalizePackagePath(AssetPath);
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);
	}
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
	{
		return ErrorInvalidParams(ValidationError);
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString ObjectPath = ObjectPathForPackage(AssetPath);

	bool bAllowOverwrite = false;
	if (Params->HasField(TEXT("allow_overwrite")) && !Params->TryGetBoolField(TEXT("allow_overwrite"), bAllowOverwrite))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'allow_overwrite\', expected a boolean."));
	}
	if (!bAllowOverwrite && LoadObject<UObject>(nullptr, *ObjectPath))
	{
		return ErrorInvalidParams(FString::Printf(TEXT("Queue asset already exists at %s; set allow_overwrite=true to replace it"), *ObjectPath));
	}

	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *AssetPath));
	}
	Package->MarkAsFullyLoaded();

	UMoviePipelineQueue* SavedQueue = DuplicateObject<UMoviePipelineQueue>(CurrentQueue, Package, *AssetName);
	if (!SavedQueue)
	{
		return FMonolithActionResult::Error(TEXT("Failed to duplicate the current Movie Render Queue"));
	}

	SavedQueue->SetQueueOrigin(nullptr);
	SavedQueue->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	SavedQueue->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(SavedQueue);

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, SavedQueue, *PackageFilename, SaveArgs);
	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save Movie Render Queue asset: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_path"), AssetPath);
	Result->SetStringField(TEXT("package_filename"), PackageFilename);
	Result->SetNumberField(TEXT("job_count"), CurrentQueue->GetJobs().Num());
	Result->SetBoolField(TEXT("saved"), true);
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::AddJob(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Subsystem && Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot add a Movie Render Queue job while rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	FString SequencePath;
	if (Params->HasField(TEXT("sequence_path")) && !Params->TryGetStringField(TEXT("sequence_path"), SequencePath))
	{
		return ErrorInvalidParams(TEXT("Invalid type for 'sequence_path', expected a string."));
	}
	if (!Params->HasField(TEXT("sequence_path")) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		return ErrorInvalidParams(TEXT("sequence_path is required"));
	}

	SequencePath = NormalizePackagePath(SequencePath);
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
	if (!Sequence)
	{
		return ErrorInvalidParams(FString::Printf(TEXT("Level Sequence not found: %s"), *SequencePath));
	}

	bool bClearExisting = false;
	if (Params->HasField(TEXT("clear_existing")) && !Params->TryGetBoolField(TEXT("clear_existing"), bClearExisting))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'clear_existing\', expected a boolean."));
	}
	if (bClearExisting)
	{
		Queue->Modify();
		Queue->DeleteAllJobs();
	}

	UMoviePipelineExecutorJob* Job = UMoviePipelineEditorBlueprintLibrary::CreateJobFromSequence(Queue, Sequence);
	if (!Job)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Movie Render Queue job for %s"), *SequencePath));
	}
	UMoviePipelineEditorBlueprintLibrary::EnsureJobHasDefaultSettings(Job);

	FString StringValue;
	if (Params->HasField(TEXT("map_path")) && !Params->TryGetStringField(TEXT("map_path"), StringValue))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'map_path\', expected a string."));
	}
	if (Params->HasField(TEXT("map_path")) && !StringValue.TrimStartAndEnd().IsEmpty())
	{
		Job->Map = FSoftObjectPath(NormalizePackagePath(StringValue));
	}
	if (Params->HasField(TEXT("job_name")) && !Params->TryGetStringField(TEXT("job_name"), StringValue))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'job_name\', expected a string."));
	}
	if (Params->HasField(TEXT("job_name")))
	{
		Job->JobName = StringValue;
	}
	if (Params->HasField(TEXT("author")) && !Params->TryGetStringField(TEXT("author"), StringValue))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'author\', expected a string."));
	}
	if (Params->HasField(TEXT("author")))
	{
		Job->Author = StringValue;
	}
	if (Params->HasField(TEXT("comment")) && !Params->TryGetStringField(TEXT("comment"), StringValue))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'comment\', expected a string."));
	}
	if (Params->HasField(TEXT("comment")))
	{
		Job->Comment = StringValue;
	}
	bool bEnabled = true;
	if (Params->HasField(TEXT("enabled")) && !Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'enabled\', expected a boolean."));
	}
	if (Params->HasField(TEXT("enabled")))
	{
		Job->SetIsEnabled(bEnabled);
	}

	TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
	const int32 JobIndex = Jobs.IndexOfByKey(Job) + 1;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("job"), JobToJson(Job, JobIndex));
	Result->SetNumberField(TEXT("queue_count"), Jobs.Num());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::DuplicateJob(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (Subsystem && Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot delete a Movie Render Queue job while rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
	int32 Index = INDEX_NONE;
	if (!GetOneBasedIndex(Params, TEXT("index"), Jobs.Num(), Index, Error))
	{
		return ErrorInvalidParams(Error);
	}

	UMoviePipelineExecutorJob* DuplicatedJob = Queue->DuplicateJob(Jobs[Index]);
	if (!DuplicatedJob)
	{
		return FMonolithActionResult::Error(TEXT("Failed to duplicate Movie Render Queue job"));
	}

	TArray<UMoviePipelineExecutorJob*> UpdatedJobs = Queue->GetJobs();
	const int32 NewIndex = UpdatedJobs.IndexOfByKey(DuplicatedJob) + 1;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("job"), JobToJson(DuplicatedJob, NewIndex));
	Result->SetNumberField(TEXT("queue_count"), UpdatedJobs.Num());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::DeleteJob(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (Subsystem && Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot delete a Movie Render Queue job while rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
	int32 Index = INDEX_NONE;
	if (!GetOneBasedIndex(Params, TEXT("index"), Jobs.Num(), Index, Error))
	{
		return ErrorInvalidParams(Error);
	}

	TSharedPtr<FJsonObject> Deleted = JobToJson(Jobs[Index], Index + 1);
	Queue->DeleteJob(Jobs[Index]);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("deleted"), true);
	Result->SetObjectField(TEXT("deleted_job"), Deleted);
	Result->SetNumberField(TEXT("queue_count"), Queue->GetJobs().Num());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::DeleteAllJobs(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Subsystem && Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot delete Movie Render Queue jobs while rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	const int32 RemovedCount = Queue->GetJobs().Num();
	Queue->DeleteAllJobs();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("deleted"), true);
	Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	Result->SetNumberField(TEXT("queue_count"), Queue->GetJobs().Num());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::SetJobIndex(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	FString Error;
	UMoviePipelineQueue* Queue = GetCurrentQueue(Error);
	if (!Queue)
	{
		return FMonolithActionResult::Error(Error);
	}
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (Subsystem && Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Cannot move a Movie Render Queue job while rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
	int32 Index = INDEX_NONE;
	int32 NewIndex = INDEX_NONE;
	if (!GetOneBasedIndex(Params, TEXT("index"), Jobs.Num(), Index, Error))
	{
		return ErrorInvalidParams(Error);
	}
	if (!GetOneBasedIndex(Params, TEXT("new_index"), Jobs.Num(), NewIndex, Error))
	{
		return ErrorInvalidParams(Error);
	}

	Queue->SetJobIndex(Jobs[Index], NewIndex);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("moved"), true);
	Result->SetNumberField(TEXT("from_index"), Index + 1);
	Result->SetNumberField(TEXT("to_index"), NewIndex + 1);
	Result->SetObjectField(TEXT("queue"), QueueToJson(Queue, GetMovieRenderSubsystem()));
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::ListSettings(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	FString Filter;
	if (Params->HasField(TEXT("filter")) && !Params->TryGetStringField(TEXT("filter"), Filter))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'filter\', expected a string."));
	}

	TArray<TSharedPtr<FJsonValue>> Settings;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(UMoviePipelineSetting::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		const FString ClassName = Class->GetName();
		const FString DisplayName = Class->GetDisplayNameText().ToString();
		if (!Filter.IsEmpty()
			&& !ClassName.Contains(Filter, ESearchCase::IgnoreCase)
			&& !DisplayName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_name"), ClassName);
		Row->SetStringField(TEXT("display_name"), DisplayName);
		Row->SetStringField(TEXT("path_name"), Class->GetPathName());
		Settings.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("settings"), Settings);
	Result->SetNumberField(TEXT("count"), Settings.Num());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::RenderQueue(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	bool bConfirm = false;
	if (Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'confirm\', expected a boolean."));
	}
	if (!bConfirm)
	{
		return ErrorInvalidParams(TEXT("confirm=true is required to launch a Movie Render Queue render"));
	}

	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue editor subsystem is not available"));
	}
	if (Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue is already rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	UMoviePipelineQueue* Queue = Subsystem->GetQueue();
	if (!Queue || Queue->GetJobs().Num() == 0)
	{
		return ErrorInvalidParams(TEXT("Movie Render Queue is empty"));
	}

	FString ExecutorClassPath;
	if (Params->HasField(TEXT("executor_class")) && !Params->TryGetStringField(TEXT("executor_class"), ExecutorClassPath))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'executor_class\', expected a string."));
	}
	FString Error;
	UClass* ExecutorClass = ResolveExecutorClass(ExecutorClassPath, Error);
	if (!ExecutorClass)
	{
		return ErrorInvalidParams(Error);
	}

	UMoviePipelineExecutorBase* Executor = Subsystem->RenderQueueWithExecutor(ExecutorClass);
	if (!Executor)
	{
		return FMonolithActionResult::Error(TEXT("Failed to start Movie Render Queue executor"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("started"), true);
	Result->SetNumberField(TEXT("job_count"), Queue->GetJobs().Num());
	Result->SetStringField(TEXT("executor_class"), ExecutorClass->GetPathName());
	Result->SetBoolField(TEXT("is_rendering"), Subsystem->IsRendering());
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::IsRendering(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_rendering"), Subsystem ? Subsystem->IsRendering() : false);
	if (Subsystem && Subsystem->GetActiveExecutor())
	{
		Result->SetStringField(TEXT("executor_class"), Subsystem->GetActiveExecutor()->GetClass()->GetPathName());
	}
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::RenderProgress(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue editor subsystem is not available"));
	}

	UMoviePipelineExecutorBase* Executor = Subsystem->GetActiveExecutor();
	TSharedPtr<FJsonObject> Result = QueueToJson(Subsystem->GetQueue(), Subsystem);
	Result->SetBoolField(TEXT("is_rendering"), Subsystem->IsRendering());
	if (Executor)
	{
		Result->SetStringField(TEXT("executor_class"), Executor->GetClass()->GetPathName());
		Result->SetNumberField(TEXT("progress"), Executor->GetStatusProgress());
		Result->SetStringField(TEXT("status_message"), Executor->GetStatusMessage());
	}
	else
	{
		Result->SetNumberField(TEXT("progress"), 0.0);
		Result->SetStringField(TEXT("status_message"), FString());
	}
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithMovieRenderQueueActions::CancelRender(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_MONOLITH_MRQ
	return OptionalDepUnavailable();
#else
	UMoviePipelineQueueSubsystem* Subsystem = GetMovieRenderSubsystem();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue editor subsystem is not available"));
	}
	if (!Subsystem->IsRendering())
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue is not rendering"), FMonolithJsonUtils::ErrInvalidRequest);
	}

	UMoviePipelineExecutorBase* Executor = Subsystem->GetActiveExecutor();
	if (!Executor)
	{
		return FMonolithActionResult::Error(TEXT("Movie Render Queue has no active executor"));
	}

	bool bCancelAll = true;
	if (Params->HasField(TEXT("cancel_all")) && !Params->TryGetBoolField(TEXT("cancel_all"), bCancelAll))
	{
		return ErrorInvalidParams(TEXT("Invalid type for \'cancel_all\', expected a boolean."));
	}
	if (bCancelAll)
	{
		Executor->CancelAllJobs();
	}
	else
	{
		Executor->CancelCurrentJob();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("cancel_requested"), true);
	Result->SetBoolField(TEXT("cancel_all"), bCancelAll);
	Result->SetStringField(TEXT("executor_class"), Executor->GetClass()->GetPathName());
	Result->SetBoolField(TEXT("is_rendering"), Subsystem->IsRendering());
	return FMonolithActionResult::Success(Result);
#endif
}
