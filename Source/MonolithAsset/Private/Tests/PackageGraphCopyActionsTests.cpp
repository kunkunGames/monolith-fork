#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAssetPackageGraphActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetPackageGraphRegistryTest,
	"Monolith.Asset.PackageGraph.RegistryAndParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetPackageGraphRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("asset"), TEXT("plan_package_graph_copy")))
	{
		FMonolithAssetPackageGraphActions::RegisterActions(Registry);
	}

	TestTrue(TEXT("asset.register_content_mount_points action is registered"), Registry.HasAction(TEXT("asset"), TEXT("register_content_mount_points")));
	TestTrue(TEXT("asset.plan_package_graph_copy action is registered"), Registry.HasAction(TEXT("asset"), TEXT("plan_package_graph_copy")));
	TestTrue(TEXT("asset.copy_package_graph_with_remap action is registered"), Registry.HasAction(TEXT("asset"), TEXT("copy_package_graph_with_remap")));
	TestTrue(TEXT("asset.copy_package_graph_with_strategy action is registered"), Registry.HasAction(TEXT("asset"), TEXT("copy_package_graph_with_strategy")));
	TestTrue(TEXT("asset.fixup_copied_references action is registered"), Registry.HasAction(TEXT("asset"), TEXT("fixup_copied_references")));
	TestTrue(TEXT("asset.validate_dependency_closure action is registered"), Registry.HasAction(TEXT("asset"), TEXT("validate_dependency_closure")));
	TestEqual(TEXT("register_content_mount_points tracks dirty packages without transaction wrapping"), Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("register_content_mount_points")).PolicyId, FString(TEXT("track_dirty_packages")));
	TestEqual(TEXT("plan_package_graph_copy is read-only"), Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("plan_package_graph_copy")).PolicyId, FString(TEXT("read_only")));
	TestEqual(TEXT("copy_package_graph_with_remap is guarded mutating"), Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("copy_package_graph_with_remap")).PolicyId, FString(TEXT("transaction_optional")));
	TestEqual(TEXT("copy_package_graph_with_strategy is guarded mutating"), Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("copy_package_graph_with_strategy")).PolicyId, FString(TEXT("transaction_optional")));
	TestEqual(TEXT("fixup_copied_references is guarded mutating"), Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("fixup_copied_references")).PolicyId, FString(TEXT("transaction_optional")));

	FMonolithActionResult MissingPlanParams = FMonolithAssetPackageGraphActions::PlanPackageGraphCopy(MakeShared<FJsonObject>());
	TestFalse(TEXT("plan_package_graph_copy rejects missing root_packages"), MissingPlanParams.bSuccess);
	TestEqual(TEXT("plan_package_graph_copy invalid param code"), MissingPlanParams.ErrorCode, -32602);

	FMonolithActionResult MissingClosureParams = FMonolithAssetPackageGraphActions::ValidateDependencyClosure(MakeShared<FJsonObject>());
	TestFalse(TEXT("validate_dependency_closure rejects missing destination_roots"), MissingClosureParams.bSuccess);
	TestEqual(TEXT("validate_dependency_closure invalid param code"), MissingClosureParams.ErrorCode, -32602);

	FMonolithActionResult MissingMountParams = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(MakeShared<FJsonObject>());
	TestFalse(TEXT("register_content_mount_points rejects missing mount_points"), MissingMountParams.bSuccess);
	TestEqual(TEXT("register_content_mount_points invalid param code"), MissingMountParams.ErrorCode, -32602);

	const FString TestRunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);

	TSharedPtr<FJsonObject> DuplicateResolverParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> DuplicateResolverSpecs;
	TSharedPtr<FJsonObject> DuplicateResolverSpec = MakeShared<FJsonObject>();
	DuplicateResolverSpec->SetStringField(TEXT("root"), TEXT("/MonolithDuplicateResolver/"));
	DuplicateResolverSpec->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
	DuplicateResolverSpec->SetStringField(TEXT("project_plugin_dir"), TEXT("MonolithDuplicateResolver"));
	DuplicateResolverSpecs.Add(MakeShared<FJsonValueObject>(DuplicateResolverSpec));
	DuplicateResolverParams->SetArrayField(TEXT("mount_points"), DuplicateResolverSpecs);
	FMonolithActionResult DuplicateResolver = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(DuplicateResolverParams);
	TestFalse(TEXT("register_content_mount_points rejects multiple resolvers"), DuplicateResolver.bSuccess);
	TestEqual(TEXT("register_content_mount_points duplicate resolver error code"), DuplicateResolver.ErrorCode, -32602);
	TestTrue(TEXT("register_content_mount_points duplicate resolver message"), DuplicateResolver.ErrorMessage.Contains(TEXT("exactly one resolver")));

	TSharedPtr<FJsonObject> InvalidProjectPluginDirParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> InvalidProjectPluginDirSpecs;
	TSharedPtr<FJsonObject> InvalidProjectPluginDirSpec = MakeShared<FJsonObject>();
	InvalidProjectPluginDirSpec->SetStringField(TEXT("root"), TEXT("/MonolithInvalidProjectPluginDir/"));
	InvalidProjectPluginDirSpec->SetStringField(TEXT("project_plugin_dir"), TEXT("../Saved"));
	InvalidProjectPluginDirSpecs.Add(MakeShared<FJsonValueObject>(InvalidProjectPluginDirSpec));
	InvalidProjectPluginDirParams->SetArrayField(TEXT("mount_points"), InvalidProjectPluginDirSpecs);
	FMonolithActionResult InvalidProjectPluginDir = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(InvalidProjectPluginDirParams);
	TestFalse(TEXT("register_content_mount_points rejects project_plugin_dir traversal"), InvalidProjectPluginDir.bSuccess);
	TestEqual(TEXT("register_content_mount_points traversal error code"), InvalidProjectPluginDir.ErrorCode, -32602);
	TestTrue(TEXT("register_content_mount_points traversal message"), InvalidProjectPluginDir.ErrorMessage.Contains(TEXT("project_plugin_dir")));

	TSharedPtr<FJsonObject> SameRequestConflictParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SameRequestConflictSpecs;
	TSharedPtr<FJsonObject> SameRequestConflictSpecA = MakeShared<FJsonObject>();
	SameRequestConflictSpecA->SetStringField(TEXT("root"), TEXT("/MonolithSameRequestConflict/"));
	SameRequestConflictSpecA->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
	SameRequestConflictSpecs.Add(MakeShared<FJsonValueObject>(SameRequestConflictSpecA));
	TSharedPtr<FJsonObject> SameRequestConflictSpecB = MakeShared<FJsonObject>();
	SameRequestConflictSpecB->SetStringField(TEXT("root"), TEXT("/MonolithSameRequestConflict/"));
	SameRequestConflictSpecB->SetStringField(TEXT("content_dir"), FPaths::ProjectSavedDir());
	SameRequestConflictSpecs.Add(MakeShared<FJsonValueObject>(SameRequestConflictSpecB));
	SameRequestConflictParams->SetArrayField(TEXT("mount_points"), SameRequestConflictSpecs);
	FMonolithActionResult SameRequestConflict = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(SameRequestConflictParams);
	TestFalse(TEXT("register_content_mount_points rejects same-request conflicting roots"), SameRequestConflict.bSuccess);
	TestEqual(TEXT("register_content_mount_points same-request conflict error code"), SameRequestConflict.ErrorCode, -32602);
	TestTrue(TEXT("register_content_mount_points same-request conflict message"), SameRequestConflict.ErrorMessage.Contains(TEXT("Conflicting mount point specs")));

	TSharedPtr<FJsonObject> MountWithoutGuardParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> GuardMountSpecs;
	TSharedPtr<FJsonObject> GuardMountSpec = MakeShared<FJsonObject>();
	GuardMountSpec->SetStringField(TEXT("root"), TEXT("/MonolithTestMount/"));
	GuardMountSpec->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
	GuardMountSpecs.Add(MakeShared<FJsonValueObject>(GuardMountSpec));
	MountWithoutGuardParams->SetArrayField(TEXT("mount_points"), GuardMountSpecs);
	MountWithoutGuardParams->SetBoolField(TEXT("dry_run"), false);
	FMonolithActionResult MountWithoutGuard = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(MountWithoutGuardParams);
	TestFalse(TEXT("register_content_mount_points requires confirm when dry_run is false"), MountWithoutGuard.bSuccess);
	TestEqual(TEXT("register_content_mount_points guard error code"), MountWithoutGuard.ErrorCode, -32602);
	TestTrue(TEXT("register_content_mount_points guard message"), MountWithoutGuard.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	TSharedPtr<FJsonObject> MountDryRunParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MountSpecs;
	TSharedPtr<FJsonObject> MountSpec = MakeShared<FJsonObject>();
	MountSpec->SetStringField(TEXT("root"), TEXT("/MonolithTestMount/"));
	MountSpec->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
	MountSpecs.Add(MakeShared<FJsonValueObject>(MountSpec));
	MountDryRunParams->SetArrayField(TEXT("mount_points"), MountSpecs);
	FMonolithActionResult MountDryRun = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(MountDryRunParams);
	TestTrue(TEXT("register_content_mount_points dry_run returns report"), MountDryRun.bSuccess);
	TestTrue(TEXT("register_content_mount_points dry_run returns json"), MountDryRun.Result.IsValid());
	if (MountDryRun.Result.IsValid())
	{
		bool bDryRun = false;
		MountDryRun.Result->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("register_content_mount_points defaults to dry_run"), bDryRun);
		TestTrue(TEXT("register_content_mount_points includes mount_points"), MountDryRun.Result->HasTypedField<EJson::Array>(TEXT("mount_points")));
		TestTrue(TEXT("register_content_mount_points includes probe rows"), MountDryRun.Result->HasTypedField<EJson::Array>(TEXT("probe_packages")));
		TestTrue(TEXT("register_content_mount_points points to plan action"), MountDryRun.Result->HasField(TEXT("next_recommended_action")));
	}
	TestFalse(TEXT("register_content_mount_points dry_run does not register mount"), FPackageName::MountPointExists(TEXT("/MonolithTestMount/")));

	auto NormalizeTestContentDir = [](FString ContentDir)
	{
		ContentDir = FPaths::ConvertRelativePathToFull(ContentDir);
		FPaths::NormalizeDirectoryName(ContentDir);
		if (!ContentDir.EndsWith(TEXT("/")))
		{
			ContentDir += TEXT("/");
		}
		return ContentDir;
	};
	auto SetStringArray = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	};

	const FString TempProjectPluginDirName = FString::Printf(TEXT("MonolithMountAutoPlugin_%s"), *TestRunId.Left(12));
	const FString TempProjectPluginRootDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TempProjectPluginDirName);
	const FString TempProjectPluginContent = NormalizeTestContentDir(FPaths::Combine(TempProjectPluginRootDir, TEXT("Content")));
	TestTrue(TEXT("register_content_mount_points temp project plugin content exists"), IFileManager::Get().MakeDirectory(*TempProjectPluginContent, true));

	TSharedPtr<FJsonObject> ProjectPluginDryRunParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ProjectPluginSpecs;
	TSharedPtr<FJsonObject> ProjectPluginSpec = MakeShared<FJsonObject>();
	ProjectPluginSpec->SetStringField(TEXT("root"), FString::Printf(TEXT("/MonolithProjectPlugin%s/"), *TestRunId.Left(12)));
	ProjectPluginSpec->SetStringField(TEXT("project_plugin_dir"), TempProjectPluginDirName);
	ProjectPluginSpecs.Add(MakeShared<FJsonValueObject>(ProjectPluginSpec));
	ProjectPluginDryRunParams->SetArrayField(TEXT("mount_points"), ProjectPluginSpecs);
	FMonolithActionResult ProjectPluginDryRun = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(ProjectPluginDryRunParams);
	TestTrue(TEXT("register_content_mount_points resolves project_plugin_dir in dry_run"), ProjectPluginDryRun.bSuccess);

	auto MakeMountParams = [&SetStringArray](const FString& Root, const FString& ContentDir, bool bDryRun, bool bConfirm, bool bScanAssetRegistry, const TArray<FString>& ProbePackages)
	{
		TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Specs;
		TSharedPtr<FJsonObject> SpecObject = MakeShared<FJsonObject>();
		SpecObject->SetStringField(TEXT("root"), Root);
		SpecObject->SetStringField(TEXT("content_dir"), ContentDir);
		Specs.Add(MakeShared<FJsonValueObject>(SpecObject));
		ParamsObject->SetArrayField(TEXT("mount_points"), Specs);
		ParamsObject->SetBoolField(TEXT("dry_run"), bDryRun);
		ParamsObject->SetBoolField(TEXT("confirm"), bConfirm);
		ParamsObject->SetBoolField(TEXT("scan_asset_registry"), bScanAssetRegistry);
		if (ProbePackages.Num() > 0)
		{
			SetStringArray(ParamsObject, TEXT("probe_packages"), ProbePackages);
		}
		return ParamsObject;
	};

	const FString MountRoot = FString::Printf(TEXT("/MonolithMountAuto%s/"), *TestRunId.Left(12));
	const FString MountBaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithMountAuto"), TestRunId);
	const FString MountContentA = NormalizeTestContentDir(FPaths::Combine(MountBaseDir, TEXT("A"), TEXT("Content")));
	const FString MountContentB = NormalizeTestContentDir(FPaths::Combine(MountBaseDir, TEXT("B"), TEXT("Content")));
	TestTrue(TEXT("register_content_mount_points test content dir A exists"), IFileManager::Get().MakeDirectory(*MountContentA, true));
	TestTrue(TEXT("register_content_mount_points test content dir B exists"), IFileManager::Get().MakeDirectory(*MountContentB, true));

	const FString MissingProbePackage = MountRoot + TEXT("MissingAsset");
	FMonolithActionResult ConfirmedMount = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(
		MakeMountParams(MountRoot, MountContentA, false, true, false, { MissingProbePackage }));
	TestTrue(TEXT("register_content_mount_points confirmed mount succeeds"), ConfirmedMount.bSuccess);
	TestTrue(TEXT("register_content_mount_points confirmed mount returns json"), ConfirmedMount.Result.IsValid());
	if (ConfirmedMount.Result.IsValid())
	{
		double RegisteredCount = 0.0;
		ConfirmedMount.Result->TryGetNumberField(TEXT("registered_count"), RegisteredCount);
		TestEqual(TEXT("register_content_mount_points confirmed mount registers one row"), static_cast<int32>(RegisteredCount), 1);
		TestTrue(TEXT("register_content_mount_points confirmed mount includes probes"), ConfirmedMount.Result->HasTypedField<EJson::Array>(TEXT("probe_packages")));
	}

	if (ConfirmedMount.bSuccess)
	{
		FMonolithActionResult RepeatMount = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(
			MakeMountParams(MountRoot, MountContentA, false, true, false, {}));
		TestTrue(TEXT("register_content_mount_points repeated mount succeeds idempotently"), RepeatMount.bSuccess);
		TestTrue(TEXT("register_content_mount_points repeated mount returns json"), RepeatMount.Result.IsValid());
		if (RepeatMount.Result.IsValid())
		{
			double AlreadyRegisteredCount = 0.0;
			RepeatMount.Result->TryGetNumberField(TEXT("already_registered_count"), AlreadyRegisteredCount);
			TestEqual(TEXT("register_content_mount_points repeated mount reports already_registered"), static_cast<int32>(AlreadyRegisteredCount), 1);
		}

		FMonolithActionResult ConflictMount = FMonolithAssetPackageGraphActions::RegisterContentMountPoints(
			MakeMountParams(MountRoot, MountContentB, false, true, false, {}));
		TestFalse(TEXT("register_content_mount_points rejects conflicting mount root"), ConflictMount.bSuccess);
		TestEqual(TEXT("register_content_mount_points conflict error code"), ConflictMount.ErrorCode, -32602);
		TestTrue(TEXT("register_content_mount_points conflict returns error data"), ConflictMount.ErrorData.IsValid());
		if (ConflictMount.ErrorData.IsValid())
		{
			FString Status;
			ConflictMount.ErrorData->TryGetStringField(TEXT("status"), Status);
			TestEqual(TEXT("register_content_mount_points conflict status"), Status, FString(TEXT("preflight_failed")));
			double ErrorCount = 0.0;
			ConflictMount.ErrorData->TryGetNumberField(TEXT("preflight_error_count"), ErrorCount);
			TestEqual(TEXT("register_content_mount_points conflict has one preflight error"), static_cast<int32>(ErrorCount), 1);
		}
	}
	FPackageName::UnRegisterMountPoint(MountRoot, MountContentA);
	IFileManager::Get().DeleteDirectory(*MountBaseDir, false, true);
	IFileManager::Get().DeleteDirectory(*TempProjectPluginRootDir, false, true);

	TSharedPtr<FJsonObject> MutatingParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MutatingRoots;
	MutatingRoots.Add(MakeShared<FJsonValueString>(TEXT("/Game/MonolithTests/AssetPackageGraph/Root")));
	MutatingParams->SetArrayField(TEXT("root_packages"), MutatingRoots);
	TSharedPtr<FJsonObject> MutatingRemaps = MakeShared<FJsonObject>();
	MutatingRemaps->SetStringField(TEXT("/Game/MonolithTests"), TEXT("/Game/MonolithTestsCopied"));
	MutatingParams->SetObjectField(TEXT("root_remaps"), MutatingRemaps);

	FMonolithActionResult CopyWithoutGuard = FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap(MutatingParams);
	TestFalse(TEXT("copy_package_graph_with_remap requires dry_run or confirm"), CopyWithoutGuard.bSuccess);
	TestEqual(TEXT("copy_package_graph_with_remap guard error code"), CopyWithoutGuard.ErrorCode, -32602);
	TestTrue(TEXT("copy_package_graph_with_remap guard message"), CopyWithoutGuard.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	FMonolithActionResult StrategyWithoutGuard = FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy(MutatingParams);
	TestFalse(TEXT("copy_package_graph_with_strategy requires dry_run or confirm"), StrategyWithoutGuard.bSuccess);
	TestEqual(TEXT("copy_package_graph_with_strategy guard error code"), StrategyWithoutGuard.ErrorCode, -32602);
	TestTrue(TEXT("copy_package_graph_with_strategy guard message"), StrategyWithoutGuard.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	FMonolithActionResult FixupWithoutGuard = FMonolithAssetPackageGraphActions::FixupCopiedReferences(MutatingParams);
	TestFalse(TEXT("fixup_copied_references requires dry_run or confirm"), FixupWithoutGuard.bSuccess);
	TestEqual(TEXT("fixup_copied_references guard error code"), FixupWithoutGuard.ErrorCode, -32602);
	TestTrue(TEXT("fixup_copied_references guard message"), FixupWithoutGuard.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	TSharedPtr<FJsonObject> PlanParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> RootPackages;
	RootPackages.Add(MakeShared<FJsonValueString>(TEXT("/Game/MonolithTests/AssetPackageGraph/Root")));
	PlanParams->SetArrayField(TEXT("root_packages"), RootPackages);
	TSharedPtr<FJsonObject> Remaps = MakeShared<FJsonObject>();
	Remaps->SetStringField(TEXT("/Game/MonolithTests"), TEXT("/Game/MonolithTestsCopied"));
	PlanParams->SetObjectField(TEXT("root_remaps"), Remaps);
	PlanParams->SetBoolField(TEXT("check_collisions"), false);

	FMonolithActionResult Plan = FMonolithAssetPackageGraphActions::PlanPackageGraphCopy(PlanParams);
	TestTrue(TEXT("plan_package_graph_copy returns a read-only plan for valid params"), Plan.bSuccess);
	TestTrue(TEXT("plan_package_graph_copy returns json"), Plan.Result.IsValid());
	if (Plan.Result.IsValid())
	{
		bool bReadOnly = false;
		Plan.Result->TryGetBoolField(TEXT("read_only"), bReadOnly);
		TestTrue(TEXT("plan reports read_only"), bReadOnly);
		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		TestTrue(TEXT("plan includes package_map"), Plan.Result->TryGetArrayField(TEXT("package_map"), PackageMap) && PackageMap != nullptr);
	}

	TSharedPtr<FJsonObject> CopyDryRunParams = MakeShared<FJsonObject>();
	CopyDryRunParams->Values = PlanParams->Values;
	CopyDryRunParams->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult CopyDryRun = FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap(CopyDryRunParams);
	TestTrue(TEXT("copy_package_graph_with_remap dry_run returns a plan/report"), CopyDryRun.bSuccess);
	TestTrue(TEXT("copy_package_graph_with_remap dry_run returns json"), CopyDryRun.Result.IsValid());
	if (CopyDryRun.Result.IsValid())
	{
		bool bDryRun = false;
		CopyDryRun.Result->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("copy_package_graph_with_remap reports dry_run"), bDryRun);
	}

	TSharedPtr<FJsonObject> StrategyPlanOnlyParams = MakeShared<FJsonObject>();
	StrategyPlanOnlyParams->Values = PlanParams->Values;
	StrategyPlanOnlyParams->SetStringField(TEXT("strategy"), TEXT("plan_only"));
	FMonolithActionResult StrategyPlanOnly = FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy(StrategyPlanOnlyParams);
	TestTrue(TEXT("copy_package_graph_with_strategy plan_only returns a plan"), StrategyPlanOnly.bSuccess);
	TestTrue(TEXT("copy_package_graph_with_strategy plan_only returns json"), StrategyPlanOnly.Result.IsValid());
	if (StrategyPlanOnly.Result.IsValid())
	{
		FString Status;
		StrategyPlanOnly.Result->TryGetStringField(TEXT("status"), Status);
		TestEqual(TEXT("copy_package_graph_with_strategy plan_only status"), Status, FString(TEXT("plan_only")));
		bool bReadOnly = false;
		StrategyPlanOnly.Result->TryGetBoolField(TEXT("read_only"), bReadOnly);
		TestTrue(TEXT("copy_package_graph_with_strategy plan_only reports read_only"), bReadOnly);
		const TArray<TSharedPtr<FJsonValue>>* Phases = nullptr;
		TestTrue(TEXT("copy_package_graph_with_strategy plan_only includes phases"), StrategyPlanOnly.Result->TryGetArrayField(TEXT("phases"), Phases) && Phases != nullptr);
	}

	TSharedPtr<FJsonObject> StrategyDryRunParams = MakeShared<FJsonObject>();
	StrategyDryRunParams->Values = PlanParams->Values;
	StrategyDryRunParams->SetStringField(TEXT("strategy"), TEXT("copy_fixup_validate"));
	StrategyDryRunParams->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult StrategyDryRun = FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy(StrategyDryRunParams);
	TestTrue(TEXT("copy_package_graph_with_strategy dry_run returns a report"), StrategyDryRun.bSuccess);
	TestTrue(TEXT("copy_package_graph_with_strategy dry_run returns json"), StrategyDryRun.Result.IsValid());
	if (StrategyDryRun.Result.IsValid())
	{
		bool bDryRun = false;
		StrategyDryRun.Result->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("copy_package_graph_with_strategy reports dry_run"), bDryRun);
		TestTrue(TEXT("copy_package_graph_with_strategy includes copy_report"), StrategyDryRun.Result->HasTypedField<EJson::Object>(TEXT("copy_report")));
		TestTrue(TEXT("copy_package_graph_with_strategy includes planned fixup params"), StrategyDryRun.Result->HasTypedField<EJson::Object>(TEXT("planned_fixup_params")));
		TestTrue(TEXT("copy_package_graph_with_strategy includes planned closure params"), StrategyDryRun.Result->HasTypedField<EJson::Object>(TEXT("planned_closure_params")));
	}

	TSharedPtr<FJsonObject> FixupDryRunParams = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> FixupRemaps = MakeShared<FJsonObject>();
	FixupRemaps->SetStringField(TEXT("/Game/MonolithTests"), TEXT("/Game/MonolithTestsCopied"));
	FixupDryRunParams->SetObjectField(TEXT("root_remaps"), FixupRemaps);
	FixupDryRunParams->SetBoolField(TEXT("dry_run"), true);
	TArray<TSharedPtr<FJsonValue>> FixupDestinationRoots;
	FixupDestinationRoots.Add(MakeShared<FJsonValueString>(TEXT("/Game/MonolithTestsCopied")));
	FixupDryRunParams->SetArrayField(TEXT("destination_roots"), FixupDestinationRoots);

	FMonolithActionResult FixupDryRun = FMonolithAssetPackageGraphActions::FixupCopiedReferences(FixupDryRunParams);
	TestTrue(TEXT("fixup_copied_references dry_run returns a report"), FixupDryRun.bSuccess);
	TestTrue(TEXT("fixup_copied_references dry_run returns json"), FixupDryRun.Result.IsValid());
	if (FixupDryRun.Result.IsValid())
	{
		bool bDryRun = false;
		FixupDryRun.Result->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("fixup_copied_references reports dry_run"), bDryRun);
	}

	TSharedPtr<FJsonObject> ClosureParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> DestinationRoots;
	DestinationRoots.Add(MakeShared<FJsonValueString>(TEXT("/Game/MonolithTestsCopied")));
	ClosureParams->SetArrayField(TEXT("destination_roots"), DestinationRoots);
	TArray<TSharedPtr<FJsonValue>> PackagePaths;
	PackagePaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/MonolithTestsCopied/Root")));
	ClosureParams->SetArrayField(TEXT("package_paths"), PackagePaths);
	TArray<TSharedPtr<FJsonValue>> AllowedExternalRoots;
	AllowedExternalRoots.Add(MakeShared<FJsonValueString>(TEXT("/Script")));
	AllowedExternalRoots.Add(MakeShared<FJsonValueString>(TEXT("/Engine")));
	ClosureParams->SetArrayField(TEXT("allowed_external_roots"), AllowedExternalRoots);

	FMonolithActionResult Closure = FMonolithAssetPackageGraphActions::ValidateDependencyClosure(ClosureParams);
	TestTrue(TEXT("validate_dependency_closure returns a report for valid params"), Closure.bSuccess);
	TestTrue(TEXT("validate_dependency_closure returns json"), Closure.Result.IsValid());
	if (Closure.Result.IsValid())
	{
		TestTrue(TEXT("closure has ok field"), Closure.Result->HasField(TEXT("ok")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
