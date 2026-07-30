#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithLocalizationActions.h"
#include "MonolithLocalizationTargetConfig.h"
#include "MonolithToolRegistry.h"

namespace
{
	bool WriteLocalizationPipelineConfigFixture(
		const FString& TargetName,
		const FString& OperationSuffix,
		const FString& CommandletClass,
		FString& OutConfigPath)
	{
		OutConfigPath = FPaths::Combine(
			FPaths::ProjectConfigDir(),
			TEXT("Localization"),
			FString::Printf(TEXT("%s_%s.ini"), *TargetName, *OperationSuffix));
		const FString ContentPath = FString::Printf(TEXT("Content/Localization/%s"), *TargetName);
		const FString ConfigText = FString::Printf(
			TEXT("[CommonSettings]\n")
			TEXT("SourcePath=%s\n")
			TEXT("DestinationPath=%s\n")
			TEXT("\n")
			TEXT("[GatherTextStep0]\n")
			TEXT("CommandletClass=%s\n"),
			*ContentPath,
			*ContentPath,
			*CommandletClass);
		return FFileHelper::SaveStringToFile(ConfigText, *OutConfigPath);
	}

	TSharedPtr<FJsonObject> MakeValidLocalizationTargetSearchDirectoryParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), TEXT("EngineOverrides"));
		Params->SetArrayField(
			TEXT("search_directories"),
			{MakeShared<FJsonValueString>(
				TEXT("%LOCENGINEROOT%Source/Runtime/InputCore"))});
		Params->SetStringField(
			TEXT("source_control_policy"),
			TEXT("require_checked_out"));
		Params->SetNumberField(TEXT("target_changelist"), 1203);
		Params->SetBoolField(TEXT("dry_run"), true);
		return Params;
	}

	TSharedPtr<FJsonObject> FindLocalizationTargetSearchDirectorySchema()
	{
		for (const FMonolithActionInfo& Info :
			FMonolithToolRegistry::Get().GetActions(TEXT("localization")))
		{
			if (Info.Action == TEXT("set_target_text_search_directories"))
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableActionsRegisterTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableActionsRegister", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableActionsRegisterTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("create_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("create_string_table")));
	TestTrue(TEXT("set_string_entry action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_entry")));
	TestTrue(TEXT("remove_string_entry action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("remove_string_entry")));
	TestTrue(TEXT("set_string_metadata action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_metadata")));
	TestTrue(TEXT("import_string_table_csv action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("import_string_table_csv")));
	TestTrue(TEXT("export_string_table_csv action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("export_string_table_csv")));
	TestTrue(TEXT("set_target_text_search_directories action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_target_text_search_directories")));
	TestTrue(TEXT("run_target_pipeline action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("run_target_pipeline")));

	const FMonolithActionExecutionPolicy ConfigureTargetPolicy =
		FMonolithToolRegistry::Get().GetActionExecutionPolicy(
			TEXT("localization"),
			TEXT("set_target_text_search_directories"));
	TestTrue(
		TEXT("set_target_text_search_directories is classified as a mutation"),
		ConfigureTargetPolicy.PolicyId != TEXT("read_only"));
	TestEqual(
		TEXT("target configuration uses explicit dirty-package tracking"),
		ConfigureTargetPolicy.PolicyId,
		FString(TEXT("track_dirty_packages")));
	TestFalse(
		TEXT("target configuration policy is not inferred"),
		ConfigureTargetPolicy.bDefaulted);
	TestTrue(
		TEXT("target configuration tracks dirty packages"),
		ConfigureTargetPolicy.bDirtyPackageTracking);
	TestFalse(
		TEXT("target configuration does not nest a central transaction"),
		ConfigureTargetPolicy.bTransactionWrapping);
	TestFalse(
		TEXT("target configuration owns post-edit/readback validation"),
		ConfigureTargetPolicy.bPostEditValidation);
	TestTrue(
		TEXT("target configuration policy is enforced"),
		ConfigureTargetPolicy.bEnforced);

	const TSharedPtr<FJsonObject> ConfigureTargetSchema =
		FindLocalizationTargetSearchDirectorySchema();
	TestNotNull(
		TEXT("target configuration schema is registered"),
		ConfigureTargetSchema.Get());
	if (ConfigureTargetSchema.IsValid())
	{
		const TSharedPtr<FJsonObject>* SourceControlPolicySchema = nullptr;
		const TSharedPtr<FJsonObject>* TargetChangelistSchema = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* SourceControlPolicyValues = nullptr;
		TestTrue(
			TEXT("source_control_policy is a required enum"),
			ConfigureTargetSchema->TryGetObjectField(
				TEXT("source_control_policy"),
				SourceControlPolicySchema) &&
				SourceControlPolicySchema &&
				SourceControlPolicySchema->IsValid() &&
				(*SourceControlPolicySchema)->GetBoolField(TEXT("required")) &&
				(*SourceControlPolicySchema)->TryGetArrayField(
					TEXT("enum"),
					SourceControlPolicyValues) &&
				SourceControlPolicyValues &&
				SourceControlPolicyValues->Num() == 1 &&
				(*SourceControlPolicyValues)[0]->AsString() ==
					TEXT("require_checked_out"));
		TestTrue(
			TEXT("target_changelist is a required positive integer"),
			ConfigureTargetSchema->TryGetObjectField(
				TEXT("target_changelist"),
				TargetChangelistSchema) &&
				TargetChangelistSchema &&
				TargetChangelistSchema->IsValid() &&
				(*TargetChangelistSchema)->GetBoolField(TEXT("required")) &&
				(*TargetChangelistSchema)->GetStringField(TEXT("type")) ==
					TEXT("integer") &&
				(*TargetChangelistSchema)->GetNumberField(TEXT("minimum")) ==
					1.0);
	}

	FMonolithActionExecutionGuard& ExecutionGuard =
		FMonolithActionExecutionGuard::Get();
	FMonolithActionExecutionGuard::FExecutionScope GuardScope =
		ExecutionGuard.BeginAction(
			TEXT("localization"),
			TEXT("set_target_text_search_directories"),
			MakeValidLocalizationTargetSearchDirectoryParams());
	TestTrue(
		TEXT("module registration selects handler-owned source control"),
		GuardScope.bHandlerOwnedSourceControlPrepare);
	TestFalse(
		TEXT("central guard does not auto-checkout localization config files"),
		GuardScope.bSourceControlPrepareActive);
	ExecutionGuard.SetActionOutcome(
		GuardScope,
		/*bSuccess=*/false,
		/*ErrorCode=*/-1,
		nullptr,
		TEXT("focused registration probe"));
	ExecutionGuard.EndAction(GuardScope);

	const FMonolithActionExecutionPolicy PipelinePolicy =
		FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("localization"), TEXT("run_target_pipeline"));
	TestTrue(TEXT("run_target_pipeline is classified as a mutation"), PipelinePolicy.PolicyId != TEXT("read_only"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableWriteGateTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableWriteGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableWriteGateTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_WriteGate"));

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
	TestFalse(TEXT("create_string_table rejects mutation without dry_run or confirm"), Result.bSuccess);
	TestTrue(TEXT("write gate error mentions dry_run or confirm"), Result.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableCreateDryRunTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableCreateDryRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableCreateDryRunTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_DryRunOnly"));
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
	TestTrue(TEXT("create_string_table dry_run succeeds without creating an asset"), Result.bSuccess);
	bool bWouldCreate = false;
	bool bChanged = true; // default to true so failure to parse doesn't artificially pass the !changed check
	if (Result.Result.IsValid())
	{
		Result.Result->TryGetBoolField(TEXT("would_create"), bWouldCreate);
		Result.Result->TryGetBoolField(TEXT("changed"), bChanged);
	}
	TestTrue(TEXT("dry_run result reports would_create"), bWouldCreate);
	TestTrue(TEXT("dry_run result reports changed=false"), !bChanged);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamGuardLocalizationTargetSearchDirectoriesWriteGateTest,
	"Monolith.ParamGuard.MonolithConfig.LocalizationTargetSearchDirectoriesWriteGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetSearchDirectoriesWriteGateTest::RunTest(
	const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("WriteGateFixture"));
	Params->SetArrayField(
		TEXT("search_directories"),
		{MakeShared<FJsonValueString>(TEXT("%LOCPROJECTROOT%Source"))});

	const FMonolithActionResult Result =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("localization"),
			TEXT("set_target_text_search_directories"),
			Params);
	TestFalse(
		TEXT("set_target_text_search_directories rejects mutation without dry_run or confirm"),
		Result.bSuccess);
	TestTrue(
		TEXT("target search directory write gate mentions dry_run or confirm"),
		Result.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamGuardLocalizationTargetGatherConfigPatchTest,
	"Monolith.ParamGuard.MonolithConfig.LocalizationTargetGatherConfigPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetGatherConfigPatchTest::RunTest(
	const FString& Parameters)
{
	const FString ExistingConfig =
		TEXT("; generated header\r\n")
		TEXT("[CommonSettings]\r\n")
		TEXT("SourcePath=Content/Localization/Fixture\r\n")
		TEXT("DestinationPath=Content/Localization/Fixture\r\n")
		TEXT("ManifestName=Fixture.manifest\r\n")
		TEXT("\r\n")
		TEXT("[GatherTextStep0]\r\n")
		TEXT("CommandletClass=GatherTextFromSource\r\n")
		TEXT("SearchDirectoryPaths=%LOCPROJECTROOT%Config\r\n")
		TEXT("FileNameFilters=*.cpp\r\n")
		TEXT("\r\n")
		TEXT("[GatherTextStep1]\r\n")
		TEXT("CommandletClass=GenerateGatherManifest\r\n");

	MonolithLocalizationTargetConfig::FGatherConfigPatch Patch;
	FString Error;
	TestTrue(
		TEXT("canonical gather config patch builds"),
		MonolithLocalizationTargetConfig::BuildGatherConfigPatch(
			ExistingConfig,
			TEXT("Fixture"),
			{TEXT("%LOCPROJECTROOT%Source")},
			Patch,
			Error));
	TestTrue(TEXT("gather config patch reports a change"), Patch.bChanged);
	TestEqual(
		TEXT("one existing directory is read back"),
		Patch.ExistingSearchDirectories.Num(),
		1);
	if (Patch.ExistingSearchDirectories.Num() == 1)
	{
		TestEqual(
			TEXT("existing directory value is preserved"),
			Patch.ExistingSearchDirectories[0],
			FString(TEXT("%LOCPROJECTROOT%Config")));
	}
	TestEqual(
		TEXT("only the search directory line changes"),
		Patch.DesiredContents,
		ExistingConfig.Replace(
			TEXT("SearchDirectoryPaths=%LOCPROJECTROOT%Config"),
			TEXT("SearchDirectoryPaths=%LOCPROJECTROOT%Source"),
			ESearchCase::CaseSensitive));

	FString InvalidScopeConfig = ExistingConfig.Replace(
		TEXT("SourcePath=Content/Localization/Fixture"),
		TEXT("SourcePath=Content/Localization/Other"),
		ESearchCase::CaseSensitive);
	MonolithLocalizationTargetConfig::FGatherConfigPatch InvalidPatch;
	Error.Reset();
	TestFalse(
		TEXT("wrong target scope is rejected"),
		MonolithLocalizationTargetConfig::BuildGatherConfigPatch(
			InvalidScopeConfig,
			TEXT("Fixture"),
			{TEXT("%LOCPROJECTROOT%Source")},
			InvalidPatch,
			Error));
	TestTrue(
		TEXT("wrong target scope error is explicit"),
		Error.Contains(TEXT("source/destination")));

	const FString DuplicateStepConfig =
		ExistingConfig +
		TEXT("\r\n")
		TEXT("[gathertextstep0]\r\n")
		TEXT("CommandletClass=GenerateGatherManifest\r\n");
	Error.Reset();
	TestFalse(
		TEXT("duplicate gather step section name is rejected"),
		MonolithLocalizationTargetConfig::BuildGatherConfigPatch(
			DuplicateStepConfig,
			TEXT("Fixture"),
			{TEXT("%LOCPROJECTROOT%Source")},
			InvalidPatch,
			Error));
	TestTrue(
		TEXT("duplicate gather step error is explicit"),
		Error.Contains(TEXT("duplicate gather step")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithParamGuardLocalizationTargetSearchDirectoriesMalformedParamsTest,
	"Monolith.ParamGuard.MonolithConfig.LocalizationTargetSearchDirectoriesRejectsMalformedParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetSearchDirectoriesMalformedParamsTest::RunTest(
	const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetStringField(TEXT("search_directories"), TEXT("%LOCENGINEROOT%Source"));

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("non-array search_directories is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("non-array error identifies search_directories"),
			Result.ErrorMessage.Contains(TEXT("search_directories")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetArrayField(
			TEXT("search_directories"),
			{MakeShared<FJsonValueString>(TEXT("%LOCPROJECTROOT%../Outside"))});

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("parent traversal search directory is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("parent traversal error identifies the path component"),
			Result.ErrorMessage.Contains(TEXT("path component")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetArrayField(
			TEXT("search_directories"),
			{
				MakeShared<FJsonValueString>(TEXT("%LOCENGINEROOT%Source/Runtime/InputCore")),
				MakeShared<FJsonValueString>(TEXT("%locengineroot%Source/Runtime/InputCore/"))
			});

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("normalized duplicate search directory is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("duplicate error is explicit"),
			Result.ErrorMessage.Contains(TEXT("Duplicate")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->RemoveField(TEXT("source_control_policy"));

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("missing source_control_policy is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("missing source-control policy error identifies the parameter"),
			Result.ErrorMessage.Contains(TEXT("source_control_policy")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetStringField(
			TEXT("source_control_policy"),
			TEXT("auto_checkout"));

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("automatic checkout policy is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("source-control policy error identifies the parameter"),
			Result.ErrorMessage.Contains(TEXT("source_control_policy")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->RemoveField(TEXT("target_changelist"));

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("missing target_changelist is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("missing changelist error identifies the parameter"),
			Result.ErrorMessage.Contains(TEXT("target_changelist")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetNumberField(TEXT("target_changelist"), 0);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("default target_changelist is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("default changelist error identifies the parameter"),
			Result.ErrorMessage.Contains(TEXT("target_changelist")));
	}

	{
		TSharedPtr<FJsonObject> Params =
			MakeValidLocalizationTargetSearchDirectoryParams();
		Params->SetNumberField(TEXT("target_changelist"), 1203.5);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_target_text_search_directories"),
				Params);
		TestFalse(TEXT("fractional target_changelist is rejected"), Result.bSuccess);
		TestTrue(
			TEXT("fractional changelist error identifies the parameter"),
			Result.ErrorMessage.Contains(TEXT("target_changelist")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationTargetPipelineWriteGateTest, "Monolith.ParamGuard.MonolithConfig.LocalizationTargetPipelineWriteGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetPipelineWriteGateTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("EngineOverrides"));

	const FMonolithActionResult Result =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("run_target_pipeline"), Params);
	TestFalse(TEXT("run_target_pipeline rejects mutation without dry_run or confirm"), Result.bSuccess);
	TestTrue(TEXT("write gate error mentions dry_run or confirm"), Result.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationTargetPipelineDryRunTest, "Monolith.ParamGuard.MonolithConfig.LocalizationTargetPipelineDryRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetPipelineDryRunTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	const FString TargetName = TEXT("MonolithPipelineDryRunTest");
	FString GatherConfigPath;
	FString CompileConfigPath;
	ON_SCOPE_EXIT
	{
		if (!GatherConfigPath.IsEmpty())
		{
			IFileManager::Get().Delete(*GatherConfigPath, false, true, true);
		}
		if (!CompileConfigPath.IsEmpty())
		{
			IFileManager::Get().Delete(*CompileConfigPath, false, true, true);
		}
	};

	TestTrue(
		TEXT("gather config fixture is written"),
		WriteLocalizationPipelineConfigFixture(
			TargetName,
			TEXT("Gather"),
			TEXT("GatherTextFromSource"),
			GatherConfigPath));
	TestTrue(
		TEXT("compile config fixture is written"),
		WriteLocalizationPipelineConfigFixture(
			TargetName,
			TEXT("Compile"),
			TEXT("GenerateTextLocalizationResource"),
			CompileConfigPath));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TargetName);
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("run_target_pipeline"), Params);
	TestTrue(TEXT("run_target_pipeline dry_run succeeds without launching a child process"), Result.bSuccess);
	if (!Result.Result.IsValid())
	{
		AddError(TEXT("run_target_pipeline dry_run returned no result object"));
		return false;
	}

	TestEqual(TEXT("dry_run status is planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	TestEqual(TEXT("dry_run target is preserved"), Result.Result->GetStringField(TEXT("target")), TargetName);
	TestTrue(TEXT("dry_run reports ready for a target without generated outputs"), Result.Result->GetBoolField(TEXT("ready")));
	TestTrue(TEXT("dry_run reports changed=false"), !Result.Result->GetBoolField(TEXT("changed")));
	TestTrue(
		TEXT("dry_run reports child source control disabled"),
		!Result.Result->GetBoolField(TEXT("source_control_enabled_for_child")));
	TestEqual(TEXT("dry_run resolves two configs"), Result.Result->GetArrayField(TEXT("config_paths")).Num(), 2);
	TestFalse(TEXT("dry_run does not mint a job id"), Result.Result->HasField(TEXT("job_id")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationTargetPipelineMalformedParamsTest, "Monolith.ParamGuard.MonolithConfig.LocalizationTargetPipelineRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationTargetPipelineMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), TEXT("../EngineOverrides"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("run_target_pipeline"), Params);
		TestFalse(TEXT("target traversal is rejected"), Result.bSuccess);
		TestTrue(TEXT("target traversal reports target syntax"), Result.ErrorMessage.Contains(TEXT("target")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), TEXT("EngineOverrides"));
		Params->SetArrayField(
			TEXT("operations"),
			{
				MakeShared<FJsonValueString>(TEXT("compile")),
				MakeShared<FJsonValueString>(TEXT("gather"))
			});
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("run_target_pipeline"), Params);
		TestFalse(TEXT("compile before gather is rejected"), Result.bSuccess);
		TestTrue(TEXT("operation order error is explicit"), Result.ErrorMessage.Contains(TEXT("gather before compile")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), TEXT("EngineOverrides"));
		Params->SetArrayField(
			TEXT("operations"),
			{MakeShared<FJsonValueString>(TEXT("export"))});
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("run_target_pipeline"), Params);
		TestFalse(TEXT("unsupported operation is rejected"), Result.bSuccess);
		TestTrue(TEXT("unsupported operation reports allowed operations"), Result.ErrorMessage.Contains(TEXT("allowed values")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableMalformedParamsTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("source_string"), TEXT("Hello"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestFalse(TEXT("set_string_entry rejects missing key before asset load"), Result.bSuccess);
		TestTrue(TEXT("set_string_entry reports missing key"), Result.ErrorMessage.Contains(TEXT("key")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), TEXT("D:/MonolithOutsideExport.csv"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), Params);
		TestFalse(TEXT("export_string_table_csv rejects filesystem paths outside the project"), Result.bSuccess);
		TestTrue(TEXT("export_string_table_csv reports project directory scope"), Result.ErrorMessage.Contains(TEXT("project directory")));
	}

	{
		const FString CsvPath = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
			TEXT("MonolithTests/empty_string_table_import.csv"));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*CsvPath, false, true, true);
		};
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(TEXT("header-only CSV fixture is written"), FFileHelper::SaveStringToFile(TEXT("key,source_string\n"), *CsvPath));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvPath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects destructive empty replace"), Result.bSuccess);
		TestTrue(
			*FString::Printf(
				TEXT("import_string_table_csv reports replace_existing guard (actual error: %s)"),
				*Result.ErrorMessage),
			Result.ErrorMessage.Contains(TEXT("replace_existing")));
	}

	return true;
}
