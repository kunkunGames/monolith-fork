#include "MonolithModelGenActions.h"
#include "MonolithMeshTechArtActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"

#include "Engine/StaticMesh.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
namespace MonolithModelGen
{
	static FString HashString(const FString& Value)
	{
		return Value.IsEmpty() ? TEXT("") : FMD5::HashAnsiString(*Value);
	}

	static FString RootDir()
	{
		return FPaths::ProjectSavedDir() / TEXT("Monolith/GeneratedModels");
	}

	static FString MakeJobId()
	{
		return FString::Printf(TEXT("model_%s_%s"),
			*FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S")),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	static FString JobDir(const FString& JobId)
	{
		return RootDir() / JobId;
	}

	static FString ManifestPath(const FString& JobId)
	{
		return JobDir(JobId) / TEXT("manifest.json");
	}

	static bool ValidateJobId(const FString& JobId, FString& OutError)
	{
		if (JobId.IsEmpty())
		{
			OutError = TEXT("job_id is required");
			return false;
		}
		if (JobId.Contains(TEXT(".")) || JobId.Contains(TEXT("/")) || JobId.Contains(TEXT("\\")))
		{
			OutError = TEXT("job_id may not contain path separators or dots");
			return false;
		}
		for (TCHAR Ch : JobId)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TCHAR('_') && Ch != TCHAR('-'))
			{
				OutError = TEXT("job_id may contain only letters, numbers, underscore, and hyphen");
				return false;
			}
		}
		return true;
	}

	static FString SanitizeAssetName(const FString& Input)
	{
		FString Sanitized = Input.Left(64);
		const FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`+=\t\r\n");
		for (int32 Index = 0; Index < InvalidChars.Len(); ++Index)
		{
			const FString InvalidChar = InvalidChars.Mid(Index, 1);
			Sanitized = Sanitized.Replace(*InvalidChar, TEXT("_"));
		}
		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}
		Sanitized.TrimStartAndEndInline();
		while (Sanitized.StartsWith(TEXT("_")))
		{
			Sanitized.RightChopInline(1);
		}
		while (Sanitized.EndsWith(TEXT("_")))
		{
			Sanitized.LeftChopInline(1);
		}
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("GeneratedModel");
		}
		if (FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Generated_") + Sanitized;
		}
		if (!Sanitized.StartsWith(TEXT("SM_")))
		{
			Sanitized = TEXT("SM_") + Sanitized;
		}
		return Sanitized;
	}

	static void AddStringArray(TSharedPtr<FJsonObject> Obj, const FString& Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Obj->SetArrayField(Field, JsonValues);
	}

	static bool IsSupportedModelFile(const FString& Path)
	{
		return Path.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase)
			|| Path.EndsWith(TEXT(".obj"), ESearchCase::IgnoreCase)
			|| Path.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase)
			|| Path.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase);
	}

	static bool WriteJsonFile(const FString& Path, const TSharedPtr<FJsonObject>& Obj, FString& OutError)
	{
		FString Json;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			OutError = TEXT("Failed to serialize JSON");
			return false;
		}
		if (!FFileHelper::SaveStringToFile(Json, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to write file: %s"), *Path);
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path, FString& OutError)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to read file: %s"), *Path);
			return nullptr;
		}

		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse JSON file: %s"), *Path);
			return nullptr;
		}
		return Obj;
	}

	static bool WriteDeterministicObj(const FString& Prompt, const FString& OutputPath, FString& OutError)
	{
		const uint32 H = GetTypeHash(Prompt);
		const float Width = 80.0f + static_cast<float>(H & 0x1f);
		const float Depth = 80.0f + static_cast<float>((H >> 5) & 0x1f);
		const float Height = 90.0f + static_cast<float>((H >> 10) & 0x3f);

		FString Obj;
		Obj += TEXT("# Monolith deterministic generated model\n");
		Obj += TEXT("o MonolithGeneratedModel\n");
		Obj += FString::Printf(TEXT("v %.3f %.3f 0.000\n"), -Width, -Depth);
		Obj += FString::Printf(TEXT("v %.3f %.3f 0.000\n"), Width, -Depth);
		Obj += FString::Printf(TEXT("v %.3f %.3f 0.000\n"), Width, Depth);
		Obj += FString::Printf(TEXT("v %.3f %.3f 0.000\n"), -Width, Depth);
		Obj += FString::Printf(TEXT("v 0.000 0.000 %.3f\n"), Height);
		Obj += TEXT("vn 0.000 0.000 -1.000\n");
		Obj += TEXT("f 1 4 3 2\n");
		Obj += TEXT("f 1 2 5\n");
		Obj += TEXT("f 2 3 5\n");
		Obj += TEXT("f 3 4 5\n");
		Obj += TEXT("f 4 1 5\n");

		const FString Dir = FPaths::GetPath(OutputPath);
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			IFileManager::Get().MakeDirectory(*Dir, true);
		}
		if (!FFileHelper::SaveStringToFile(Obj, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to write OBJ file: %s"), *OutputPath);
			return false;
		}
		return true;
	}

	static FString ObjectPathFromAssetPath(const FString& AssetPath)
	{
		if (AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		return AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	}

	static TSharedPtr<FJsonObject> BuildProvenance(
		const FString& Provider,
		const FString& Model,
		const FString& Source,
		const FString& JobId,
		const FString& Prompt,
		const FString& SourceImageHash,
		const FString& Format,
		int64 FileSizeBytes)
	{
		TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("kind"), TEXT("model"));
		Provenance->SetStringField(TEXT("provider"), Provider);
		Provenance->SetStringField(TEXT("model"), Model);
		Provenance->SetStringField(TEXT("source"), Source);
		Provenance->SetStringField(TEXT("job_id"), JobId);
		Provenance->SetStringField(TEXT("prompt_hash"), HashString(Prompt));
		Provenance->SetStringField(TEXT("source_image_hash"), SourceImageHash);
		Provenance->SetStringField(TEXT("prompt_redacted"), TEXT("true"));
		Provenance->SetStringField(TEXT("format"), Format);
		Provenance->SetStringField(TEXT("file_size_bytes"), FString::Printf(TEXT("%lld"), FileSizeBytes));
		Provenance->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
		return Provenance;
	}

	static bool ApplyProvenance(UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Provenance, bool bSave, FString& OutError)
	{
		if (!Mesh)
		{
			OutError = TEXT("Null StaticMesh");
			return false;
		}

#if WITH_METADATA
		UPackage* Package = Mesh->GetOutermost();
		FMetaData& MetaData = Package->GetMetaData();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Provenance->Values)
		{
			FString Value;
			if (Pair.Value.IsValid())
			{
				if (!Pair.Value->TryGetString(Value))
				{
					Value = Pair.Value->AsString();
				}
			}
			MetaData.SetValue(Mesh, *FString::Printf(TEXT("Monolith.Generated.%s"), *Pair.Key), *Value);
		}
		Package->MarkPackageDirty();

		if (bSave)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Mesh, *PackageFilename, SaveArgs))
			{
				OutError = FString::Printf(TEXT("Failed to save StaticMesh package: %s"), *Package->GetName());
				return false;
			}
		}
		return true;
#else
		OutError = TEXT("Asset metadata is unavailable in this build");
		return false;
#endif
	}

	static bool ValidateDestinationFolder(const FString& Destination, FString& OutError)
	{
		if (Destination.IsEmpty())
		{
			OutError = TEXT("'destination' is required");
			return false;
		}
		const FString Probe = Destination.EndsWith(TEXT("/")) ? Destination + TEXT("__Probe") : Destination / TEXT("__Probe");
		OutError = MonolithCore::ValidatePackagePath(Probe);
		return OutError.IsEmpty();
	}
}



void FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("modelgen"), TEXT("list_model_generation_providers"),
		TEXT("List Monolith-native generated-model provider boundaries. Remote generation is caller-owned; Monolith imports local artifacts."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::ListModelGenerationProviders),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("submit_generated_model_job"),
		TEXT("Submit a local deterministic text-to-StaticMesh placeholder job. Writes a completed OBJ job under Project/Saved/Monolith/GeneratedModels."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::SubmitGeneratedModelJob),
		FParamSchemaBuilder()
			.Required(TEXT("prompt"), TEXT("string"), TEXT("Text prompt. Stored only as prompt_hash in job/provenance data."))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("Only local_deterministic is supported."), TEXT("local_deterministic"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("Only monolith/local-obj-v1 is supported."), TEXT("monolith/local-obj-v1"))
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional suggested mesh asset name for later import."))
			.Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("get_generated_model_job"),
		TEXT("Read a generated model job manifest by job_id."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::GetGeneratedModelJob),
		FParamSchemaBuilder()
			.Required(TEXT("job_id"), TEXT("string"), TEXT("Generated model job id"))
			.Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("cancel_generated_model_job"),
		TEXT("Cancel a generated model job if it has not already completed. Local deterministic jobs complete immediately."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::CancelGeneratedModelJob),
		FParamSchemaBuilder()
			.Required(TEXT("job_id"), TEXT("string"), TEXT("Generated model job id"))
			.Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("download_generated_model_result"),
		TEXT("Resolve the local artifact path for a completed generated model job. No network download is performed."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::DownloadGeneratedModelResult),
		FParamSchemaBuilder()
			.Required(TEXT("job_id"), TEXT("string"), TEXT("Generated model job id"))
			.Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("import_generated_model"),
		TEXT("Import a completed generated model job or caller-supplied FBX/OBJ/GLB/GLTF file as StaticMesh assets and attach redacted provenance."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::ImportGeneratedModel),
		FParamSchemaBuilder()
			.Optional(TEXT("job_id"), TEXT("string"), TEXT("Completed local generated model job id"))
			.Optional(TEXT("file_path"), TEXT("string"), TEXT("Caller-supplied local FBX/OBJ/GLB/GLTF file. Used when job_id is absent."))
			.Required(TEXT("destination"), TEXT("string"), TEXT("Destination content folder, e.g. /Game/GeneratedModels"))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("Provider id for caller-supplied files."), TEXT("external"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("Model id for caller-supplied files."), TEXT("unknown"))
			.Optional(TEXT("prompt"), TEXT("string"), TEXT("Prompt for caller-supplied files. Stored only as hash."))
			.Optional(TEXT("source_image_hash"), TEXT("string"), TEXT("Hash of external reference image input, if any."))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("Forwarded to mesh.import_mesh"), TEXT("false"))
			.Optional(TEXT("material_import"), TEXT("string"), TEXT("create_new, find_existing, or skip"), TEXT("create_new"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported StaticMesh packages after provenance is written"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("modelgen"), TEXT("get_generated_model_provenance"),
		TEXT("Read Monolith generation provenance metadata from a StaticMesh asset."),
		FMonolithActionHandler::CreateStatic(&FMonolithModelGenActions::GetGeneratedModelProvenance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StaticMesh package path or object path"))
			.Build());
}


FMonolithActionResult FMonolithModelGenActions::ListModelGenerationProviders(const TSharedPtr<FJsonObject>&)
{
	TArray<TSharedPtr<FJsonValue>> Providers;
	Providers.Reserve(2);

	TSharedPtr<FJsonObject> Local = MakeShared<FJsonObject>();
	Local->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
	Local->SetStringField(TEXT("model"), TEXT("monolith/local-obj-v1"));
	Local->SetBoolField(TEXT("available"), true);
	Local->SetBoolField(TEXT("network_required"), false);
	Local->SetStringField(TEXT("output_format"), TEXT("obj"));
	MonolithModelGen::AddStringArray(Local, TEXT("input_modes"), { TEXT("text") });
	Providers.Add(MakeShared<FJsonValueObject>(Local));

	TSharedPtr<FJsonObject> External = MakeShared<FJsonObject>();
	External->SetStringField(TEXT("provider"), TEXT("external"));
	External->SetStringField(TEXT("model"), TEXT("caller_supplied"));
	External->SetBoolField(TEXT("available"), true);
	External->SetBoolField(TEXT("network_required"), false);
	External->SetStringField(TEXT("boundary_action"), TEXT("modelgen.import_generated_model"));
	External->SetStringField(TEXT("secret_policy"), TEXT("Monolith does not read or store provider credentials for generated model imports."));
	MonolithModelGen::AddStringArray(External, TEXT("formats"), { TEXT("fbx"), TEXT("obj"), TEXT("glb"), TEXT("gltf") });
	Providers.Add(MakeShared<FJsonValueObject>(External));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("providers"), Providers);
	Result->SetStringField(TEXT("default_provider"), TEXT("local_deterministic"));
	Result->SetStringField(TEXT("default_model"), TEXT("monolith/local-obj-v1"));
	Result->SetStringField(TEXT("job_root"), MonolithModelGen::RootDir());
	return FMonolithActionResult::Success(Result);
}
FMonolithActionResult FMonolithModelGenActions::SubmitGeneratedModelJob(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString Prompt;
	if (!Params->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: prompt"), -32602);
	}

	FString Provider;
	if (!Params->TryGetStringField(TEXT("provider"), Provider) || Provider.IsEmpty())
	{
		Provider = TEXT("local_deterministic");
	}
	FString Model;
	if (!Params->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
	{
		Model = TEXT("monolith/local-obj-v1");
	}
	if (Provider != TEXT("local_deterministic") || Model != TEXT("monolith/local-obj-v1"))
	{
		return FMonolithActionResult::Error(
			TEXT("Only provider='local_deterministic' and model='monolith/local-obj-v1' are supported for Monolith-native job submission. Use modelgen.import_generated_model for external artifacts."),
			-32602);
	}

	const FString JobId = MonolithModelGen::MakeJobId();
	const FString Dir = MonolithModelGen::JobDir(JobId);
	IFileManager::Get().MakeDirectory(*Dir, true);

	FString AssetName;
	if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		AssetName = MonolithModelGen::SanitizeAssetName(Prompt.Left(48));
	}
	else
	{
		AssetName = MonolithModelGen::SanitizeAssetName(AssetName);
	}

	const FString ObjPath = Dir / FString::Printf(TEXT("%s.obj"), *AssetName);
	FString Error;
	if (!MonolithModelGen::WriteDeterministicObj(Prompt, ObjPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}

	const int64 FileSize = IFileManager::Get().FileSize(*ObjPath);
	TSharedPtr<FJsonObject> Manifest = MonolithModelGen::BuildProvenance(
		Provider, Model, TEXT("local_deterministic"), JobId, Prompt, TEXT(""), TEXT("obj"), FileSize);
	Manifest->SetStringField(TEXT("job_id"), JobId);
	Manifest->SetStringField(TEXT("status"), TEXT("completed"));
	Manifest->SetStringField(TEXT("local_file"), ObjPath);
	Manifest->SetStringField(TEXT("suggested_asset_name"), AssetName);
	Manifest->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(TEXT("completed_at_utc"), FDateTime::UtcNow().ToIso8601());

	if (!MonolithModelGen::WriteJsonFile(MonolithModelGen::ManifestPath(JobId), Manifest, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}

	return FMonolithActionResult::Success(Manifest);
}

FMonolithActionResult FMonolithModelGenActions::GetGeneratedModelJob(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}
	FString JobId;
	if (!Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: job_id"), -32602);
	}

	FString Error;
	if (!MonolithModelGen::ValidateJobId(JobId, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	TSharedPtr<FJsonObject> Manifest = MonolithModelGen::ReadJsonFile(MonolithModelGen::ManifestPath(JobId), Error);
	if (!Manifest.IsValid())
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	return FMonolithActionResult::Success(Manifest);
}

FMonolithActionResult FMonolithModelGenActions::CancelGeneratedModelJob(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithActionResult ReadResult = GetGeneratedModelJob(Params);
	if (!ReadResult.bSuccess)
	{
		return ReadResult;
	}

	FString Status = ReadResult.Result->GetStringField(TEXT("status"));
	TSharedPtr<FJsonObject> Result = ReadResult.Result;
	if (Status == TEXT("completed") || Status == TEXT("failed") || Status == TEXT("cancelled"))
	{
		Result->SetBoolField(TEXT("cancelled"), false);
		Result->SetStringField(TEXT("cancel_note"), TEXT("Job is already terminal"));
		return FMonolithActionResult::Success(Result);
	}

	Result->SetStringField(TEXT("status"), TEXT("cancelled"));
	Result->SetBoolField(TEXT("cancelled"), true);
	Result->SetStringField(TEXT("cancelled_at_utc"), FDateTime::UtcNow().ToIso8601());

	FString JobId;
	if (!Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: job_id"), -32602);
	}
	FString Error;
	if (!MonolithModelGen::ValidateJobId(JobId, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (!MonolithModelGen::WriteJsonFile(MonolithModelGen::ManifestPath(JobId), Result, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModelGenActions::DownloadGeneratedModelResult(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithActionResult ReadResult = GetGeneratedModelJob(Params);
	if (!ReadResult.bSuccess)
	{
		return ReadResult;
	}

	const FString Status = ReadResult.Result->GetStringField(TEXT("status"));
	if (Status != TEXT("completed"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Job is not completed (status=%s)"), *Status), -32602);
	}

	const FString LocalFile = ReadResult.Result->GetStringField(TEXT("local_file"));
	if (!FPaths::FileExists(LocalFile))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Generated model file not found: %s"), *LocalFile), -32603);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("job_id"), ReadResult.Result->GetStringField(TEXT("job_id")));
	Result->SetStringField(TEXT("status"), Status);
	Result->SetStringField(TEXT("file_path"), LocalFile);
	Result->SetStringField(TEXT("format"), FPaths::GetExtension(LocalFile).ToLower());
	Result->SetNumberField(TEXT("file_size_bytes"), IFileManager::Get().FileSize(*LocalFile));
	Result->SetStringField(TEXT("download_policy"), TEXT("local_file_only"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModelGenActions::ImportGeneratedModel(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString Destination;
	if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
	{
		Destination = TEXT("/Game/GeneratedModels");
	}
	FString DestinationError;
	if (!MonolithModelGen::ValidateDestinationFolder(Destination, DestinationError))
	{
		return FMonolithActionResult::Error(DestinationError, -32602);
	}

	FString FilePath;
	FString JobId;
	TSharedPtr<FJsonObject> Manifest;
	if (Params->TryGetStringField(TEXT("job_id"), JobId) && !JobId.IsEmpty())
	{
		FString Error;
		if (!MonolithModelGen::ValidateJobId(JobId, Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
		Manifest = MonolithModelGen::ReadJsonFile(MonolithModelGen::ManifestPath(JobId), Error);
		if (!Manifest.IsValid())
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
		if (Manifest->GetStringField(TEXT("status")) != TEXT("completed"))
		{
			return FMonolithActionResult::Error(TEXT("Generated model job must be completed before import"), -32602);
		}
		FilePath = Manifest->GetStringField(TEXT("local_file"));
	}
	else if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Either job_id or file_path is required"), -32602);
	}

	if (!FPaths::FileExists(FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Model file not found: %s"), *FilePath), -32602);
	}
	if (!MonolithModelGen::IsSupportedModelFile(FilePath))
	{
		return FMonolithActionResult::Error(TEXT("Unsupported model file extension. Expected .fbx, .obj, .glb, or .gltf"), -32602);
	}

	TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Files;
	Files.Reserve(1);
	Files.Add(MakeShared<FJsonValueString>(FilePath));
	ImportParams->SetArrayField(TEXT("files"), Files);
	ImportParams->SetStringField(TEXT("destination"), Destination);

	bool bReplaceExisting = false;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
	ImportParams->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	ImportParams->SetBoolField(TEXT("combine_meshes"), true);
	ImportParams->SetBoolField(TEXT("generate_lightmap_uvs"), true);
	ImportParams->SetBoolField(TEXT("auto_generate_collision"), true);

	FString MaterialImport;
	if (!Params->TryGetStringField(TEXT("material_import"), MaterialImport) || MaterialImport.IsEmpty())
	{
		MaterialImport = TEXT("create_new");
	}
	ImportParams->SetStringField(TEXT("material_import"), MaterialImport);

	FMonolithActionResult ImportResult = FMonolithMeshTechArtActions::ImportMesh(ImportParams);
	if (!ImportResult.bSuccess)
	{
		return ImportResult;
	}

	FString Provider = TEXT("external");
	FString Model = TEXT("unknown");
	FString Prompt;
	FString PromptHash;
	FString SourceImageHash;
	if (Manifest.IsValid())
	{
		Provider = Manifest->GetStringField(TEXT("provider"));
		Model = Manifest->GetStringField(TEXT("model"));
		JobId = Manifest->GetStringField(TEXT("job_id"));
		Manifest->TryGetStringField(TEXT("prompt_hash"), PromptHash);
		SourceImageHash = Manifest->GetStringField(TEXT("source_image_hash"));
	}
	else
	{
		Params->TryGetStringField(TEXT("provider"), Provider);
		Params->TryGetStringField(TEXT("model"), Model);
		Params->TryGetStringField(TEXT("prompt"), Prompt);
		Params->TryGetStringField(TEXT("source_image_hash"), SourceImageHash);
	}

	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
	TSharedPtr<FJsonObject> Provenance = MonolithModelGen::BuildProvenance(
		Provider, Model, Manifest.IsValid() ? TEXT("local_deterministic") : TEXT("external_file"),
		JobId, Prompt, SourceImageHash, FPaths::GetExtension(FilePath).ToLower(), FileSize);
	if (!PromptHash.IsEmpty())
	{
		Provenance->SetStringField(TEXT("prompt_hash"), PromptHash);
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	const TArray<TSharedPtr<FJsonValue>>* ImportedArray = nullptr;
	TArray<TSharedPtr<FJsonValue>> GeneratedMeshes;
	if (ImportResult.Result->TryGetArrayField(TEXT("imported"), ImportedArray) && ImportedArray)
	{
		GeneratedMeshes.Reserve(ImportedArray->Num());
		for (const TSharedPtr<FJsonValue>& ImportedValue : *ImportedArray)
		{
			const TSharedPtr<FJsonObject>* ImportedObj = nullptr;
			if (!ImportedValue->TryGetObject(ImportedObj) || !ImportedObj || !ImportedObj->IsValid())
			{
				continue;
			}

			const FString ImportedPath = (*ImportedObj)->GetStringField(TEXT("asset_path"));
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MonolithModelGen::ObjectPathFromAssetPath(ImportedPath));
			if (!Mesh)
			{
				continue;
			}

			FString Error;
			if (!MonolithModelGen::ApplyProvenance(Mesh, Provenance, bSave, Error))
			{
				return FMonolithActionResult::Error(Error, -32603);
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
			Entry->SetStringField(TEXT("object_path"), Mesh->GetPathName());
			if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
				Entry->SetNumberField(TEXT("vertex_count"), LOD0.GetNumVertices());
				Entry->SetNumberField(TEXT("triangle_count"), LOD0.GetNumTriangles());
			}
			GeneratedMeshes.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	if (GeneratedMeshes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Import completed but no StaticMesh asset was found"), -32603);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("imported"), GeneratedMeshes);
	Result->SetNumberField(TEXT("total_imported"), GeneratedMeshes.Num());
	Result->SetStringField(TEXT("file_path"), FilePath);
	Result->SetStringField(TEXT("destination"), Destination);
	Result->SetBoolField(TEXT("saved"), bSave);
	Result->SetObjectField(TEXT("provenance"), Provenance);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithModelGenActions::GetGeneratedModelProvenance(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
	}

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MonolithModelGen::ObjectPathFromAssetPath(AssetPath));
	if (!Mesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' could not be loaded as UStaticMesh"), *AssetPath), -32602);
	}

	TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
#if WITH_METADATA
	FMetaData& MetaData = Mesh->GetOutermost()->GetMetaData();
	const TArray<FString> Keys = {
		TEXT("kind"), TEXT("provider"), TEXT("model"), TEXT("source"), TEXT("job_id"),
		TEXT("prompt_hash"), TEXT("source_image_hash"), TEXT("prompt_redacted"),
		TEXT("format"), TEXT("file_size_bytes"), TEXT("generated_at_utc")
	};
	bool bFoundAny = false;
	for (const FString& Key : Keys)
	{
		const FString FullKey = FString::Printf(TEXT("Monolith.Generated.%s"), *Key);
		if (const FString* Value = MetaData.FindValue(Mesh, *FullKey))
		{
			Provenance->SetStringField(Key, *Value);
			bFoundAny = true;
		}
	}
#else
	const bool bFoundAny = false;
#endif

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Mesh->GetOutermost()->GetName());
	Result->SetStringField(TEXT("object_path"), Mesh->GetPathName());
	Result->SetBoolField(TEXT("found"), bFoundAny);
	Result->SetObjectField(TEXT("provenance"), Provenance);
	return FMonolithActionResult::Success(Result);
}
