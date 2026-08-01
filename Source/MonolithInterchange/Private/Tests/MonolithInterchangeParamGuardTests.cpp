#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MonolithInterchangeActions.h"
#include "MonolithInterchangeImportRollback.h"
#include "UObject/Package.h"

namespace
{
	bool HasMessageCode(
		const TSharedPtr<FJsonObject>& Payload,
		const FString& Code)
	{
		if (!Payload.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
		if (!Payload->TryGetArrayField(TEXT("messages"), Messages) || !Messages)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
		{
			const TSharedPtr<FJsonObject> Message = MessageValue->AsObject();
			if (Message.IsValid() &&
				Message->HasTypedField<EJson::String>(TEXT("code")) &&
				Message->GetStringField(TEXT("code")) == Code)
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> GetFirstRow(const FMonolithActionResult& Result)
	{
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.Result->TryGetArrayField(TEXT("rows"), Rows) ||
			!Rows ||
			Rows->Num() == 0)
		{
			return nullptr;
		}
		return (*Rows)[0]->AsObject();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardInterchangeImportMalformedParamsTest, "Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardInterchangeImportMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("interchange"), TEXT("get_supported_formats")))
	{
		FMonolithInterchangeActions::RegisterActions(Registry);
	}

	static const TCHAR* ExpectedActions[] = {
		TEXT("get_supported_formats"),
		TEXT("can_import"),
		TEXT("can_reimport"),
		TEXT("get_import_data"),
		TEXT("import_asset"),
		TEXT("import_assets"),
		TEXT("import_scene"),
		TEXT("import_mesh"),
		TEXT("import_skeletal_mesh"),
		TEXT("import_texture"),
		TEXT("import_audio"),
		TEXT("update_reimport_path"),
		TEXT("reimport_asset"),
		TEXT("reimport_assets"),
		TEXT("export_asset")
	};

	for (const TCHAR* ActionName : ExpectedActions)
	{
		TestTrue(
			FString::Printf(TEXT("interchange.%s action is registered"), ActionName),
			Registry.HasAction(TEXT("interchange"), ActionName));
	}
	TestFalse(
		TEXT("unimplemented import_with_options action is not advertised"),
		Registry.HasAction(TEXT("interchange"), TEXT("import_with_options")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
		Params->SetBoolField(TEXT("dry_run"), true);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestFalse(TEXT("import_asset rejects missing source_file"), Result.bSuccess);
		TestTrue(TEXT("import_asset reports missing source_file"), Result.ErrorMessage.Contains(TEXT("source_file")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_file"), TEXT("missing.fbx"));
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestTrue(TEXT("import_asset returns structured row for guarded mutation failure"), Result.bSuccess);
		TestTrue(TEXT("import_asset response object is valid"), Result.Result.IsValid());
	}

	{
		const FString SourceFile =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) /
			TEXT("Automation/MonolithInterchange/type_mismatch.png");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourceFile), true);
		TestTrue(
			TEXT("typed import fixture was written"),
			FFileHelper::SaveStringToFile(TEXT("not decoded because type validation must fail first"), *SourceFile));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_file"), SourceFile);
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("import_audio"), Params);
		TestTrue(TEXT("import_audio returns a structured typed-validation row"), Result.bSuccess);
		if (TestTrue(TEXT("import_audio typed-validation payload is valid"), Result.Result.IsValid()))
		{
			TestEqual(
				TEXT("import_audio reports its requested kind"),
				Result.Result->GetStringField(TEXT("requested_import_kind")),
				FString(TEXT("audio")));

			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (TestTrue(
				TEXT("import_audio returns one row"),
				Result.Result->TryGetArrayField(TEXT("rows"), Rows) && Rows && Rows->Num() == 1))
			{
				const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
				TestTrue(TEXT("typed-validation row is valid"), Row.IsValid());
				if (Row.IsValid())
				{
					TestEqual(
						TEXT("mismatched PNG is rejected by the audio entrypoint"),
						Row->GetStringField(TEXT("status")),
						FString(TEXT("error")));
					TestEqual(
						TEXT("audio entrypoint reaches audio-specific validation"),
						Row->GetStringField(TEXT("requested_import_kind")),
						FString(TEXT("audio")));

					const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
					bool bFoundTypedMismatch = false;
					if (Row->TryGetArrayField(TEXT("messages"), Messages) && Messages)
					{
						for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
						{
							const TSharedPtr<FJsonObject> Message = MessageValue->AsObject();
							if (Message.IsValid() &&
								Message->GetStringField(TEXT("code")) == TEXT("typed_import_extension_mismatch"))
							{
								bFoundTypedMismatch = true;
								break;
							}
						}
					}
					TestTrue(
						TEXT("audio entrypoint identifies the typed extension mismatch"),
						bFoundTypedMismatch);
				}
			}
		}

		TestTrue(
			TEXT("typed import fixture was removed"),
			IFileManager::Get().Delete(*SourceFile, false, true, true));
	}

	{
		const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString FixtureRoot =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) /
			TEXT("Automation/MonolithInterchange") /
			FixtureId;
		const FString SourceA = FixtureRoot / TEXT("A/duplicate.png");
		const FString SourceB = FixtureRoot / TEXT("B/duplicate.png");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourceA), true);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourceB), true);
		TArray<uint8> PngBytes;
		TestTrue(
			TEXT("batch-preview PNG fixture decodes"),
			FBase64::Decode(
				TEXT(
					"iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GB"
					"gYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC"),
				PngBytes));
		TestTrue(
			TEXT("first batch-preview fixture was written"),
			FFileHelper::SaveArrayToFile(PngBytes, *SourceA));
		TestTrue(
			TEXT("second batch-preview fixture was written"),
			FFileHelper::SaveArrayToFile(PngBytes, *SourceB));

		const FString DestinationPath =
			TEXT("/Game/Tests/Monolith/Interchange/Batch_") + FixtureId;
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("source_file"), SourceA);
			Params->SetStringField(
				TEXT("destination_path"),
				TEXT("  ") + DestinationPath + TEXT("/  "));

			const FMonolithActionResult Result =
				Registry.ExecuteAction(TEXT("interchange"), TEXT("can_import"), Params);
			TestTrue(TEXT("can_import normalization returns a payload"), Result.bSuccess && Result.Result.IsValid());
			if (Result.bSuccess && Result.Result.IsValid())
			{
				const TSharedPtr<FJsonObject>* Destination = nullptr;
				if (TestTrue(
					TEXT("can_import returns destination validation"),
					Result.Result->TryGetObjectField(TEXT("destination"), Destination) &&
						Destination &&
						Destination->IsValid()))
				{
					TestEqual(
						TEXT("can_import normalizes harmless destination formatting"),
						(*Destination)->GetStringField(TEXT("destination_path")),
						DestinationPath);
					TestTrue(
						TEXT("normalized destination is valid"),
						(*Destination)->GetBoolField(TEXT("valid")));
				}
			}
		}

		auto MakeBatchParams = [&SourceA, &SourceB, &DestinationPath](const FString& ConflictPolicy)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sources;
			Sources.Add(MakeShared<FJsonValueString>(SourceA));
			Sources.Add(MakeShared<FJsonValueString>(SourceB));
			Params->SetArrayField(TEXT("source_files"), Sources);
			Params->SetStringField(TEXT("destination_path"), DestinationPath);
			Params->SetStringField(TEXT("conflict_policy"), ConflictPolicy);
			Params->SetBoolField(TEXT("dry_run"), true);
			return Params;
		};
		{
			const FMonolithActionResult Result =
				Registry.ExecuteAction(
					TEXT("interchange"),
					TEXT("import_assets"),
					MakeBatchParams(TEXT("fail")));
			TestTrue(TEXT("fail-policy batch preview returns a payload"), Result.bSuccess && Result.Result.IsValid());
			if (Result.bSuccess && Result.Result.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
				if (TestTrue(
					TEXT("fail-policy batch preview returns two rows"),
					Result.Result->TryGetArrayField(TEXT("rows"), Rows) &&
						Rows &&
						Rows->Num() == 2))
				{
					TestEqual(
						TEXT("first same-name source can be imported"),
						(*Rows)[0]->AsObject()->GetStringField(TEXT("status")),
						FString(TEXT("would_import")));
					TestEqual(
						TEXT("second same-name source sees the prospective conflict"),
						(*Rows)[1]->AsObject()->GetStringField(TEXT("status")),
						FString(TEXT("error")));
					TestTrue(
						TEXT("second same-name source reports a package conflict"),
						(*Rows)[1]->AsObject()->GetBoolField(TEXT("likely_package_conflict")));
				}
			}
		}

		{
			const FMonolithActionResult Result =
				Registry.ExecuteAction(
					TEXT("interchange"),
					TEXT("import_assets"),
					MakeBatchParams(TEXT("rename")));
			TestTrue(TEXT("rename-policy batch preview returns a payload"), Result.bSuccess && Result.Result.IsValid());
			if (Result.bSuccess && Result.Result.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
				if (TestTrue(
					TEXT("rename-policy batch preview returns two rows"),
					Result.Result->TryGetArrayField(TEXT("rows"), Rows) &&
						Rows &&
						Rows->Num() == 2))
				{
					const TSharedPtr<FJsonObject> FirstRow = (*Rows)[0]->AsObject();
					const TSharedPtr<FJsonObject> SecondRow = (*Rows)[1]->AsObject();
					TestEqual(
						TEXT("first rename preview is importable"),
						FirstRow->GetStringField(TEXT("status")),
						FString(TEXT("would_import")));
					TestEqual(
						TEXT("second rename preview is importable"),
						SecondRow->GetStringField(TEXT("status")),
						FString(TEXT("would_import")));
					TestNotEqual(
						TEXT("same-name rename previews reserve distinct package names"),
						FirstRow->GetStringField(TEXT("resolved_package")),
						SecondRow->GetStringField(TEXT("resolved_package")));
				}
			}
		}

		TestTrue(
			TEXT("batch-preview fixture directory was removed"),
			IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetStringField(TEXT("source_file_index"), TEXT("1"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("reimport_asset"), Params);
		TestTrue(TEXT("reimport_asset returns structured index validation"), Result.bSuccess && Result.Result.IsValid());
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (TestTrue(
				TEXT("reimport_asset index validation returns one row"),
				Result.Result->TryGetArrayField(TEXT("rows"), Rows) &&
					Rows &&
					Rows->Num() == 1))
			{
				const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
				bool bFoundInvalidIndex = false;
				if (Row.IsValid() &&
					Row->TryGetArrayField(TEXT("messages"), Messages) &&
					Messages)
				{
					for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
					{
						const TSharedPtr<FJsonObject> Message = MessageValue->AsObject();
						if (Message.IsValid() &&
							Message->GetStringField(TEXT("code")) == TEXT("invalid_source_file_index"))
						{
							bFoundInvalidIndex = true;
							break;
						}
					}
				}
				TestTrue(
					TEXT("nonnumeric source_file_index is rejected explicitly"),
					bFoundInvalidIndex);
			}
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetBoolField(TEXT("source_file"), true);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("reimport_asset"), Params);
		const TSharedPtr<FJsonObject> Row = GetFirstRow(Result);
		TestTrue(
			TEXT("reimport_asset returns a row for a mistyped optional source_file"),
			Row.IsValid());
		TestTrue(
			TEXT("mistyped optional source_file is rejected instead of falling back to stored metadata"),
			HasMessageCode(Row, TEXT("invalid_source_file")));
	}

	{
		const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString FixtureRoot =
			FPaths::ProjectSavedDir() / TEXT("Automation/MonolithInterchange") / FixtureId;
		const FString SourceA = FixtureRoot / TEXT("scene_a.fbx");
		const FString SourceB = FixtureRoot / TEXT("scene_b.fbx");
		IFileManager::Get().MakeDirectory(*FixtureRoot, true);
		TestTrue(
			TEXT("first multi-output fixture was written"),
			FFileHelper::SaveStringToFile(TEXT("guard-only fbx fixture A"), *SourceA));
		TestTrue(
			TEXT("second multi-output fixture was written"),
			FFileHelper::SaveStringToFile(TEXT("guard-only fbx fixture B"), *SourceB));

		const FString DestinationPath =
			TEXT("/Game/Tests/Monolith/Interchange/MultiOutput_") + FixtureId;
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("source_file"), SourceA);
			Params->SetStringField(TEXT("destination_path"), DestinationPath);
			Params->SetStringField(TEXT("conflict_policy"), TEXT("rename"));
			Params->SetBoolField(TEXT("dry_run"), true);

			const FMonolithActionResult Result =
				Registry.ExecuteAction(TEXT("interchange"), TEXT("import_scene"), Params);
			const TSharedPtr<FJsonObject> Row = GetFirstRow(Result);
			TestTrue(
				TEXT("scene import returns a guarded multi-output row"),
				Row.IsValid());
			TestTrue(
				TEXT("scene rename policy is rejected because secondary package names are importer-defined"),
				HasMessageCode(Row, TEXT("multi_output_conflict_policy_unsupported")));
		}

		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sources;
			Sources.Add(MakeShared<FJsonValueString>(SourceA));
			Sources.Add(MakeShared<FJsonValueString>(SourceB));
			Params->SetArrayField(TEXT("source_files"), Sources);
			Params->SetStringField(TEXT("destination_path"), DestinationPath);
			Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
			Params->SetBoolField(TEXT("dry_run"), true);

			const FMonolithActionResult Result =
				Registry.ExecuteAction(TEXT("interchange"), TEXT("import_assets"), Params);
			const TSharedPtr<FJsonObject> Row = GetFirstRow(Result);
			TestTrue(
				TEXT("multi-source scene batch returns a guarded row"),
				Row.IsValid());
			TestTrue(
				TEXT("multi-source scene batch is rejected before unknown secondary outputs can collide"),
				HasMessageCode(Row, TEXT("multi_output_batch_unsupported")));
		}

		TestTrue(
			TEXT("multi-output fixture directory was removed"),
			IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true));
	}

	{
		const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString ReplacementSource =
			FPaths::ProjectSavedDir() /
			TEXT("Automation/MonolithInterchange") /
			(FixtureId + TEXT(".wav"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReplacementSource), true);
		TestTrue(
			TEXT("replacement compatibility fixture was written"),
			FFileHelper::SaveStringToFile(TEXT("guard-only wav fixture"), *ReplacementSource));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetStringField(TEXT("source_file"), ReplacementSource);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("reimport_asset"), Params);
		const TSharedPtr<FJsonObject> Row = GetFirstRow(Result);
		TestTrue(
			TEXT("replacement compatibility validation returns a row"),
			Row.IsValid());
		TestTrue(
			TEXT("audio replacement is rejected for a texture asset"),
			HasMessageCode(Row, TEXT("replacement_source_incompatible")));

		TestTrue(
			TEXT("replacement compatibility fixture was removed"),
			IFileManager::Get().Delete(*ReplacementSource, false, true, true));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetStringField(
			TEXT("file_path"),
			FPaths::ProjectSavedDir() / TEXT("Automation/MonolithInterchange/default.unsupported"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("export_asset"), Params);
		TestTrue(TEXT("export_asset returns structured preflight data"), Result.bSuccess);
		if (TestTrue(TEXT("export_asset preflight payload is valid"), Result.Result.IsValid()))
		{
			TestEqual(
				TEXT("unsupported extension is rejected during dry-run"),
				Result.Result->GetStringField(TEXT("status")),
				FString(TEXT("error")));
			TestFalse(
				TEXT("unsupported extension has no matching exporter"),
				Result.Result->GetBoolField(TEXT("exporter_available")));
		}
	}

	{
		const FString DirectoryPath =
			FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() /
				TEXT("Automation/MonolithInterchange") /
				(FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png")));
		TestTrue(
			TEXT("directory-shaped export fixture was created"),
			IFileManager::Get().MakeDirectory(*DirectoryPath, true));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetStringField(TEXT("file_path"), DirectoryPath);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("export_asset"), Params);
		TestTrue(
			TEXT("directory-shaped export preflight returns structured data"),
			Result.bSuccess && Result.Result.IsValid());
		TestTrue(
			TEXT("existing directory is rejected as an output file"),
			HasMessageCode(Result.Result, TEXT("output_path_is_directory")));
		if (Result.Result.IsValid())
		{
			TestTrue(
				TEXT("export response reports the directory collision"),
				Result.Result->GetBoolField(TEXT("path_is_directory")));
		}

		TestTrue(
			TEXT("directory-shaped export fixture was removed"),
			IFileManager::Get().DeleteDirectory(*DirectoryPath, false, true));
	}

	{
		const FString ExportPath =
			FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() /
				TEXT("Automation/MonolithInterchange") /
				(FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png")));
		TestTrue(
			TEXT("transactional export replacement fixture was created"),
			FFileHelper::SaveStringToFile(TEXT("original destination sentinel"), *ExportPath));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Params->SetStringField(TEXT("file_path"), ExportPath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("interchange"), TEXT("export_asset"), Params);
		TestTrue(
			TEXT("transactional export returns structured data"),
			Result.bSuccess && Result.Result.IsValid());
		if (Result.Result.IsValid())
		{
			TestEqual(
				TEXT("transactional export reports success"),
				Result.Result->GetStringField(TEXT("status")),
				FString(TEXT("exported")));
			TestTrue(
				TEXT("transactional export commits the staged file"),
				Result.Result->GetBoolField(TEXT("commit_succeeded")));
			TestTrue(
				TEXT("transactional export reports a complete rollback capability"),
				Result.Result->GetBoolField(TEXT("rollback_complete")));
			TestFalse(
				TEXT("successful transactional export is not a partial mutation"),
				Result.Result->GetBoolField(TEXT("partial_mutation")));
			TestTrue(
				TEXT("transactional export removes its staging directory"),
				Result.Result->GetBoolField(TEXT("staging_cleanup_complete")));
			TestEqual(
				TEXT("default texture export resolves one output file"),
				static_cast<int32>(Result.Result->GetNumberField(TEXT("output_file_count"))),
				1);
		}

		TArray<uint8> ExportedBytes;
		TestTrue(
			TEXT("transactional export destination exists"),
			FFileHelper::LoadFileToArray(ExportedBytes, *ExportPath));
		TestTrue(
			TEXT("transactional export replaced the short original sentinel with PNG bytes"),
			ExportedBytes.Num() > FCString::Strlen(TEXT("original destination sentinel")));
		TestTrue(
			TEXT("transactional export fixture was removed"),
			IFileManager::Get().Delete(*ExportPath, false, true, true));
	}

	{
		const FString RollbackId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString NewPackagePath =
			FString::Printf(TEXT("/Game/Tests/Monolith/Interchange/Rollback_%s/NewAsset"), *RollbackId);
		UPackage* NewPackage = CreatePackage(*NewPackagePath);
		UTexture2D* NewAsset = NewObject<UTexture2D>(
			NewPackage,
			TEXT("NewAsset"),
			RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(NewAsset);

		TArray<UObject*> NewImportResults;
		NewImportResults.Add(NewAsset);
		const FMonolithInterchangeRollbackResult CompleteRollback =
			RollbackNewImportedObjects(NewImportResults, TSet<FName>());
		TestTrue(TEXT("new typed-mismatch assets are fully rolled back"), CompleteRollback.IsComplete());
		TestEqual(TEXT("one rollback candidate is identified"), CompleteRollback.CandidateCount, 1);
		TestEqual(TEXT("one rollback candidate is deleted"), CompleteRollback.DeletedCount, 1);
		TestEqual(TEXT("complete rollback reports the deleted path"), CompleteRollback.DeletedObjectPaths.Num(), 1);

		const FString ExistingPackagePath =
			FString::Printf(TEXT("/Game/Tests/Monolith/Interchange/Rollback_%s/ExistingAsset"), *RollbackId);
		UPackage* ExistingPackage = CreatePackage(*ExistingPackagePath);
		UTexture2D* ExistingAsset = NewObject<UTexture2D>(
			ExistingPackage,
			TEXT("ExistingAsset"),
			RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(ExistingAsset);

		TArray<UObject*> ExistingImportResults;
		ExistingImportResults.Add(ExistingAsset);
		TSet<FName> PreExistingPaths;
		PreExistingPaths.Add(FName(*ExistingAsset->GetPathName()));
		const FMonolithInterchangeRollbackResult PartialRollback =
			RollbackNewImportedObjects(ExistingImportResults, PreExistingPaths);
		TestFalse(TEXT("pre-existing typed-mismatch results are not misreported as rolled back"), PartialRollback.IsComplete());
		TestEqual(
			TEXT("pre-existing result is preserved for explicit partial-mutation reporting"),
			PartialRollback.PreExistingObjectPaths.Num(),
			1);
		TestEqual(TEXT("pre-existing results are not rollback candidates"), PartialRollback.CandidateCount, 0);

		const FMonolithInterchangeRollbackResult CleanupRollback =
			RollbackNewImportedObjects(ExistingImportResults, TSet<FName>());
		TestTrue(TEXT("pre-existing rollback fixture cleanup succeeds"), CleanupRollback.IsComplete());
	}

	return true;
}
