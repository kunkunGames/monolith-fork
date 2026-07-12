#include "MonolithBuildArtifactActions.h"

#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MonolithBuildArtifact
{
	static constexpr int32 ErrInvalidParams = -32602;

	static FMonolithActionExecutionPolicy ReadOnlyPolicy()
	{
		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.bDefaulted = false;
		return Policy;
	}

	static FString NormalizeDiskPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return Path;
		}
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	static FString ReadString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FString& DefaultValue = FString())
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return DefaultValue;
		}
		FString Value;
		return Params->TryGetStringField(FieldName, Value) ? Value.TrimStartAndEnd() : DefaultValue;
	}

	static bool ReadBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
	{
		bool Value = DefaultValue;
		return Params.IsValid() && Params->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
	}

	static bool TryReadBoundedInt(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FString& OutError)
	{
		OutValue = DefaultValue;
		OutError.Reset();
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue>* RawValue = Params->Values.Find(FieldName);
		if (!RawValue)
		{
			return true;
		}
		if (!RawValue->IsValid() || (*RawValue)->Type != EJson::Number)
		{
			OutError = FString::Printf(TEXT("%s must be a JSON number."), FieldName);
			return false;
		}

		const double Number = (*RawValue)->AsNumber();
		if (!FMath::IsFinite(Number)
			|| Number != FMath::TruncToDouble(Number)
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue))
		{
			OutError = FString::Printf(
				TEXT("%s must be an integer between %d and %d."),
				FieldName,
				MinValue,
				MaxValue);
			return false;
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	static TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
	{
		TArray<FString> Values;
		if (!Params.IsValid())
		{
			return Values;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Params->TryGetArrayField(FieldName, Array) || !Array)
		{
			return Values;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue))
			{
				StringValue.TrimStartAndEndInline();
				if (!StringValue.IsEmpty())
				{
					Values.Add(StringValue);
				}
			}
		}
		return Values;
	}

	static TSharedPtr<FJsonObject> ErrorData(const FString& Code, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("detail"), Detail);
		return Obj;
	}

	static FString EngineRootFromCurrentProcess()
	{
		FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		FPaths::NormalizeFilename(EngineDir);
		FPaths::CollapseRelativeDirectories(EngineDir);
		FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(EngineDir, TEXT("..")));
		FPaths::NormalizeFilename(Root);
		FPaths::CollapseRelativeDirectories(Root);
		return Root;
	}

	static FString RunUatPathFromEngineRoot(const FString& EngineRoot)
	{
#if PLATFORM_WINDOWS
		const TCHAR* RunUatName = TEXT("RunUAT.bat");
#else
		const TCHAR* RunUatName = TEXT("RunUAT.sh");
#endif
		return NormalizeDiskPath(FPaths::Combine(EngineRoot, TEXT("Engine"), TEXT("Build"), TEXT("BatchFiles"), RunUatName));
	}

	static FString DefaultProjectPath()
	{
		FString ProjectPath = FPaths::GetProjectFilePath();
		if (ProjectPath.IsEmpty())
		{
			ProjectPath = FPaths::Combine(FPaths::ProjectDir(), FString(FApp::GetProjectName()) + TEXT(".uproject"));
		}
		return NormalizeDiskPath(ProjectPath);
	}

	static FString QuoteArg(const FString& Arg)
	{
		FString Escaped = Arg;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString JoinCommandLine(const TArray<FString>& Args)
	{
		FString Joined;
		for (const FString& Arg : Args)
		{
			if (!Joined.IsEmpty())
			{
				Joined.AppendChar(TEXT(' '));
			}
			Joined.Append(Arg.Contains(TEXT(" ")) ? QuoteArg(Arg) : Arg);
		}
		return Joined;
	}

	static bool ContainsCommandLineControlChar(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			if (Ch == TEXT('"') || Ch == TEXT('\'') || Ch == TEXT('&') || Ch == TEXT('|') ||
				Ch == TEXT('<') || Ch == TEXT('>') || Ch == TEXT('^') || Ch == TEXT('%') ||
				Ch == TEXT(';') || Ch == TEXT('`') || Ch == TEXT('$') || Ch == TEXT('\n') ||
				Ch == TEXT('\r') || Ch == TEXT('\t'))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsSafeBuildCookRunToken(const FString& Value)
	{
		if (Value.IsEmpty() || ContainsCommandLineControlChar(Value))
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('.') ||
				Ch == TEXT('+') || Ch == TEXT('/') || Ch == TEXT('\\') || Ch == TEXT(':') ||
				Ch == TEXT('=') || Ch == TEXT(',') || Ch == TEXT(' ')))
			{
				return false;
			}
		}
		return true;
	}

	static bool ValidateBuildCookRunValue(const TCHAR* FieldName, const FString& Value, FString& OutError)
	{
		if (!Value.IsEmpty() && !IsSafeBuildCookRunToken(Value))
		{
			OutError = FString::Printf(TEXT("%s contains characters that are not safe for a guarded UAT command line."), FieldName);
			return false;
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(Value));
		}
		return Rows;
	}

	static bool WriteJsonFile(const FString& Path, const TSharedPtr<FJsonObject>& Object, FString& OutError)
	{
		FString Serialized;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
		{
			OutError = TEXT("Failed to serialize JSON.");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveStringToFile(Serialized, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to write '%s'."), *Path);
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> FileRow(const FString& FilePath, const FString& RootPath)
	{
		const FString NormalizedPath = NormalizeDiskPath(FilePath);
		const FFileStatData Stat = IFileManager::Get().GetStatData(*NormalizedPath);

		FString RelativePath = NormalizedPath;
		if (!RootPath.IsEmpty())
		{
			FPaths::MakePathRelativeTo(RelativePath, *RootPath);
			FPaths::NormalizeFilename(RelativePath);
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), NormalizedPath);
		Obj->SetStringField(TEXT("relative_path"), RelativePath);
		Obj->SetBoolField(TEXT("exists"), Stat.bIsValid);
		Obj->SetBoolField(TEXT("is_directory"), Stat.bIsValid && Stat.bIsDirectory);
		Obj->SetNumberField(TEXT("size_bytes"), Stat.bIsValid && !Stat.bIsDirectory ? static_cast<double>(Stat.FileSize) : 0.0);
		Obj->SetStringField(TEXT("modified_utc"), Stat.bIsValid ? Stat.ModificationTime.ToIso8601() : FString());
		return Obj;
	}

	static void FindFilesUnderDirectory(const FString& RootDir, bool bRecursive, int32 Limit, TArray<FString>& OutFiles)
	{
		TArray<FString> Found;
		if (bRecursive)
		{
			IFileManager::Get().FindFilesRecursive(Found, *RootDir, TEXT("*.*"), true, false, false);
		}
		else
		{
			IFileManager::Get().FindFiles(Found, *FPaths::Combine(RootDir, TEXT("*.*")), true, false);
			for (FString& File : Found)
			{
				File = FPaths::Combine(RootDir, File);
			}
		}
		Found.Sort();
		for (const FString& File : Found)
		{
			if (OutFiles.Num() >= Limit)
			{
				break;
			}
			OutFiles.Add(NormalizeDiskPath(File));
		}
	}

	static TArray<FString> ScreenshotFilesFromParams(const TSharedPtr<FJsonObject>& Params, int32 Limit)
	{
		TArray<FString> Files;
		for (const FString& File : ReadStringArray(Params, TEXT("files")))
		{
			Files.Add(NormalizeDiskPath(File));
			if (Files.Num() >= Limit)
			{
				return Files;
			}
		}

		const FString SourceDir = NormalizeDiskPath(ReadString(Params, TEXT("source_dir")));
		if (!SourceDir.IsEmpty())
		{
			TArray<FString> Found;
			for (const TCHAR* Pattern : { TEXT("*.png"), TEXT("*.jpg"), TEXT("*.jpeg"), TEXT("*.gif") })
			{
				IFileManager::Get().FindFilesRecursive(Found, *SourceDir, Pattern, true, false, false);
			}
			Found.Sort();
			for (const FString& File : Found)
			{
				if (Files.Num() >= Limit)
				{
					break;
				}
				Files.Add(NormalizeDiskPath(File));
			}
		}
		return Files;
	}

	static TArray<TSharedPtr<FJsonValue>> FileRows(const TArray<FString>& Files, const FString& RootPath = FString())
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Files.Num());
		for (const FString& File : Files)
		{
			Rows.Add(MakeShared<FJsonValueObject>(FileRow(File, RootPath)));
		}
		return Rows;
	}

	static FString DefaultNotifyBodyPath()
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("Notify"));
		return NormalizeDiskPath(FPaths::Combine(Dir, FString::Printf(TEXT("discord_screenshot_evidence_%s.json"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")))));
	}

	static FString EscapePowerShellSingleQuoted(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("'"), TEXT("''"));
		return Escaped;
	}
}

void FMonolithBuildArtifactActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithBuildArtifact;

	Registry.RegisterAction(
		TEXT("build"), TEXT("resolve_unreal_engine"),
		TEXT("Resolve the current project's Unreal Engine root from the loaded project context and report UAT/UBT paths without hard-coded engine checkouts."),
		FMonolithActionHandler::CreateStatic(&ResolveUnrealEngine),
		FParamSchemaBuilder()
			.OptionalDiskPath(TEXT("project_path"), TEXT("Optional .uproject path. Defaults to the loaded editor project."))
			.Build(),
		TEXT("Planning"),
		ReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("build"), TEXT("run_buildcookrun"),
		TEXT("Build a guarded UAT BuildCookRun command from structured parameters; dry-run by default, external process launch requires dry_run=false and confirm=true."),
		FMonolithActionHandler::CreateStatic(&RunBuildCookRun),
		FParamSchemaBuilder()
			.OptionalDiskPath(TEXT("project_path"), TEXT("Optional .uproject path. Defaults to the loaded editor project."))
			.Optional(TEXT("platform"), TEXT("string"), TEXT("UAT -platform value."), TEXT("Win64"))
			.Optional(TEXT("client_config"), TEXT("string"), TEXT("UAT -clientconfig value."), TEXT("Development"))
			.Optional(TEXT("server_config"), TEXT("string"), TEXT("Optional UAT -serverconfig value."))
			.Optional(TEXT("target"), TEXT("string"), TEXT("Optional UAT -target value."))
			.Optional(TEXT("map"), TEXT("string"), TEXT("Optional UAT -map value."))
			.Optional(TEXT("custom_config"), TEXT("string"), TEXT("Optional -CustomConfig value such as EOS."))
			.OptionalDiskPath(TEXT("archive_directory"), TEXT("Optional UAT -archivedirectory value."))
			.Optional(TEXT("build"), TEXT("boolean"), TEXT("Add -build."), TEXT("true"))
			.Optional(TEXT("cook"), TEXT("boolean"), TEXT("Add -cook."), TEXT("true"))
			.Optional(TEXT("stage"), TEXT("boolean"), TEXT("Add -stage."), TEXT("true"))
			.Optional(TEXT("pak"), TEXT("boolean"), TEXT("Add -pak."), TEXT("true"))
			.Optional(TEXT("archive"), TEXT("boolean"), TEXT("Add -archive when archive_directory is set."), TEXT("true"))
			.Optional(TEXT("additional_args"), TEXT("array"), TEXT("Extra trusted UAT args appended verbatim."))
			.Optional(TEXT("wait"), TEXT("boolean"), TEXT("When executing, block until UAT exits and return exit code/stdout/stderr."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview command without launching UAT."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false to launch UAT."), TEXT("false"))
			.Build(),
		TEXT("BuildCookRun"),
		ReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("artifact"), TEXT("package_build_outputs"),
		TEXT("Create a structured manifest for a build/archive output directory and optionally write manifest.json with dry-run/confirm guards."),
		FMonolithActionHandler::CreateStatic(&PackageBuildOutputs),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("archive_dir"), TEXT("Build/archive directory to scan."))
			.OptionalDiskPath(TEXT("manifest_path"), TEXT("Manifest output path. Defaults to <archive_dir>/manifest.json."))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Scan recursively."), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum files to include. Must be an integer in 1..50000."), TEXT("5000"))
			.Range(TEXT("limit"), 1, 50000)
			.Optional(TEXT("write_manifest"), TEXT("boolean"), TEXT("Write the manifest file when dry_run=false and confirm=true."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview manifest without writing."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false and write_manifest=true."), TEXT("false"))
			.Build(),
		TEXT("Artifacts"),
		ReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("artifact"), TEXT("mirror_screenshot_evidence"),
		TEXT("Plan or copy screenshot evidence files into a destination evidence directory using explicit files or a source directory scan."),
		FMonolithActionHandler::CreateStatic(&MirrorScreenshotEvidence),
		FParamSchemaBuilder()
			.Optional(TEXT("files"), TEXT("array"), TEXT("Explicit screenshot file paths."))
			.OptionalDiskPath(TEXT("source_dir"), TEXT("Optional directory to scan for png/jpg/jpeg/gif files when files is omitted."))
			.RequiredDiskPath(TEXT("dest_dir"), TEXT("Destination directory for mirrored files."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum files to mirror. Must be an integer in 1..10000."), TEXT("200"))
			.Range(TEXT("limit"), 1, 10000)
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview copy rows without writing."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false to copy files."), TEXT("false"))
			.Build(),
		TEXT("Artifacts"),
		ReadOnlyPolicy());

	Registry.RegisterAction(
		TEXT("notify"), TEXT("discord_screenshot_evidence"),
		TEXT("Build and optionally send a redacted Discord screenshot-evidence notification using a webhook URL read only from an environment variable."),
		FMonolithActionHandler::CreateStatic(&DiscordScreenshotEvidence),
		FParamSchemaBuilder()
			.Required(TEXT("test_name"), TEXT("string"), TEXT("One-line test/evidence summary."))
			.Optional(TEXT("files"), TEXT("array"), TEXT("Screenshot/evidence file paths to list in the notification."))
			.OptionalDiskPath(TEXT("source_dir"), TEXT("Optional evidence directory to scan for screenshots when files is omitted."))
			.Optional(TEXT("webhook_env"), TEXT("string"), TEXT("Environment variable containing the Discord webhook URL."), TEXT("DISCORD_WEBHOOK_URL"))
			.Optional(TEXT("send"), TEXT("boolean"), TEXT("Actually send the text-only notification via PowerShell Invoke-RestMethod."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview payload without sending."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false and send=true."), TEXT("false"))
			.Build(),
		TEXT("Notification"),
		ReadOnlyPolicy());

	Registry.SetActionAnnotations(TEXT("build"), TEXT("run_buildcookrun"), false, true, false, TEXT("Run UAT BuildCookRun"));
	Registry.SetActionAnnotations(TEXT("artifact"), TEXT("package_build_outputs"), false, false, true, TEXT("Package Build Outputs"));
	Registry.SetActionAnnotations(TEXT("artifact"), TEXT("mirror_screenshot_evidence"), false, false, false, TEXT("Mirror Screenshot Evidence"));
	Registry.SetActionAnnotations(TEXT("notify"), TEXT("discord_screenshot_evidence"), false, false, false, TEXT("Discord Screenshot Evidence"));

	Registry.SetActionPlanningMetadata(TEXT("build"), TEXT("resolve_unreal_engine"), TEXT("unreal-build"),
		{ TEXT("Loaded Unreal editor project context or explicit project_path") },
		{ TEXT("engine_root, uat_path, ubt_path, project_path") });
	Registry.SetActionPlanningMetadata(TEXT("build"), TEXT("run_buildcookrun"), TEXT("unreal-build"),
		{ TEXT("Use dry_run=true first; external UAT launch requires confirm=true") },
		{ TEXT("command_line, launched/waited status, optional exit code") },
		{ TEXT("artifact.package_build_outputs") });
	Registry.SetActionPlanningMetadata(TEXT("artifact"), TEXT("package_build_outputs"), TEXT("unreal-build"),
		{ TEXT("Existing archive_dir") },
		{ TEXT("manifest, file rows, optional manifest_path") });
	Registry.SetActionPlanningMetadata(TEXT("artifact"), TEXT("mirror_screenshot_evidence"), TEXT("unreal-build"),
		{ TEXT("Explicit files or source_dir plus dest_dir") },
		{ TEXT("copy plan or copied file rows") },
		{ TEXT("notify.discord_screenshot_evidence") });
	Registry.SetActionPlanningMetadata(TEXT("notify"), TEXT("discord_screenshot_evidence"), TEXT("unreal-build"),
		{ TEXT("Webhook URL must come from webhook_env; dry_run first") },
		{ TEXT("redacted payload preview or send exit code") });
}

FMonolithActionResult FMonolithBuildArtifactActions::ResolveUnrealEngine(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithBuildArtifact;

	const FString ProjectPath = NormalizeDiskPath(ReadString(Params, TEXT("project_path"), DefaultProjectPath()));
	const FString EngineRoot = EngineRootFromCurrentProcess();
	const FString EngineDir = NormalizeDiskPath(FPaths::EngineDir());
	const FString RunUatPath = RunUatPathFromEngineRoot(EngineRoot);
	const FString UbtPath = NormalizeDiskPath(FPaths::Combine(EngineRoot, TEXT("Engine"), TEXT("Binaries"), TEXT("DotNET"), TEXT("UnrealBuildTool"), TEXT("UnrealBuildTool.exe")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("build"));
	Result->SetStringField(TEXT("action"), TEXT("resolve_unreal_engine"));
	Result->SetStringField(TEXT("project_path"), ProjectPath);
	Result->SetBoolField(TEXT("project_exists"), FPaths::FileExists(ProjectPath));
	Result->SetStringField(TEXT("engine_root"), EngineRoot);
	Result->SetStringField(TEXT("engine_dir"), EngineDir);
	Result->SetStringField(TEXT("uat_path"), RunUatPath);
	Result->SetBoolField(TEXT("uat_exists"), FPaths::FileExists(RunUatPath));
	Result->SetStringField(TEXT("ubt_path"), UbtPath);
	Result->SetBoolField(TEXT("ubt_exists"), FPaths::FileExists(UbtPath));
	Result->SetStringField(TEXT("resolution_source"), TEXT("current_editor_engine_context"));
	Result->SetStringField(TEXT("note"), TEXT("This action reports the engine actually running the loaded project; it does not hard-code alternate engine checkouts."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBuildArtifactActions::RunBuildCookRun(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithBuildArtifact;

	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), true);
	const bool bConfirm = ReadBool(Params, TEXT("confirm"), false);
	const bool bWait = ReadBool(Params, TEXT("wait"), false);
	if (!bDryRun && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("run_buildcookrun launches an external UAT process; pass dry_run=true to inspect the command or confirm=true with dry_run=false to launch."),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("confirm_required"), TEXT("dry_run=false requires confirm=true.")));
	}

	const FString ProjectPath = NormalizeDiskPath(ReadString(Params, TEXT("project_path"), DefaultProjectPath()));
	FString ValidationError;
	if (!ValidateBuildCookRunValue(TEXT("project_path"), ProjectPath, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}

	const FString EngineRoot = EngineRootFromCurrentProcess();
	const FString RunUatPath = RunUatPathFromEngineRoot(EngineRoot);
	if (!FPaths::FileExists(RunUatPath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("RunUAT not found at '%s'."), *RunUatPath),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("uat_not_found"), RunUatPath));
	}
	if (!FPaths::FileExists(ProjectPath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Project file not found at '%s'."), *ProjectPath),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("project_not_found"), ProjectPath));
	}

	TArray<FString> Args;
	Args.Add(TEXT("BuildCookRun"));
	Args.Add(FString::Printf(TEXT("-project=%s"), *QuoteArg(ProjectPath)));
	Args.Add(TEXT("-noP4"));
	if (ReadBool(Params, TEXT("build"), true)) { Args.Add(TEXT("-build")); }
	if (ReadBool(Params, TEXT("cook"), true)) { Args.Add(TEXT("-cook")); }
	if (ReadBool(Params, TEXT("stage"), true)) { Args.Add(TEXT("-stage")); }
	if (ReadBool(Params, TEXT("pak"), true)) { Args.Add(TEXT("-pak")); }
	const FString Platform = ReadString(Params, TEXT("platform"), TEXT("Win64"));
	if (!ValidateBuildCookRunValue(TEXT("platform"), Platform, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!Platform.IsEmpty()) { Args.Add(FString::Printf(TEXT("-platform=%s"), *Platform)); }
	const FString ClientConfig = ReadString(Params, TEXT("client_config"), TEXT("Development"));
	if (!ValidateBuildCookRunValue(TEXT("client_config"), ClientConfig, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!ClientConfig.IsEmpty()) { Args.Add(FString::Printf(TEXT("-clientconfig=%s"), *ClientConfig)); }
	const FString ServerConfig = ReadString(Params, TEXT("server_config"));
	if (!ValidateBuildCookRunValue(TEXT("server_config"), ServerConfig, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!ServerConfig.IsEmpty()) { Args.Add(FString::Printf(TEXT("-serverconfig=%s"), *ServerConfig)); }
	const FString Target = ReadString(Params, TEXT("target"));
	if (!ValidateBuildCookRunValue(TEXT("target"), Target, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!Target.IsEmpty()) { Args.Add(FString::Printf(TEXT("-target=%s"), *Target)); }
	const FString Map = ReadString(Params, TEXT("map"));
	if (!ValidateBuildCookRunValue(TEXT("map"), Map, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!Map.IsEmpty()) { Args.Add(FString::Printf(TEXT("-map=%s"), *Map)); }
	const FString CustomConfig = ReadString(Params, TEXT("custom_config"));
	if (!ValidateBuildCookRunValue(TEXT("custom_config"), CustomConfig, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!CustomConfig.IsEmpty()) { Args.Add(FString::Printf(TEXT("-CustomConfig=%s"), *CustomConfig)); }
	const FString ArchiveDirectory = NormalizeDiskPath(ReadString(Params, TEXT("archive_directory")));
	if (!ValidateBuildCookRunValue(TEXT("archive_directory"), ArchiveDirectory, ValidationError))
	{
		return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
	}
	if (!ArchiveDirectory.IsEmpty())
	{
		if (ReadBool(Params, TEXT("archive"), true))
		{
			Args.Add(TEXT("-archive"));
		}
		Args.Add(FString::Printf(TEXT("-archivedirectory=%s"), *QuoteArg(ArchiveDirectory)));
	}
	const TArray<FString> AdditionalArgs = ReadStringArray(Params, TEXT("additional_args"));
	for (const FString& AdditionalArg : AdditionalArgs)
	{
		if (!ValidateBuildCookRunValue(TEXT("additional_args"), AdditionalArg, ValidationError))
		{
			return FMonolithActionResult::Error(ValidationError, ErrInvalidParams)
				.WithErrorData(ErrorData(TEXT("unsafe_buildcookrun_argument"), ValidationError));
		}
	}
	Args.Append(AdditionalArgs);

	const FString ArgLine = JoinCommandLine(Args);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("build"));
	Result->SetStringField(TEXT("action"), TEXT("run_buildcookrun"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetBoolField(TEXT("wait"), bWait);
	Result->SetStringField(TEXT("project_path"), ProjectPath);
	Result->SetStringField(TEXT("engine_root"), EngineRoot);
	Result->SetStringField(TEXT("uat_path"), RunUatPath);
	Result->SetArrayField(TEXT("args"), StringsToJson(Args));
	Result->SetStringField(TEXT("command_line"), FString::Printf(TEXT("%s %s"), *QuoteArg(RunUatPath), *ArgLine));
	Result->SetBoolField(TEXT("launched"), false);

	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("dry_run"));
		return FMonolithActionResult::Success(Result);
	}

	if (bWait)
	{
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		const bool bOk = FPlatformProcess::ExecProcess(*RunUatPath, *ArgLine, &ReturnCode, &StdOut, &StdErr);
		Result->SetBoolField(TEXT("launched"), bOk);
		Result->SetBoolField(TEXT("waited"), true);
		Result->SetNumberField(TEXT("exit_code"), ReturnCode);
		Result->SetStringField(TEXT("stdout_tail"), StdOut.Right(8192));
		Result->SetStringField(TEXT("stderr_tail"), StdErr.Right(8192));
		Result->SetStringField(TEXT("status"), bOk && ReturnCode == 0 ? TEXT("completed") : TEXT("failed"));
		return FMonolithActionResult::Success(Result);
	}

	FProcHandle Proc = FPlatformProcess::CreateProc(*RunUatPath, *ArgLine, true, false, false, nullptr, 0, nullptr, nullptr);
	const bool bLaunched = Proc.IsValid();
	Result->SetBoolField(TEXT("launched"), bLaunched);
	Result->SetBoolField(TEXT("waited"), false);
	Result->SetStringField(TEXT("status"), bLaunched ? TEXT("launched") : TEXT("launch_failed"));
	FPlatformProcess::CloseProc(Proc);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBuildArtifactActions::PackageBuildOutputs(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithBuildArtifact;

	const FString ArchiveDir = NormalizeDiskPath(ReadString(Params, TEXT("archive_dir")));
	if (ArchiveDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*ArchiveDir))
	{
		return FMonolithActionResult::Error(TEXT("archive_dir must be an existing directory."), ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("archive_dir_missing"), ArchiveDir));
	}

	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), true);
	const bool bConfirm = ReadBool(Params, TEXT("confirm"), false);
	const bool bWriteManifest = ReadBool(Params, TEXT("write_manifest"), false);
	if (!bDryRun && bWriteManifest && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("package_build_outputs writes manifest_path; pass dry_run=true to inspect or confirm=true with dry_run=false to write."),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("confirm_required"), TEXT("write_manifest requires confirm=true when dry_run=false.")));
	}

	const bool bRecursive = ReadBool(Params, TEXT("recursive"), true);
	int32 Limit = 0;
	FString LimitError;
	if (!TryReadBoundedInt(Params, TEXT("limit"), 5000, 1, 50000, Limit, LimitError))
	{
		return FMonolithActionResult::Error(LimitError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("limit_invalid"), LimitError));
	}
	FString ManifestPath = NormalizeDiskPath(ReadString(Params, TEXT("manifest_path")));
	if (ManifestPath.IsEmpty())
	{
		ManifestPath = NormalizeDiskPath(FPaths::Combine(ArchiveDir, TEXT("manifest.json")));
	}

	TArray<FString> Files;
	FindFilesUnderDirectory(ArchiveDir, bRecursive, Limit, Files);

	int64 TotalBytes = 0;
	for (const FString& File : Files)
	{
		const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
		if (Stat.bIsValid && !Stat.bIsDirectory)
		{
			TotalBytes += Stat.FileSize;
		}
	}

	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("kind"), TEXT("monolith_build_output_manifest"));
	Manifest->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(TEXT("archive_dir"), ArchiveDir);
	Manifest->SetNumberField(TEXT("file_count"), Files.Num());
	Manifest->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes));
	Manifest->SetArrayField(TEXT("files"), FileRows(Files, ArchiveDir));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("artifact"));
	Result->SetStringField(TEXT("action"), TEXT("package_build_outputs"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetBoolField(TEXT("write_manifest"), bWriteManifest);
	Result->SetStringField(TEXT("archive_dir"), ArchiveDir);
	Result->SetStringField(TEXT("manifest_path"), ManifestPath);
	Result->SetObjectField(TEXT("manifest"), Manifest);
	Result->SetBoolField(TEXT("manifest_written"), false);
	Result->SetStringField(TEXT("status"), bDryRun || !bWriteManifest ? TEXT("planned") : TEXT("writing"));

	if (!bDryRun && bWriteManifest)
	{
		FString Error;
		if (!WriteJsonFile(ManifestPath, Manifest, Error))
		{
			return FMonolithActionResult::Error(Error).WithErrorData(ErrorData(TEXT("manifest_write_failed"), ManifestPath));
		}
		Result->SetBoolField(TEXT("manifest_written"), true);
		Result->SetStringField(TEXT("status"), TEXT("written"));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBuildArtifactActions::MirrorScreenshotEvidence(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithBuildArtifact;

	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), true);
	const bool bConfirm = ReadBool(Params, TEXT("confirm"), false);
	if (!bDryRun && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("mirror_screenshot_evidence copies files; pass dry_run=true to inspect or confirm=true with dry_run=false to copy."),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("confirm_required"), TEXT("dry_run=false requires confirm=true.")));
	}

	const FString DestDir = NormalizeDiskPath(ReadString(Params, TEXT("dest_dir")));
	if (DestDir.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("dest_dir is required."), ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("dest_dir_missing"), TEXT("No destination directory was supplied.")));
	}

	int32 Limit = 0;
	FString LimitError;
	if (!TryReadBoundedInt(Params, TEXT("limit"), 200, 1, 10000, Limit, LimitError))
	{
		return FMonolithActionResult::Error(LimitError, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("limit_invalid"), LimitError));
	}
	const TArray<FString> Files = ScreenshotFilesFromParams(Params, Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Files.Num());
	int32 ExistingCount = 0;
	int32 CopiedCount = 0;
	IFileManager::Get().MakeDirectory(*DestDir, true);
	for (const FString& File : Files)
	{
		const bool bExists = FPaths::FileExists(File);
		if (bExists)
		{
			++ExistingCount;
		}
		const FString DestPath = NormalizeDiskPath(FPaths::Combine(DestDir, FPaths::GetCleanFilename(File)));
		bool bCopied = false;
		FString Error;
		if (!bDryRun && bExists)
		{
			const uint32 CopyResult = IFileManager::Get().Copy(*DestPath, *File, true, true);
			bCopied = CopyResult == COPY_OK;
			if (bCopied)
			{
				++CopiedCount;
			}
			else
			{
				Error = FString::Printf(TEXT("copy_failed_%d"), static_cast<int32>(CopyResult));
			}
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source"), File);
		Row->SetStringField(TEXT("destination"), DestPath);
		Row->SetBoolField(TEXT("source_exists"), bExists);
		Row->SetBoolField(TEXT("would_copy"), bDryRun && bExists);
		Row->SetBoolField(TEXT("copied"), bCopied);
		if (!Error.IsEmpty())
		{
			Row->SetStringField(TEXT("error"), Error);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("artifact"));
	Result->SetStringField(TEXT("action"), TEXT("mirror_screenshot_evidence"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetStringField(TEXT("dest_dir"), DestDir);
	Result->SetNumberField(TEXT("file_count"), Files.Num());
	Result->SetNumberField(TEXT("existing_count"), ExistingCount);
	Result->SetNumberField(TEXT("copied_count"), CopiedCount);
	Result->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("copied"));
	Result->SetArrayField(TEXT("files"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBuildArtifactActions::DiscordScreenshotEvidence(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithBuildArtifact;

	const FString TestName = ReadString(Params, TEXT("test_name"));
	if (TestName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("test_name is required."), ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("test_name_missing"), TEXT("No test/evidence summary was supplied.")));
	}

	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), true);
	const bool bConfirm = ReadBool(Params, TEXT("confirm"), false);
	const bool bSend = ReadBool(Params, TEXT("send"), false);
	if (!bDryRun && bSend && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("discord_screenshot_evidence sends an external notification; pass dry_run=true to inspect or confirm=true with dry_run=false to send."),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("confirm_required"), TEXT("send requires confirm=true when dry_run=false.")));
	}

	const FString WebhookEnv = ReadString(Params, TEXT("webhook_env"), TEXT("DISCORD_WEBHOOK_URL"));
	const FString WebhookValue = FPlatformMisc::GetEnvironmentVariable(*WebhookEnv);
	const bool bWebhookPresent = !WebhookValue.TrimStartAndEnd().IsEmpty();
	const TArray<FString> Files = ScreenshotFilesFromParams(Params, 200);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Monolith screenshot evidence: %s"), *TestName));
	for (const FString& File : Files)
	{
		Lines.Add(FString::Printf(TEXT("- %s"), *File));
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("content"), FString::Join(Lines, TEXT("\n")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("notify"));
	Result->SetStringField(TEXT("action"), TEXT("discord_screenshot_evidence"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetBoolField(TEXT("send"), bSend);
	Result->SetStringField(TEXT("test_name"), TestName);
	Result->SetStringField(TEXT("webhook_env"), WebhookEnv);
	Result->SetBoolField(TEXT("webhook_env_present"), bWebhookPresent);
	Result->SetObjectField(TEXT("payload_preview"), Payload);
	Result->SetArrayField(TEXT("files"), FileRows(Files));
	Result->SetBoolField(TEXT("sent"), false);
	Result->SetStringField(TEXT("status"), bDryRun || !bSend ? TEXT("planned") : TEXT("sending"));

	if (bDryRun || !bSend)
	{
		return FMonolithActionResult::Success(Result);
	}
	if (!bWebhookPresent)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Environment variable '%s' is not set; webhook values are never accepted as action params."), *WebhookEnv),
			ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("webhook_env_missing"), WebhookEnv));
	}

	const FString BodyPath = DefaultNotifyBodyPath();
	FString WriteError;
	if (!WriteJsonFile(BodyPath, Payload, WriteError))
	{
		return FMonolithActionResult::Error(WriteError).WithErrorData(ErrorData(TEXT("payload_write_failed"), BodyPath));
	}

	const FString PsCommand = FString::Printf(
		TEXT("$u=[Environment]::GetEnvironmentVariable('%s'); if([string]::IsNullOrWhiteSpace($u)){exit 2}; Invoke-RestMethod -Uri $u -Method Post -ContentType 'application/json' -InFile '%s' | Out-Null"),
		*EscapePowerShellSingleQuoted(WebhookEnv),
		*EscapePowerShellSingleQuoted(BodyPath));
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = -1;
	const bool bProcessOk = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -Command %s"), *QuoteArg(PsCommand)),
		&ReturnCode,
		&StdOut,
		&StdErr);
	Result->SetBoolField(TEXT("sent"), bProcessOk && ReturnCode == 0);
	Result->SetNumberField(TEXT("exit_code"), ReturnCode);
	Result->SetStringField(TEXT("payload_path"), BodyPath);
	Result->SetStringField(TEXT("stdout_tail"), StdOut.Right(2048));
	Result->SetStringField(TEXT("stderr_tail"), StdErr.Right(2048));
	Result->SetStringField(TEXT("status"), bProcessOk && ReturnCode == 0 ? TEXT("sent") : TEXT("send_failed"));
	return FMonolithActionResult::Success(Result);
}
