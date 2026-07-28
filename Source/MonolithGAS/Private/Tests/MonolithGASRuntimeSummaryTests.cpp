#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithGASInternal.h"
#include "MonolithGASInspectActions.h"
#include "MonolithGASScaffoldActions.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASRuntimeSummaryPreflightShapeTest, "Monolith.GAS.RuntimeSummary.PreflightShape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASRuntimeSummaryPreflightShapeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetBoolField(TEXT("include_actor_samples"), false);
	Params->SetNumberField(TEXT("max_actors"), 0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("get_runtime_summary"), Params);
	TestTrue(TEXT("get_runtime_summary should succeed as a runtime preflight even when PIE is not active"), Result.bSuccess);
	TestTrue(TEXT("get_runtime_summary should return a JSON result"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	bool bPieActive = true;
	TestTrue(TEXT("Result should expose pie_active"), Result.Result->TryGetBoolField(TEXT("pie_active"), bPieActive));

	bool bHasRuntimeData = true;
	TestTrue(TEXT("Result should expose has_runtime_data"), Result.Result->TryGetBoolField(TEXT("has_runtime_data"), bHasRuntimeData));

	double ASCCount = -1.0;
	TestTrue(TEXT("Result should expose asc_count"), Result.Result->TryGetNumberField(TEXT("asc_count"), ASCCount));
	TestTrue(TEXT("ASC count should be non-negative"), ASCCount >= 0.0);

	double SampledASCCount = -1.0;
	TestTrue(TEXT("Result should expose sampled_asc_count"), Result.Result->TryGetNumberField(TEXT("sampled_asc_count"), SampledASCCount));
	TestEqual(TEXT("sampled_asc_count should honor include_actor_samples=false"), SampledASCCount, 0.0);

	bool bGasNamespaceRegistered = false;
	TestTrue(TEXT("Result should expose gas_namespace_registered"),
		Result.Result->TryGetBoolField(TEXT("gas_namespace_registered"), bGasNamespaceRegistered));

	double GASActionCount = -1.0;
	TestTrue(TEXT("Result should expose gas_action_count"),
		Result.Result->TryGetNumberField(TEXT("gas_action_count"), GASActionCount));
	TestTrue(TEXT("GAS action count should be non-negative"), GASActionCount >= 0.0);

	bool bProjectIndexAvailable = false;
	TestTrue(TEXT("Result should expose project_index_available"),
		Result.Result->TryGetBoolField(TEXT("project_index_available"), bProjectIndexAvailable));
	TestTrue(TEXT("Result should expose read_only_fallback"),
		Result.Result->HasTypedField(TEXT("read_only_fallback"), EJson::Object));

	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	TestTrue(TEXT("Result should always expose actors array"), Result.Result->TryGetArrayField(TEXT("actors"), Actors));
	TestTrue(TEXT("actors array should be empty when samples are disabled"), Actors != nullptr && Actors->Num() == 0);

	TestTrue(TEXT("Result should include an operator-facing message"), Result.Result->HasField(TEXT("message")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASEventCueProbeOutsidePIETest, "Monolith.GAS.EventCueProbe.OutsidePIEErrors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASEventCueProbeOutsidePIETest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("gas"), TEXT("start_event_cue_probe")))
	{
		FMonolithGASInspectActions::RegisterActions(Registry);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor"), TEXT("__MonolithMissingActor"));

	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("gas"), TEXT("start_event_cue_probe"), Params);
	TestFalse(TEXT("start_event_cue_probe should fail cleanly outside PIE"), Result.bSuccess);
	TestFalse(TEXT("start_event_cue_probe should report a no-PIE error"), Result.ErrorMessage.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASValidateSetupUsesResolvedProjectModuleTest, "Monolith.GAS.ValidateSetup.UsesResolvedProjectModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASValidateSetupUsesResolvedProjectModuleTest::RunTest(const FString& Parameters)
{
	MonolithGAS::FProjectCodeModuleInfo ModuleInfo;
	FString ResolveError;
	TestTrue(TEXT("Project code module should resolve from Source/*.Build.cs"),
		MonolithGAS::ResolveProjectCodeModule(ModuleInfo, &ResolveError));
	if (ModuleInfo.ModuleName.IsEmpty())
	{
		AddError(FString::Printf(TEXT("ResolveProjectCodeModule failed: %s"), *ResolveError));
		return false;
	}

	TestFalse(TEXT("Resolved module should prefer a runtime module over an editor module"),
		ModuleInfo.ModuleName.EndsWith(TEXT("Editor")));
	TestTrue(TEXT("Resolved Build.cs should exist"), FPaths::FileExists(ModuleInfo.BuildCSPath));

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("gas"), TEXT("validate_gas_setup")))
	{
		FMonolithGASScaffoldActions::RegisterActions(Registry);
	}

	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("gas"), TEXT("validate_gas_setup"), MakeShared<FJsonObject>());
	TestTrue(TEXT("validate_gas_setup should execute"), Result.bSuccess);
	TestTrue(TEXT("validate_gas_setup should return a JSON result"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	FString ReportedModule;
	FString ReportedBuildCSPath;
	TestTrue(TEXT("validate_gas_setup should report selected module_name"),
		Result.Result->TryGetStringField(TEXT("module_name"), ReportedModule));
	TestTrue(TEXT("validate_gas_setup should report selected build_cs_path"),
		Result.Result->TryGetStringField(TEXT("build_cs_path"), ReportedBuildCSPath));
	TestEqual(TEXT("validate_gas_setup should use the resolver-selected module"), ReportedModule, ModuleInfo.ModuleName);
	TestEqual(TEXT("validate_gas_setup should use the resolver-selected Build.cs"), ReportedBuildCSPath, ModuleInfo.BuildCSPath);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
