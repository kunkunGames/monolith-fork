#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAssetPackageGraphActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

	TestTrue(TEXT("asset.plan_package_graph_copy action is registered"), Registry.HasAction(TEXT("asset"), TEXT("plan_package_graph_copy")));
	TestTrue(TEXT("asset.copy_package_graph_with_remap action is registered"), Registry.HasAction(TEXT("asset"), TEXT("copy_package_graph_with_remap")));
	TestTrue(TEXT("asset.copy_package_graph_with_strategy action is registered"), Registry.HasAction(TEXT("asset"), TEXT("copy_package_graph_with_strategy")));
	TestTrue(TEXT("asset.fixup_copied_references action is registered"), Registry.HasAction(TEXT("asset"), TEXT("fixup_copied_references")));
	TestTrue(TEXT("asset.validate_dependency_closure action is registered"), Registry.HasAction(TEXT("asset"), TEXT("validate_dependency_closure")));
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
