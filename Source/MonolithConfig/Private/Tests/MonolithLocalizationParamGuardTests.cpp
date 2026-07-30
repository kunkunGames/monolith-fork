#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithAssetUtils.h"
#include "MonolithLocalizationActions.h"
#include "MonolithLocalizationTargetConfig.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 MaxExpectedValidationIssueRows = 200;

	void EnsureLocalizationActionsRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("localization"), TEXT("list_cultures")))
		{
			FMonolithLocalizationActions::RegisterActions(Registry);
		}
	}
}

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
	EnsureLocalizationActionsRegistered();

	TestTrue(TEXT("list_cultures action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("list_cultures")));
	TestTrue(TEXT("list_string_tables action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("list_string_tables")));
	TestTrue(TEXT("get_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("get_string_table")));
	TestTrue(TEXT("validate_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("validate_string_table")));
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
	EnsureLocalizationActionsRegistered();

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
	EnsureLocalizationActionsRegistered();

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
	EnsureLocalizationActionsRegistered();

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
		TestEqual(TEXT("outside path is an invalid-params error"), Result.ErrorCode, -32602);
	}

	{
		const FString CsvRelativePath = TEXT("Saved/MonolithTests/empty_string_table_import.csv");
		const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(TEXT("header-only CSV fixture is written"), FFileHelper::SaveStringToFile(TEXT("key,source_string\n"), *CsvPath));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*CsvPath, false, true);
		};

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects destructive empty replace"), Result.bSuccess);
		TestTrue(TEXT("import_string_table_csv reports replace_existing guard"), Result.ErrorMessage.Contains(TEXT("replace_existing")));
		TestEqual(TEXT("empty destructive import is an invalid-params error"), Result.ErrorCode, -32602);
	}

	{
		const FString CsvRelativePath = TEXT("Saved/MonolithTests/duplicate_string_table_headers.csv");
		const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(
			TEXT("duplicate-header CSV fixture is written"),
			FFileHelper::SaveStringToFile(TEXT("key,source_string,KEY\nOne,Hello,Corrupt\n"), *CsvPath));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*CsvPath, false, true);
		};

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects duplicate structural headers"), Result.bSuccess);
		TestTrue(TEXT("duplicate-header import reports the duplicated header"), Result.ErrorMessage.Contains(TEXT("duplicated")));
		TestEqual(TEXT("duplicate-header import is an invalid-params error"), Result.ErrorCode, -32602);
	}

	{
		const FString CsvRelativePath = TEXT("Saved/MonolithTests/whitespace_string_table_header.csv");
		const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(
			TEXT("whitespace-header CSV fixture is written"),
			FFileHelper::SaveStringToFile(TEXT("key,source_string, Owner \nOne,Hello,UI\n"), *CsvPath));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*CsvPath, false, true);
		};

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects metadata headers with edge whitespace"), Result.bSuccess);
		TestTrue(TEXT("whitespace-header import reports the ambiguity"), Result.ErrorMessage.Contains(TEXT("whitespace")));
		TestEqual(TEXT("whitespace-header import is an invalid-params error"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStrictJsonTypesTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStrictJsonTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStrictJsonTypesTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	auto ExpectInvalidParams = [this](const FString& Label, const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), Action, Params);
		TestFalse(*FString::Printf(TEXT("%s is rejected"), *Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s returns -32602"), *Label), Result.ErrorCode, -32602);
	};

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("20"));
		ExpectInvalidParams(TEXT("string limit"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1.5);
		ExpectInvalidParams(TEXT("fractional integer limit"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("include_entries"), TEXT("false"));
		ExpectInvalidParams(TEXT("string include_entries"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("include_derived"), TEXT("false"));
		ExpectInvalidParams(TEXT("string include_derived"), TEXT("list_cultures"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CultureNames;
		CultureNames.Add(MakeShared<FJsonValueBoolean>(true));
		Params->SetArrayField(TEXT("culture_names"), CultureNames);
		ExpectInvalidParams(TEXT("non-string culture_names element"), TEXT("list_cultures"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("culture_names"), TEXT("[\"en\"]"));
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("list_cultures"), Params);
		TestFalse(TEXT("JSON-encoded string culture_names is rejected"), Result.bSuccess);
		TestEqual(TEXT("JSON-encoded string culture_names returns -32602"), Result.ErrorCode, -32602);
		TestTrue(TEXT("JSON-encoded string culture_names reports exact array contract"), Result.ErrorMessage.Contains(TEXT("culture_names")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
		ExpectInvalidParams(TEXT("null path"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 17.0);
		Params->SetBoolField(TEXT("dry_run"), true);
		ExpectInvalidParams(TEXT("numeric asset_path"), TEXT("create_string_table"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_StrictType"));
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		ExpectInvalidParams(TEXT("string dry_run"), TEXT("create_string_table"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_Package.ObjectMismatch"));
		Params->SetBoolField(TEXT("dry_run"), true);
		ExpectInvalidParams(TEXT("mismatched package and object names"), TEXT("create_string_table"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("key"), TEXT("Strict.Metadata"));
		Params->SetStringField(TEXT("source_string"), TEXT("Must not load"));
		Params->SetStringField(TEXT("metadata"), TEXT("{\"Context\":\"Injected\"}"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestFalse(TEXT("JSON-encoded string metadata is rejected"), Result.bSuccess);
		TestEqual(TEXT("JSON-encoded string metadata returns -32602"), Result.ErrorCode, -32602);
		TestTrue(TEXT("JSON-encoded string metadata fails before asset lookup"), Result.ErrorMessage.Contains(TEXT("metadata")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("key"), TEXT("Strict.MetadataWhitespace"));
		Params->SetStringField(TEXT("metadata_key"), TEXT(" Owner "));
		Params->SetStringField(TEXT("metadata_value"), TEXT("Must not load"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestFalse(TEXT("metadata keys with edge whitespace are rejected"), Result.bSuccess);
		TestEqual(TEXT("metadata whitespace returns -32602"), Result.ErrorCode, -32602);
		TestTrue(TEXT("metadata whitespace error is explicit"), Result.ErrorMessage.Contains(TEXT("whitespace")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1.0e20);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("list_string_tables"), Params);
		TestTrue(TEXT("finite integral limits above int32 clamp safely before conversion"), Result.bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableLifecycleTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableLifecycleTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetFolder = FString::Printf(TEXT("/Game/Tests/Monolith/Localization/Lifecycle_%s"), *Suffix);
	const FString AssetName = TEXT("ST_Primary");
	const FString AssetPath = FString::Printf(TEXT("%s/%s"), *AssetFolder, *AssetName);
	const FString ValidationAssetName = TEXT("ST_ValidationBudget");
	const FString ValidationAssetPath = FString::Printf(TEXT("%s/%s"), *AssetFolder, *ValidationAssetName);
	const FString CsvRelativePath = FString::Printf(TEXT("Saved/MonolithTests/Localization/%s.csv"), *AssetName);
	const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));

	UStringTable* CreatedTable = nullptr;
	UStringTable* ValidationTable = nullptr;
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*CsvPath, false, true);
		if (CreatedTable)
		{
			FStringTableRegistry::Get().UnregisterStringTable(CreatedTable->GetStringTableId());
			if (UPackage* Package = CreatedTable->GetOutermost())
			{
				Package->SetDirtyFlag(false);
			}
			CreatedTable->ClearFlags(RF_Public | RF_Standalone);
			CreatedTable->MarkAsGarbage();
		}
		if (ValidationTable)
		{
			FStringTableRegistry::Get().UnregisterStringTable(ValidationTable->GetStringTableId());
			if (UPackage* Package = ValidationTable->GetOutermost())
			{
				Package->SetDirtyFlag(false);
			}
			ValidationTable->ClearFlags(RF_Public | RF_Standalone);
			ValidationTable->MarkAsGarbage();
		}
	};

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("namespace"), TEXT("Monolith.Automation"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
		if (!TestTrue(TEXT("create_string_table creates an in-memory fixture"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
	}

	CreatedTable = Cast<UStringTable>(FMonolithAssetUtils::LoadAssetByPath(AssetPath));
	if (!TestNotNull(TEXT("created StringTable resolves by asset path"), CreatedTable))
	{
		return false;
	}
	TestEqual(TEXT("created StringTable namespace is applied"), CreatedTable->GetStringTable()->GetNamespace(), FString(TEXT("Monolith.Automation")));

	const FString EntryKey = TEXT("MainMenu.Play");
	{
		TSharedPtr<FJsonObject> BadMetadata = MakeShared<FJsonObject>();
		BadMetadata->SetBoolField(TEXT("Context"), true);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("source_string"), TEXT("MustNotWrite"));
		Params->SetObjectField(TEXT("metadata"), BadMetadata);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestFalse(TEXT("set_string_entry rejects non-string metadata value"), Result.bSuccess);
		TestEqual(TEXT("non-string metadata is an invalid-params error"), Result.ErrorCode, -32602);

		FString UnexpectedSource;
		TestFalse(TEXT("failed metadata validation does not create the entry"),
			CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), UnexpectedSource));
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
	CreatedTable->GetMutableStringTable()->SetSourceString(
		FTextKey(EntryKey),
		TEXT("Before action update"),
		TEXT("Preserve this translator context"));
#endif

	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(TEXT("Context"), TEXT("Main menu button"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("source_string"), TEXT("Play"));
		Params->SetObjectField(TEXT("metadata"), Metadata);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestTrue(TEXT("set_string_entry writes source and metadata"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), TEXT("Aardvark.First"));
		Params->SetStringField(TEXT("source_string"), TEXT("First"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestTrue(TEXT("second entry is inserted after the primary entry"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("include_metadata"), false);
		Params->SetNumberField(TEXT("limit"), 1);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("get_string_table"), Params);
		if (TestTrue(TEXT("capped get_string_table succeeds"), Result.bSuccess) && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (TestTrue(TEXT("capped get_string_table returns entries"), Result.Result->TryGetArrayField(TEXT("entries"), Entries) && Entries))
			{
				TestEqual(TEXT("entries are sorted by key before the limit is applied"), Entries->Num(), 1);
				if (Entries->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* FirstEntry = nullptr;
					if (TestTrue(TEXT("first capped entry is an object"), (*Entries)[0]->TryGetObject(FirstEntry) && FirstEntry))
					{
						FString FirstKey;
						(*FirstEntry)->TryGetStringField(TEXT("key"), FirstKey);
						TestEqual(TEXT("alphabetically first key is returned"), FirstKey, FString(TEXT("Aardvark.First")));
					}
				}
			}
		}
	}

	FString SourceString;
	TestTrue(TEXT("written entry exists"), CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), SourceString));
	TestEqual(TEXT("written source string round-trips"), SourceString, FString(TEXT("Play")));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
	const FStringTableEntryConstPtr UpdatedEntry = CreatedTable->GetStringTable()->FindEntry(FTextKey(EntryKey));
	if (TestTrue(TEXT("updated UE 5.8 StringTable entry exists"), UpdatedEntry.IsValid()))
	{
		TestEqual(
			TEXT("set_string_entry preserves UE 5.8 developer notes"),
			UpdatedEntry->GetDevNotes(),
			FString(TEXT("Preserve this translator context")));
	}
#endif

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), ValidationAssetPath);
		Params->SetStringField(TEXT("namespace"), TEXT("Monolith.Automation.Validation"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
		if (!TestTrue(TEXT("validation-budget StringTable fixture is created"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
	}

	ValidationTable = Cast<UStringTable>(FMonolithAssetUtils::LoadAssetByPath(ValidationAssetPath));
	if (!TestNotNull(TEXT("validation-budget StringTable resolves by asset path"), ValidationTable))
	{
		return false;
	}
	for (int32 Index = 0; Index < MaxExpectedValidationIssueRows + 5; ++Index)
	{
		const FTextKey ValidationKey(FString::Printf(TEXT("Empty.%03d"), Index));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		ValidationTable->GetMutableStringTable()->SetSourceString(ValidationKey, FString(), FString());
#else
		ValidationTable->GetMutableStringTable()->SetSourceString(
			ValidationKey,
			FString());
#endif
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), AssetFolder);
		Params->SetBoolField(TEXT("include_entries"), true);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetNumberField(TEXT("limit"), 2);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("list_string_tables"), Params);
		if (TestTrue(TEXT("list_string_tables aggregate entry budget executes"), Result.bSuccess) && Result.Result.IsValid())
		{
			TestEqual(
				TEXT("two table summaries are returned"),
				static_cast<int32>(Result.Result->GetIntegerField(TEXT("returned_count"))),
				2);
			TestEqual(
				TEXT("aggregate serialized entries do not exceed limit"),
				static_cast<int32>(Result.Result->GetIntegerField(TEXT("returned_entry_count"))),
				2);
			TestTrue(
				TEXT("aggregate result reports entries omitted by the shared budget"),
				Result.Result->GetIntegerField(TEXT("truncated_entry_count")) > 0);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), ValidationAssetPath);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("validate_string_table"), Params);
		if (TestTrue(TEXT("validation issue cap executes"), Result.bSuccess) && Result.Result.IsValid())
		{
			TestEqual(
				TEXT("validation reports the full issue total"),
				static_cast<int32>(Result.Result->GetIntegerField(TEXT("issue_count"))),
				MaxExpectedValidationIssueRows + 5);
			TestEqual(
				TEXT("validation returns at most the bounded issue rows"),
				static_cast<int32>(Result.Result->GetIntegerField(TEXT("returned_issue_count"))),
				MaxExpectedValidationIssueRows);
			TestEqual(
				TEXT("validation reports the truncated issue count"),
				static_cast<int32>(Result.Result->GetIntegerField(TEXT("truncated_issue_count"))),
				5);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("metadata_key"), TEXT("Owner"));
		Params->SetStringField(TEXT("metadata_value"), TEXT("UI"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestTrue(TEXT("set_string_metadata writes one metadata field"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("metadata_key"), TEXT("EmptyAllowed"));
		Params->SetStringField(TEXT("metadata_value"), TEXT(""));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		bool bWouldChange = false;
		if (TestTrue(TEXT("empty metadata dry-run succeeds"), Result.bSuccess) && Result.Result.IsValid())
		{
			Result.Result->TryGetBoolField(TEXT("would_change"), bWouldChange);
		}
		TestTrue(TEXT("absent metadata with an empty value is reported as a change"), bWouldChange);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("metadata_key"), TEXT("EmptyAllowed"));
		Params->SetStringField(TEXT("metadata_value"), TEXT(""));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestTrue(TEXT("set_string_metadata stores an empty value when the field was absent"), Result.bSuccess);

		bool bFoundEmptyMetadata = false;
		CreatedTable->GetStringTable()->EnumerateMetaData(
			FTextKey(EntryKey),
			[&bFoundEmptyMetadata](FName MetadataId, const FString& MetadataValue)
			{
				if (MetadataId == FName(TEXT("EmptyAllowed")))
				{
					bFoundEmptyMetadata = MetadataValue.IsEmpty();
					return false;
				}
				return true;
			});
		TestTrue(TEXT("empty metadata field exists after the confirmed call"), bFoundEmptyMetadata);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("metadata_key"), TEXT("key"));
		Params->SetStringField(TEXT("metadata_value"), TEXT("Reserved collision"));
		Params->SetBoolField(TEXT("confirm"), true);
		const FMonolithActionResult SetResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestTrue(TEXT("reserved-header metadata fixture is created"), SetResult.bSuccess);

		TSharedPtr<FJsonObject> ExportParams = MakeShared<FJsonObject>();
		ExportParams->SetStringField(TEXT("asset_path"), AssetPath);
		ExportParams->SetStringField(TEXT("file_path"), CsvRelativePath);
		ExportParams->SetBoolField(TEXT("include_metadata"), true);
		ExportParams->SetBoolField(TEXT("dry_run"), true);
		const FMonolithActionResult ExportResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), ExportParams);
		TestFalse(TEXT("CSV export rejects metadata that collides with a reserved header"), ExportResult.bSuccess);
		TestEqual(TEXT("reserved-header export returns -32602"), ExportResult.ErrorCode, -32602);
		TestTrue(TEXT("reserved-header export explains the conflict"), ExportResult.ErrorMessage.Contains(TEXT("reserved CSV header")));

		Params->SetBoolField(TEXT("remove"), true);
		Params->SetBoolField(TEXT("confirm"), true);
		Params->RemoveField(TEXT("metadata_value"));
		const FMonolithActionResult RemoveResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestTrue(TEXT("reserved-header metadata fixture is removed"), RemoveResult.bSuccess);
	}

	{
		CreatedTable->GetMutableStringTable()->SetMetaData(FTextKey(EntryKey), FName(TEXT(" Owner ")), TEXT("Ambiguous"));

		TSharedPtr<FJsonObject> ExportParams = MakeShared<FJsonObject>();
		ExportParams->SetStringField(TEXT("asset_path"), AssetPath);
		ExportParams->SetStringField(TEXT("file_path"), CsvRelativePath);
		ExportParams->SetBoolField(TEXT("include_metadata"), true);
		ExportParams->SetBoolField(TEXT("dry_run"), true);
		const FMonolithActionResult ExportResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), ExportParams);
		TestFalse(TEXT("CSV export rejects existing metadata keys with edge whitespace"), ExportResult.bSuccess);
		TestEqual(TEXT("metadata-whitespace export returns -32602"), ExportResult.ErrorCode, -32602);
		TestTrue(TEXT("metadata-whitespace export explains the ambiguity"), ExportResult.ErrorMessage.Contains(TEXT("whitespace")));

		CreatedTable->GetMutableStringTable()->RemoveMetaData(FTextKey(EntryKey), FName(TEXT(" Owner ")));
	}

	const FString LiteralApostropheKey = TEXT("'=Literal.Key");
	const FString FormulaSourceString = TEXT("=SUM(1,1)");
	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(TEXT("FormulaValue"), TEXT("+1+1"));
		Metadata->SetStringField(TEXT("LiteralApostrophe"), TEXT("'=literal"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), LiteralApostropheKey);
		Params->SetStringField(TEXT("source_string"), FormulaSourceString);
		Params->SetObjectField(TEXT("metadata"), Metadata);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("set_string_entry"),
				Params);
		TestTrue(
			TEXT("formula and literal-apostrophe CSV fixture is created"),
			Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("validate_string_table"), Params);
		TestTrue(TEXT("validate_string_table executes"), Result.bSuccess);
		bool bValid = false;
		if (Result.Result.IsValid())
		{
			Result.Result->TryGetBoolField(TEXT("valid"), bValid);
		}
		TestTrue(TEXT("populated StringTable validates cleanly"), bValid);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), Params);
		if (!TestTrue(TEXT("export_string_table_csv writes a project-scoped CSV"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("export_string_table_csv error: %s"), *Result.ErrorMessage));
		}
		TestTrue(TEXT("exported CSV exists"), IFileManager::Get().FileExists(*CsvPath));
	}

	{
		FString ExportedCsv;
		TestTrue(
			TEXT("exported CSV can be read for formula-guard inspection"),
			FFileHelper::LoadFileToString(ExportedCsv, *CsvPath));
		TestTrue(
			TEXT("formula cells use the unambiguous versioned spreadsheet guard"),
			ExportedCsv.Contains(TEXT("'__monolith_formula_guard_v1__:")));
		TestTrue(
			TEXT("literal apostrophe-plus-formula-looking content is not rewritten as a guard"),
			ExportedCsv.Contains(LiteralApostropheKey));
	}

	// Simulate an existing row that lacks a metadata field represented as
	// present-empty in the CSV. Value-only comparison used to mistake this for a
	// no-op because GetMetaData returns "" for both absent and present-empty.
	CreatedTable->GetMutableStringTable()->RemoveMetaData(
		FTextKey(EntryKey),
		FName(TEXT("EmptyAllowed")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		if (!TestTrue(TEXT("replace import succeeds without losing existing context"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("replace import error: %s"), *Result.ErrorMessage));
		}
		if (Result.Result.IsValid())
		{
			TestTrue(
				TEXT("present-empty metadata missing from the target is detected as a real change"),
				Result.Result->GetBoolField(TEXT("changed")));
		}
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
	{
		const FStringTableEntryConstPtr ReplacedEntry = CreatedTable->GetStringTable()->FindEntry(FTextKey(EntryKey));
		if (TestTrue(TEXT("replace-imported UE 5.8 entry exists"), ReplacedEntry.IsValid()))
		{
			TestEqual(
				TEXT("replace import preserves UE 5.8 developer notes for re-imported keys"),
				ReplacedEntry->GetDevNotes(),
				FString(TEXT("Preserve this translator context")));
		}
	}
#endif

	{
		bool bFoundEmptyMetadata = false;
		CreatedTable->GetStringTable()->EnumerateMetaData(
			FTextKey(EntryKey),
			[&bFoundEmptyMetadata](FName MetadataId, const FString& MetadataValue)
			{
				if (MetadataId == FName(TEXT("EmptyAllowed")))
				{
					bFoundEmptyMetadata = MetadataValue.IsEmpty();
					return false;
				}
				return true;
			});
		TestTrue(TEXT("replace CSV import preserves a present empty metadata value"), bFoundEmptyMetadata);

		bool bUnexpectedEmptyMetadata = false;
		CreatedTable->GetStringTable()->EnumerateMetaData(
			FTextKey(TEXT("Aardvark.First")),
			[&bUnexpectedEmptyMetadata](FName MetadataId, const FString&)
			{
				if (MetadataId == FName(TEXT("EmptyAllowed")))
				{
					bUnexpectedEmptyMetadata = true;
					return false;
				}
				return true;
			});
		TestFalse(TEXT("replace CSV import does not create absent empty metadata on another row"), bUnexpectedEmptyMetadata);
	}

	{
		FString FormulaReadback;
		TestTrue(
			TEXT("literal-apostrophe key survives export/import"),
			CreatedTable->GetStringTable()->GetSourceString(
				FTextKey(LiteralApostropheKey),
				FormulaReadback));
		TestEqual(
			TEXT("formula-looking source string round-trips exactly"),
			FormulaReadback,
			FormulaSourceString);

		bool bFoundFormulaMetadata = false;
		bool bFoundLiteralApostropheMetadata = false;
		CreatedTable->GetStringTable()->EnumerateMetaData(
			FTextKey(LiteralApostropheKey),
			[&bFoundFormulaMetadata, &bFoundLiteralApostropheMetadata](
				FName MetadataId,
				const FString& MetadataValue)
			{
				if (MetadataId == FName(TEXT("FormulaValue")))
				{
					bFoundFormulaMetadata = MetadataValue == TEXT("+1+1");
				}
				else if (MetadataId == FName(TEXT("LiteralApostrophe")))
				{
					bFoundLiteralApostropheMetadata =
						MetadataValue == TEXT("'=literal");
				}
				return true;
			});
		TestTrue(
			TEXT("formula-looking metadata round-trips exactly"),
			bFoundFormulaMetadata);
		TestTrue(
			TEXT("literal apostrophe metadata is never stripped as a guard"),
			bFoundLiteralApostropheMetadata);
	}

	{
		FString OriginalCsv;
		TestTrue(
			TEXT("original CSV is snapshotted for save-failure rollback coverage"),
			FFileHelper::LoadFileToString(OriginalCsv, *CsvPath));

		FString PackageFilename;
		TestTrue(
			TEXT("fixture package resolves to a filename"),
			FPackageName::TryConvertLongPackageNameToFilename(
				CreatedTable->GetOutermost()->GetName(),
				PackageFilename,
				FPackageName::GetAssetPackageExtension()));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);
		TUniquePtr<IFileHandle> PackageFileLock(
			FPlatformFileManager::Get().GetPlatformFile().OpenWrite(
				*PackageFilename,
				false,
				false));
		if (!TestTrue(
			TEXT("exclusive package handle is acquired for deterministic save failure"),
			PackageFileLock.IsValid()))
		{
			return false;
		}

		ON_SCOPE_EXIT
		{
			PackageFileLock.Reset();
			IFileManager::Get().Delete(*PackageFilename, false, true);
			FFileHelper::SaveStringToFile(OriginalCsv, *CsvPath);
		};

		TestTrue(
			TEXT("changed replacement CSV fixture is written"),
			FFileHelper::SaveStringToFile(
				TEXT("key,source_string\nMainMenu.Play,MustRollBack\n"),
				*CsvPath));

		UPackage* TablePackage = CreatedTable->GetOutermost();
		TablePackage->SetDirtyFlag(false);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);
		Params->SetBoolField(TEXT("save"), true);

		AddExpectedError(
			TEXT("Error moving file"),
			EAutomationExpectedErrorFlags::Contains,
			1);
		AddExpectedError(
			TEXT("MoveFile was unable to move"),
			EAutomationExpectedErrorFlags::Contains,
			10);
		AddExpectedError(
			TEXT("Failed to move"),
			EAutomationExpectedErrorFlags::Contains,
			1);
		AddExpectedError(
			TEXT("Error saving"),
			EAutomationExpectedErrorFlags::Contains,
			1);
		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(
				TEXT("localization"),
				TEXT("import_string_table_csv"),
				Params);
		TestFalse(
			TEXT("blocked package save returns an explicit action failure"),
			Result.bSuccess);
		TestFalse(
			TEXT("save-failure rollback restores the originally clean package state"),
			TablePackage->IsDirty());

		FString RestoredSource;
		TestTrue(
			TEXT("save-failure rollback restores the primary entry"),
			CreatedTable->GetStringTable()->GetSourceString(
				FTextKey(EntryKey),
				RestoredSource));
		TestEqual(
			TEXT("save-failure rollback restores the primary source string"),
			RestoredSource,
			FString(TEXT("Play")));
		TestTrue(
			TEXT("save-failure replace rollback restores entries omitted by the failed CSV"),
			CreatedTable->GetStringTable()->GetSourceString(
				FTextKey(LiteralApostropheKey),
				RestoredSource));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("remove_string_entry"), Params);
		TestTrue(TEXT("remove_string_entry removes the exported entry"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		if (!TestTrue(TEXT("import_string_table_csv restores the exported entry"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("import_string_table_csv error: %s"), *Result.ErrorMessage));
		}
	}

	SourceString.Reset();
	TestTrue(TEXT("imported entry exists"), CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), SourceString));
	TestEqual(TEXT("imported source string round-trips"), SourceString, FString(TEXT("Play")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetNumberField(TEXT("limit"), 20);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("get_string_table"), Params);
		TestTrue(TEXT("get_string_table reads the imported fixture"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("remove_string_entry"), Params);
		TestTrue(TEXT("final remove_string_entry succeeds"), Result.bSuccess);
	}

	return true;
}
