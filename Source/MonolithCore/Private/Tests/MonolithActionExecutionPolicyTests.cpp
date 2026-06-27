#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithCoreTools.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

void RegisterMonolithExecutionGuardActions();

namespace
{
	FMonolithActionResult MakePolicySliceTestResult(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionExecutionPolicy MakePolicySliceExplicitMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_required");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}

	FMonolithActionExecutionPolicy MakePolicySlicePostEditValidationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("post_edit_validate");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = true;
		Policy.bEnforced = true;
		return Policy;
	}

	FMonolithPostEditValidationResult MakePolicySlicePassingValidator(const FMonolithPostEditValidationContext& /*Context*/)
	{
		return FMonolithPostEditValidationResult::Passed(TEXT("policytest_validator"), TEXT("/Game/PolicyTest/BP_OK"));
	}

	FMonolithPostEditValidationResult MakePolicySliceFailingValidator(const FMonolithPostEditValidationContext& /*Context*/)
	{
		return FMonolithPostEditValidationResult::Failed(
			TEXT("failed_by_validator"),
			TEXT("policytest_validator"),
			TEXT("Policy test forced validator failure."),
			TEXT("/Game/PolicyTest/BP_Bad"));
	}

	void RegisterPolicySliceTestNamespace()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		Registry.UnregisterNamespace(TEXT("policytest"));
		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("default_action"),
			TEXT("Policy metadata default test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));

		FMonolithActionPlanningMetadata ExplicitPlanning;
		ExplicitPlanning.Skill = TEXT("monolith-mcp");
		ExplicitPlanning.Outputs = { TEXT("Policy test JSON payload with ok=true on success.") };
		ExplicitPlanning.NextActions = { TEXT("policytest.default_action") };

		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("explicit_action"),
			TEXT("Policy metadata explicit test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult),
			FParamSchemaBuilder()
				.EnableValidation()
				.Required(TEXT("target"), TEXT("string"), TEXT("Target identifier for discover schema filtering."))
				.Build(),
			TEXT("Test"),
			MakePolicySliceExplicitMutationPolicy(),
			FMonolithActionSearchMetadata(),
			ExplicitPlanning);

		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("post_edit_action"),
			TEXT("Policy metadata post-edit validation test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult),
			nullptr,
			TEXT("Test"),
			MakePolicySlicePostEditValidationPolicy());
	}

	const TSharedPtr<FJsonObject>* FindPolicySliceActionRow(const TArray<TSharedPtr<FJsonValue>>* Rows, const FString& ActionName)
	{
		if (!Rows)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FString ActualAction;
			FString ActualName;
			if (RowValue.IsValid()
				&& RowValue->TryGetObject(Row)
				&& Row
				&& Row->IsValid()
				&& (((*Row)->TryGetStringField(TEXT("action"), ActualAction) && ActualAction == ActionName)
					|| ((*Row)->TryGetStringField(TEXT("name"), ActualName) && ActualName == ActionName)))
			{
				return Row;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindPolicySlicePlanningSignal(const TSharedPtr<FJsonObject>& Row, const FString& Kind)
	{
		if (!Row.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Signals = nullptr;
		if (!Row->TryGetArrayField(TEXT("planning_signals"), Signals) || !Signals)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& SignalValue : *Signals)
		{
			TSharedPtr<FJsonObject> Signal = SignalValue.IsValid() ? SignalValue->AsObject() : nullptr;
			if (Signal.IsValid() && Signal->GetStringField(TEXT("kind")) == Kind)
			{
				return Signal;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyDiscoverTest,
	"Monolith.Core.ActionExecutionPolicy.DiscoverMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyDiscoverTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	FMonolithActionResult Result = FMonolithCoreTools::HandleDiscover(Params);
	TestTrue(TEXT("Discover succeeds"), Result.bSuccess);
	TestTrue(TEXT("Discover result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("Actions array exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));

		const TSharedPtr<FJsonObject>* DefaultRow = FindPolicySliceActionRow(Actions, TEXT("default_action"));
		TestTrue(TEXT("Default action row found"), DefaultRow && DefaultRow->IsValid());
		if (DefaultRow && DefaultRow->IsValid())
		{
			const TSharedPtr<FJsonObject>* Policy = nullptr;
			TestTrue(TEXT("Default policy object exists"), (*DefaultRow)->TryGetObjectField(TEXT("execution_policy"), Policy));
			if (Policy && Policy->IsValid())
			{
				TestEqual(TEXT("Default policy id"), (*Policy)->GetStringField(TEXT("policy_id")), TEXT("read_only"));
				TestTrue(TEXT("Default policy is marked defaulted"), (*Policy)->GetBoolField(TEXT("defaulted")));
				TestFalse(TEXT("Default policy is not enforced"), (*Policy)->GetBoolField(TEXT("enforced")));
			}
			TestEqual(TEXT("Default output contract is not invented"), (*DefaultRow)->GetStringField(TEXT("output_contract_status")), TEXT("not_declared"));
			TestEqual(TEXT("Default next actions are not invented"), (*DefaultRow)->GetStringField(TEXT("next_actions_status")), TEXT("not_declared"));
			TSharedPtr<FJsonObject> DefaultSchemaSignal = FindPolicySlicePlanningSignal(*DefaultRow, TEXT("schema"));
			TestTrue(TEXT("Default row has generated schema planning signal"), DefaultSchemaSignal.IsValid());
			if (DefaultSchemaSignal.IsValid())
			{
				TestEqual(TEXT("Default schema signal reports absent schema"), DefaultSchemaSignal->GetStringField(TEXT("status")), TEXT("absent"));
				TestEqual(TEXT("Default schema signal has zero required params"), static_cast<int32>(DefaultSchemaSignal->GetNumberField(TEXT("required_param_count"))), 0);
			}
		}

		const TSharedPtr<FJsonObject>* ExplicitRow = FindPolicySliceActionRow(Actions, TEXT("explicit_action"));
		TestTrue(TEXT("Explicit action row found"), ExplicitRow && ExplicitRow->IsValid());
		if (ExplicitRow && ExplicitRow->IsValid())
		{
			const TSharedPtr<FJsonObject>* Policy = nullptr;
			TestTrue(TEXT("Explicit policy object exists"), (*ExplicitRow)->TryGetObjectField(TEXT("execution_policy"), Policy));
			if (Policy && Policy->IsValid())
			{
				TestEqual(TEXT("Explicit policy id"), (*Policy)->GetStringField(TEXT("policy_id")), TEXT("transaction_required"));
				TestFalse(TEXT("Explicit policy is not defaulted"), (*Policy)->GetBoolField(TEXT("defaulted")));
				TestTrue(TEXT("Explicit policy requests transaction metadata"), (*Policy)->GetBoolField(TEXT("transaction_wrapping")));
				TestTrue(TEXT("Explicit policy is enforced"), (*Policy)->GetBoolField(TEXT("enforced")));
			}

			TestEqual(TEXT("Explicit row skill"), (*ExplicitRow)->GetStringField(TEXT("skill")), TEXT("monolith-mcp"));
			TestEqual(TEXT("Explicit output contract declared"), (*ExplicitRow)->GetStringField(TEXT("output_contract_status")), TEXT("declared"));
			TestEqual(TEXT("Explicit next actions declared"), (*ExplicitRow)->GetStringField(TEXT("next_actions_status")), TEXT("declared"));
			TSharedPtr<FJsonObject> ExplicitSchemaSignal = FindPolicySlicePlanningSignal(*ExplicitRow, TEXT("schema"));
			TestTrue(TEXT("Explicit row has generated schema planning signal"), ExplicitSchemaSignal.IsValid());
			if (ExplicitSchemaSignal.IsValid())
			{
				TestEqual(TEXT("Explicit schema signal reports declared schema"), ExplicitSchemaSignal->GetStringField(TEXT("status")), TEXT("declared"));
				TestEqual(TEXT("Explicit schema signal has one required param"), static_cast<int32>(ExplicitSchemaSignal->GetNumberField(TEXT("required_param_count"))), 1);
				const TArray<TSharedPtr<FJsonValue>>* RequiredSignalParams = nullptr;
				TestTrue(TEXT("Explicit schema signal names required param"),
					ExplicitSchemaSignal->TryGetArrayField(TEXT("required_params"), RequiredSignalParams) && RequiredSignalParams && RequiredSignalParams->Num() == 1);
			}
			TSharedPtr<FJsonObject> ExplicitPolicySignal = FindPolicySlicePlanningSignal(*ExplicitRow, TEXT("execution_policy"));
			TestTrue(TEXT("Explicit row has generated execution policy planning signal"), ExplicitPolicySignal.IsValid());
			if (ExplicitPolicySignal.IsValid())
			{
				TestEqual(TEXT("Explicit policy signal id"), ExplicitPolicySignal->GetStringField(TEXT("policy_id")), TEXT("transaction_required"));
				TestTrue(TEXT("Explicit policy signal reports mutation possibility"), ExplicitPolicySignal->GetBoolField(TEXT("can_mutate")));
			}

			const TArray<TSharedPtr<FJsonValue>>* Preconditions = nullptr;
			TestTrue(TEXT("Explicit preconditions array exists"), (*ExplicitRow)->TryGetArrayField(TEXT("preconditions"), Preconditions));
			if (Preconditions)
			{
				bool bSawTargetPrecondition = false;
				for (const TSharedPtr<FJsonValue>& Precondition : *Preconditions)
				{
					FString Text;
					if (Precondition.IsValid() && Precondition->TryGetString(Text) && Text.Contains(TEXT("target")))
					{
						bSawTargetPrecondition = true;
						break;
					}
				}
				TestTrue(TEXT("Required param appears as factual precondition"), bSawTargetPrecondition);
			}
			TestEqual(TEXT("Preconditions status is derived"), (*ExplicitRow)->GetStringField(TEXT("preconditions_status")), TEXT("declared_or_derived"));

			const TArray<TSharedPtr<FJsonValue>>* PreconditionDetails = nullptr;
			TestTrue(TEXT("Explicit precondition details array exists"), (*ExplicitRow)->TryGetArrayField(TEXT("precondition_details"), PreconditionDetails));
			if (PreconditionDetails)
			{
				bool bSawTargetDetail = false;
				for (const TSharedPtr<FJsonValue>& DetailValue : *PreconditionDetails)
				{
					const TSharedPtr<FJsonObject> Detail = DetailValue.IsValid() ? DetailValue->AsObject() : nullptr;
					if (Detail.IsValid()
						&& Detail->GetStringField(TEXT("source")) == TEXT("schema_required_param")
						&& Detail->GetStringField(TEXT("param")) == TEXT("target")
						&& Detail->GetStringField(TEXT("type")) == TEXT("string"))
					{
						bSawTargetDetail = true;
						break;
					}
				}
				TestTrue(TEXT("Required param precondition detail includes source and type"), bSawTargetDetail);
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyMetadataCoverageReportTest,
	"Monolith.Core.ActionExecutionPolicy.MetadataCoverageReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyMetadataCoverageReportTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	Params->SetNumberField(TEXT("sample_limit"), 5.0);
	FMonolithActionResult Result = FMonolithCoreTools::HandleGetActionMetadataCoverage(Params);
	TestTrue(TEXT("Coverage report succeeds"), Result.bSuccess);
	TestTrue(TEXT("Coverage result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Coverage status"), Result.Result->GetStringField(TEXT("status")), TEXT("ok"));
		TestEqual(TEXT("Coverage scope is active profile registry"), Result.Result->GetStringField(TEXT("scope")), TEXT("active_profile_registry"));
		TestTrue(TEXT("Coverage semantics warn against invented metadata"),
			Result.Result->GetStringField(TEXT("report_semantics")).Contains(TEXT("must not infer or fabricate")));

		const TSharedPtr<FJsonObject>* Totals = nullptr;
		TestTrue(TEXT("Coverage totals object exists"), Result.Result->TryGetObjectField(TEXT("totals"), Totals));
		if (Totals && Totals->IsValid())
		{
			TestEqual(TEXT("Coverage action count"), static_cast<int32>((*Totals)->GetNumberField(TEXT("action_count"))), 3);

			const TSharedPtr<FJsonObject>* Skill = nullptr;
			TestTrue(TEXT("Skill coverage object exists"), (*Totals)->TryGetObjectField(TEXT("skill"), Skill));
			if (Skill && Skill->IsValid())
			{
				TestEqual(TEXT("One action has declared skill"), static_cast<int32>((*Skill)->GetNumberField(TEXT("declared"))), 1);
				TestEqual(TEXT("Two actions derive skill from namespace"), static_cast<int32>((*Skill)->GetNumberField(TEXT("derived_from_namespace"))), 2);
			}

			const TSharedPtr<FJsonObject>* OutputStatus = nullptr;
			TestTrue(TEXT("Output status coverage object exists"), (*Totals)->TryGetObjectField(TEXT("output_contract_status"), OutputStatus));
			if (OutputStatus && OutputStatus->IsValid())
			{
				TestEqual(TEXT("Only explicit output contract is declared"), static_cast<int32>((*OutputStatus)->GetNumberField(TEXT("declared"))), 1);
				TestEqual(TEXT("Undeclared output contracts stay not_declared"), static_cast<int32>((*OutputStatus)->GetNumberField(TEXT("not_declared"))), 2);
			}

			const TSharedPtr<FJsonObject>* NextStatus = nullptr;
			TestTrue(TEXT("Next action status coverage object exists"), (*Totals)->TryGetObjectField(TEXT("next_actions_status"), NextStatus));
			if (NextStatus && NextStatus->IsValid())
			{
				TestEqual(TEXT("Only explicit next action is declared"), static_cast<int32>((*NextStatus)->GetNumberField(TEXT("declared"))), 1);
				TestEqual(TEXT("Undeclared next actions stay not_declared"), static_cast<int32>((*NextStatus)->GetNumberField(TEXT("not_declared"))), 2);
			}

			const TSharedPtr<FJsonObject>* PlanningSignalsStatus = nullptr;
			TestTrue(TEXT("Planning signals status coverage object exists"), (*Totals)->TryGetObjectField(TEXT("planning_signals_status"), PlanningSignalsStatus));
			if (PlanningSignalsStatus && PlanningSignalsStatus->IsValid())
			{
				TestEqual(TEXT("All actions receive generated planning signals"), static_cast<int32>((*PlanningSignalsStatus)->GetNumberField(TEXT("generated"))), 3);
			}

			const TSharedPtr<FJsonObject>* PreconditionsStatus = nullptr;
			TestTrue(TEXT("Preconditions status coverage object exists"), (*Totals)->TryGetObjectField(TEXT("preconditions_status"), PreconditionsStatus));
			if (PreconditionsStatus && PreconditionsStatus->IsValid())
			{
				TestEqual(TEXT("One action requires no params"), static_cast<int32>((*PreconditionsStatus)->GetNumberField(TEXT("none_required"))), 1);
				TestEqual(TEXT("Two actions have declared or derived preconditions"), static_cast<int32>((*PreconditionsStatus)->GetNumberField(TEXT("declared_or_derived"))), 2);
			}

			const TSharedPtr<FJsonObject>* PolicyFieldPresence = nullptr;
			TestTrue(TEXT("Policy field presence coverage object exists"), (*Totals)->TryGetObjectField(TEXT("policy_field_presence"), PolicyFieldPresence));
			if (PolicyFieldPresence && PolicyFieldPresence->IsValid())
			{
				TestEqual(TEXT("available_offline present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("available_offline:present"))), 3);
				TestEqual(TEXT("requires_live_editor present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("requires_live_editor:present"))), 3);
				TestEqual(TEXT("mutates_assets present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("mutates_assets:present"))), 3);
				TestEqual(TEXT("writes_logs present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("writes_logs:present"))), 3);
				TestEqual(TEXT("long_running present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("long_running:present"))), 3);
				TestEqual(TEXT("supports_progress present on every row"), static_cast<int32>((*PolicyFieldPresence)->GetNumberField(TEXT("supports_progress:present"))), 3);
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyMetadataCoverageGateTest,
	"Monolith.Core.ActionExecutionPolicy.MetadataCoverageGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyMetadataCoverageGateTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	Params->SetStringField(TEXT("gate_scope"), TEXT("filtered"));
	Params->SetNumberField(TEXT("min_contract_ratio"), 0.8);
	Params->SetNumberField(TEXT("sample_limit"), 5.0);
	FMonolithActionResult Result = FMonolithCoreTools::HandleGetActionMetadataCoverage(Params);
	TestTrue(TEXT("Coverage gate report succeeds"), Result.bSuccess);
	TestTrue(TEXT("Coverage gate result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Coverage status warns when filtered gate fails"), Result.Result->GetStringField(TEXT("status")), TEXT("warning"));

		const TSharedPtr<FJsonObject>* Gate = nullptr;
		TestTrue(TEXT("Coverage gate object exists"), Result.Result->TryGetObjectField(TEXT("gate"), Gate));
		if (Gate && Gate->IsValid())
		{
			bool bGatePassed = true;
			TestTrue(TEXT("Coverage gate passed field exists"), (*Gate)->TryGetBoolField(TEXT("passed"), bGatePassed));
			TestFalse(TEXT("Coverage gate fails below threshold"), bGatePassed);
			TestEqual(TEXT("Coverage gate evaluates one filtered bucket"), static_cast<int32>((*Gate)->GetNumberField(TEXT("check_count"))), 1);

			const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
			TestTrue(TEXT("Coverage gate checks exist"), (*Gate)->TryGetArrayField(TEXT("checks"), Checks));
			if (Checks && Checks->Num() == 1)
			{
				const TSharedPtr<FJsonObject>* Check = nullptr;
				TestTrue(TEXT("Coverage gate check is object"), (*Checks)[0]->TryGetObject(Check));
				if (Check && Check->IsValid())
				{
					TestEqual(TEXT("Gate output ratio reflects one declared action"), (*Check)->GetNumberField(TEXT("output_contract_ratio")), 1.0 / 3.0);
					TestEqual(TEXT("Gate next-actions ratio reflects one declared action"), (*Check)->GetNumberField(TEXT("next_actions_ratio")), 1.0 / 3.0);

					const TArray<TSharedPtr<FJsonValue>>* Failures = nullptr;
					TestTrue(TEXT("Coverage gate failures exist"), (*Check)->TryGetArrayField(TEXT("failures"), Failures));
					bool bSawOutputThresholdFailure = false;
					bool bSawNextThresholdFailure = false;
					if (Failures)
					{
						for (const TSharedPtr<FJsonValue>& Failure : *Failures)
						{
							const FString FailureText = Failure.IsValid() ? Failure->AsString() : FString();
							bSawOutputThresholdFailure |= FailureText == TEXT("output_contract_ratio_below_threshold");
							bSawNextThresholdFailure |= FailureText == TEXT("next_actions_ratio_below_threshold");
						}
					}
					TestTrue(TEXT("Gate reports output threshold failure"), bSawOutputThresholdFailure);
					TestTrue(TEXT("Gate reports next-actions threshold failure"), bSawNextThresholdFailure);
				}
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyDiscoverActionSchemaModeTest,
	"Monolith.Core.ActionExecutionPolicy.DiscoverActionSchemaMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyDiscoverActionSchemaModeTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	Params->SetStringField(TEXT("action"), TEXT("explicit_action"));
	Params->SetStringField(TEXT("mode"), TEXT("schema"));
	FMonolithActionResult Result = FMonolithCoreTools::HandleDiscover(Params);
	TestTrue(TEXT("Discover action schema succeeds"), Result.bSuccess);
	TestTrue(TEXT("Discover action schema result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Schema mode is echoed"), Result.Result->GetStringField(TEXT("mode")), TEXT("schema"));
		TestEqual(TEXT("Filtered action is echoed"), Result.Result->GetStringField(TEXT("action")), TEXT("explicit_action"));

		const TSharedPtr<FJsonObject>* Schema = nullptr;
		TestTrue(TEXT("Schema object exists"), Result.Result->TryGetObjectField(TEXT("schema"), Schema));
		if (Schema && Schema->IsValid())
		{
			TestEqual(TEXT("Schema row action"), (*Schema)->GetStringField(TEXT("action")), TEXT("explicit_action"));
			TestEqual(TEXT("Schema row output status"), (*Schema)->GetStringField(TEXT("output_contract_status")), TEXT("declared"));

			const TSharedPtr<FJsonObject>* ParamsSchema = nullptr;
			TestTrue(TEXT("Param schema object exists"), (*Schema)->TryGetObjectField(TEXT("params"), ParamsSchema));
			if (ParamsSchema && ParamsSchema->IsValid())
			{
				const TSharedPtr<FJsonObject>* TargetParam = nullptr;
				TestTrue(TEXT("Target param exists"), (*ParamsSchema)->TryGetObjectField(TEXT("target"), TargetParam));
				if (TargetParam && TargetParam->IsValid())
				{
					TestEqual(TEXT("Target param type"), (*TargetParam)->GetStringField(TEXT("type")), TEXT("string"));
					TestTrue(TEXT("Target param required"), (*TargetParam)->GetBoolField(TEXT("required")));
				}
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyDomainCatalogTest,
	"Monolith.Core.ActionExecutionPolicy.DomainMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyDomainCatalogTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalCatalog = Settings->bEnableDeferredDomainCatalog;
	Settings->bEnableDeferredDomainCatalog = true;

	RegisterPolicySliceTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	FMonolithActionResult Result = FMonolithCoreTools::HandleDescribeDomain(Params);
	TestTrue(TEXT("Describe domain succeeds"), Result.bSuccess);
	TestTrue(TEXT("Describe result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("Actions array exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));

		const TSharedPtr<FJsonObject>* ExplicitRow = FindPolicySliceActionRow(Actions, TEXT("explicit_action"));
		TestTrue(TEXT("Explicit action row found"), ExplicitRow && ExplicitRow->IsValid());
		if (ExplicitRow && ExplicitRow->IsValid())
		{
			const TSharedPtr<FJsonObject>* Policy = nullptr;
			TestTrue(TEXT("Explicit policy object exists"), (*ExplicitRow)->TryGetObjectField(TEXT("execution_policy"), Policy));
			if (Policy && Policy->IsValid())
			{
				TestEqual(TEXT("Domain policy id"), (*Policy)->GetStringField(TEXT("policy_id")), TEXT("transaction_required"));
			}
			TestEqual(TEXT("Domain row exposes output status"), (*ExplicitRow)->GetStringField(TEXT("output_contract_status")), TEXT("declared"));
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	Settings->bEnableDeferredDomainCatalog = bOriginalCatalog;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyInferredMutationTest,
	"Monolith.Core.ActionExecutionPolicy.InferredMutationMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyInferredMutationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.UnregisterNamespace(TEXT("policyinfer"));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("list_assets"),
		TEXT("Read-like inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("create_asset"),
		TEXT("Mutating inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("connect_pins"),
		TEXT("Graph connection inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("disconnect_pins"),
		TEXT("Graph disconnection inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("place_light"),
		TEXT("Implicit mesh placement inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("generate_floor_plan"),
		TEXT("Implicit floor-plan generation inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));
	Registry.RegisterAction(
		TEXT("policyinfer"),
		TEXT("edit_level_instance"),
		TEXT("Implicit level-instance edit inferred policy test action."),
		FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult));

	FMonolithActionExecutionPolicy ReadPolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("list_assets"));
	TestEqual(TEXT("Read-like action remains read_only"), ReadPolicy.PolicyId, TEXT("read_only"));
	TestFalse(TEXT("Read-like action does not track dirty packages"), ReadPolicy.bDirtyPackageTracking);
	TestFalse(TEXT("Read-like action does not wrap transactions"), ReadPolicy.bTransactionWrapping);

	FMonolithActionExecutionPolicy MutationPolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("create_asset"));
	TestEqual(TEXT("Mutating action infers transaction policy"), MutationPolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Mutating action tracks dirty packages"), MutationPolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Mutating action wraps transaction"), MutationPolicy.bTransactionWrapping);
	TestTrue(TEXT("Mutating action is enforced"), MutationPolicy.bEnforced);
	TestTrue(TEXT("Mutating inferred policy is still defaulted"), MutationPolicy.bDefaulted);

	FMonolithActionExecutionPolicy ConnectPolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("connect_pins"));
	TestEqual(TEXT("Graph connection action infers transaction policy"), ConnectPolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Graph connection action tracks dirty packages"), ConnectPolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Graph connection action wraps transaction"), ConnectPolicy.bTransactionWrapping);

	FMonolithActionExecutionPolicy DisconnectPolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("disconnect_pins"));
	TestEqual(TEXT("Graph disconnection action infers transaction policy"), DisconnectPolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Graph disconnection action tracks dirty packages"), DisconnectPolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Graph disconnection action wraps transaction"), DisconnectPolicy.bTransactionWrapping);

	FMonolithActionExecutionPolicy PlacePolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("place_light"));
	TestEqual(TEXT("Implicit placement action infers transaction policy"), PlacePolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Implicit placement action tracks dirty packages"), PlacePolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Implicit placement action wraps transaction"), PlacePolicy.bTransactionWrapping);

	FMonolithActionExecutionPolicy GeneratePolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("generate_floor_plan"));
	TestEqual(TEXT("Implicit generation action infers transaction policy"), GeneratePolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Implicit generation action tracks dirty packages"), GeneratePolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Implicit generation action wraps transaction"), GeneratePolicy.bTransactionWrapping);

	FMonolithActionExecutionPolicy EditPolicy = Registry.GetActionExecutionPolicy(TEXT("policyinfer"), TEXT("edit_level_instance"));
	TestEqual(TEXT("Implicit edit action infers transaction policy"), EditPolicy.PolicyId, TEXT("transaction_optional"));
	TestTrue(TEXT("Implicit edit action tracks dirty packages"), EditPolicy.bDirtyPackageTracking);
	TestTrue(TEXT("Implicit edit action wraps transaction"), EditPolicy.bTransactionWrapping);

	Registry.UnregisterNamespace(TEXT("policyinfer"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyAuditTest,
	"Monolith.Core.ActionExecutionPolicy.AuditMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyAuditTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();
	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();

	FMonolithActionExecutionGuard::FExecutionScope DefaultScope = Guard.BeginAction(TEXT("policytest"), TEXT("default_action"));
	TSharedPtr<FJsonObject> DefaultResultObject = MakeShared<FJsonObject>();
	DefaultResultObject->SetBoolField(TEXT("ok"), true);
	Guard.SetActionOutcome(DefaultScope, true, 0, DefaultResultObject, FString());
	Guard.EndAction(DefaultScope);

	FMonolithActionExecutionGuard::FExecutionScope Scope = Guard.BeginAction(TEXT("policytest"), TEXT("explicit_action"));
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetBoolField(TEXT("ok"), true);
	Guard.SetActionOutcome(Scope, true, 0, ResultObject, FString());
	Guard.EndAction(Scope);

	TSharedPtr<FJsonObject> Audit = Guard.GetRecentAuditJson(2);
	TestTrue(TEXT("Audit object valid"), Audit.IsValid());
	if (Audit.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("Audit rows exist"), Audit->TryGetArrayField(TEXT("rows"), Rows));
		TestTrue(TEXT("Two audit rows returned"), Rows && Rows->Num() == 2);
		if (Rows && Rows->Num() == 2)
		{
			const TSharedPtr<FJsonObject>* ExplicitRow = FindPolicySliceActionRow(Rows, TEXT("policytest.explicit_action"));
			TestTrue(TEXT("Explicit audit row found"), ExplicitRow && ExplicitRow->IsValid());
			if (ExplicitRow && ExplicitRow->IsValid())
			{
				const TSharedPtr<FJsonObject>* Policy = nullptr;
				TestTrue(TEXT("Audit row has execution policy"), (*ExplicitRow)->TryGetObjectField(TEXT("execution_policy"), Policy));
				if (Policy && Policy->IsValid())
				{
					TestEqual(TEXT("Audit policy id"), (*Policy)->GetStringField(TEXT("policy_id")), TEXT("transaction_required"));
					TestFalse(TEXT("Audit policy not defaulted"), (*Policy)->GetBoolField(TEXT("defaulted")));
					TestTrue(TEXT("Audit policy dirty tracking metadata"), (*Policy)->GetBoolField(TEXT("dirty_package_tracking")));
				}
				TestEqual(TEXT("Explicit audit row tracks dirty packages"), (*ExplicitRow)->GetStringField(TEXT("dirty_package_tracking_status")), TEXT("tracked_by_policy"));
				TestEqual(TEXT("Explicit audit row transaction requested"), (*ExplicitRow)->GetStringField(TEXT("transaction_status")), TEXT("requested_by_policy"));
			}

			const TSharedPtr<FJsonObject>* DefaultRow = FindPolicySliceActionRow(Rows, TEXT("policytest.default_action"));
			TestTrue(TEXT("Default audit row found"), DefaultRow && DefaultRow->IsValid());
			if (DefaultRow && DefaultRow->IsValid())
			{
				TestEqual(TEXT("Default audit row skips dirty package tracking"), (*DefaultRow)->GetStringField(TEXT("dirty_package_tracking_status")), TEXT("skipped_by_policy"));
				TestEqual(TEXT("Default audit row skips transactions"), (*DefaultRow)->GetStringField(TEXT("transaction_status")), TEXT("not_requested"));
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyOverrideTest,
	"Monolith.Core.ActionExecutionPolicy.RuntimeOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyOverrideTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();
	RegisterMonolithExecutionGuardActions();

	TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
	PolicyObject->SetStringField(TEXT("policy_id"), TEXT("track_dirty_packages"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("policytest.default_action"));
	Params->SetObjectField(TEXT("policy"), PolicyObject);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("set_action_execution_policy"), Params);
	TestTrue(TEXT("Override succeeds"), Result.bSuccess);
	TestTrue(TEXT("Override result valid"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Override status"), Result.Result->GetStringField(TEXT("status")), TEXT("ok"));
		TestTrue(TEXT("Override changed"), Result.Result->GetBoolField(TEXT("changed")));
	}

	FMonolithActionExecutionPolicy Policy = FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("policytest"), TEXT("default_action"));
	TestEqual(TEXT("Policy id updated"), Policy.PolicyId, TEXT("track_dirty_packages"));
	TestFalse(TEXT("Override policy is not defaulted"), Policy.bDefaulted);
	TestTrue(TEXT("Override policy tracks dirty packages"), Policy.bDirtyPackageTracking);
	TestFalse(TEXT("Override policy does not wrap transaction"), Policy.bTransactionWrapping);
	TestTrue(TEXT("Override policy is enforced"), Policy.bEnforced);

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyOverrideAcceptsValidationTest,
	"Monolith.Core.ActionExecutionPolicy.OverrideAcceptsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyOverrideAcceptsValidationTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();
	RegisterMonolithExecutionGuardActions();

	TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
	PolicyObject->SetStringField(TEXT("policy_id"), TEXT("post_edit_validate"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("policytest.default_action"));
	Params->SetObjectField(TEXT("policy"), PolicyObject);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("set_action_execution_policy"), Params);
	TestTrue(TEXT("Validator override succeeds"), Result.bSuccess);
	TestTrue(TEXT("Validator override result valid"), Result.Result.IsValid());

	FMonolithActionExecutionPolicy Policy = FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("policytest"), TEXT("default_action"));
	TestEqual(TEXT("Policy id updated to validator"), Policy.PolicyId, TEXT("post_edit_validate"));
	TestTrue(TEXT("Override policy tracks dirty packages"), Policy.bDirtyPackageTracking);
	TestTrue(TEXT("Override policy wraps transaction"), Policy.bTransactionWrapping);
	TestTrue(TEXT("Override policy requests validation"), Policy.bPostEditValidation);
	TestTrue(TEXT("Override policy is enforced"), Policy.bEnforced);

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyOverrideRejectsLegacyValidationFlagTest,
	"Monolith.Core.ActionExecutionPolicy.OverrideRejectsLegacyValidationFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyOverrideRejectsLegacyValidationFlagTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();
	RegisterMonolithExecutionGuardActions();

	TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
	PolicyObject->SetStringField(TEXT("policy_id"), TEXT("read_only"));
	PolicyObject->SetBoolField(TEXT("post_edit_validate"), true);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("policytest.default_action"));
	Params->SetObjectField(TEXT("policy"), PolicyObject);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("set_action_execution_policy"), Params);
	TestFalse(TEXT("Legacy validator flag is rejected"), Result.bSuccess);
	TestTrue(TEXT("Legacy validator rejection names field"), Result.ErrorMessage.Contains(TEXT("post_edit_validate")));

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionExecutionPolicyPostEditValidationHookTest,
	"Monolith.Core.ActionExecutionPolicy.PostEditValidationHook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionExecutionPolicyPostEditValidationHookTest::RunTest(const FString& Parameters)
{
	RegisterPolicySliceTestNamespace();
	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();

	FString RegisterError;
	TestTrue(
		TEXT("Passing validator registers"),
		Guard.RegisterPostEditValidator(
			TEXT("policytest"),
			TEXT("post_edit_action"),
			FMonolithPostEditValidator::CreateStatic(&MakePolicySlicePassingValidator),
			RegisterError));
	TestTrue(TEXT("Register error empty"), RegisterError.IsEmpty());

	FMonolithActionExecutionGuard::FExecutionScope Scope = Guard.BeginAction(TEXT("policytest"), TEXT("post_edit_action"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("asset_path"), TEXT("/Game/PolicyTest/BP_OK"));

	FMonolithPostEditValidationResult Validation = Guard.RunPostEditValidation(Scope, Params, ResultObject);
	TestTrue(TEXT("Validator passes"), Validation.bSuccess);
	TestEqual(TEXT("Validation status"), Scope.PostEditValidationStatus, TEXT("passed_by_validator"));
	Guard.SetActionOutcome(Scope, true, 0, ResultObject, FString());
	Guard.EndAction(Scope);

	TSharedPtr<FJsonObject> Audit = Guard.GetRecentAuditJson(1);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TestTrue(TEXT("Audit rows exist"), Audit.IsValid() && Audit->TryGetArrayField(TEXT("rows"), Rows));
	if (Rows && Rows->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		TestTrue(TEXT("Audit row object"), (*Rows)[0]->TryGetObject(Row));
		if (Row && Row->IsValid())
		{
			TestEqual(TEXT("Audit validation status"), (*Row)->GetStringField(TEXT("post_edit_validation_status")), TEXT("passed_by_validator"));
		}
	}

	Guard.ResetForTests();
	TestTrue(
		TEXT("Failing validator registers"),
		Guard.RegisterPostEditValidator(
			TEXT("policytest"),
			TEXT("post_edit_action"),
			FMonolithPostEditValidator::CreateStatic(&MakePolicySliceFailingValidator),
			RegisterError));

	FMonolithActionExecutionGuard::FExecutionScope FailingScope = Guard.BeginAction(TEXT("policytest"), TEXT("post_edit_action"));
	FMonolithPostEditValidationResult FailingValidation = Guard.RunPostEditValidation(FailingScope, Params, ResultObject);
	TestFalse(TEXT("Validator fails"), FailingValidation.bSuccess);
	TestEqual(TEXT("Failure validation status"), FailingScope.PostEditValidationStatus, TEXT("failed_by_validator"));
	TestTrue(TEXT("Failure message preserved"), FailingScope.PostEditValidationMessage.Contains(TEXT("forced validator failure")));
	Guard.SetActionOutcome(FailingScope, false, -32603, nullptr, FailingValidation.ErrorMessage);
	Guard.EndAction(FailingScope);

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
