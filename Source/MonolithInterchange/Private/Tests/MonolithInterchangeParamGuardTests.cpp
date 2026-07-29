#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithInterchangeActions.h"

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
			FPaths::ProjectSavedDir() / TEXT("Automation/MonolithInterchange/type_mismatch.png");
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

	return true;
}
