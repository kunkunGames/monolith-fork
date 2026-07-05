#include "Misc/AutomationTest.h"
#include "MonolithPlanExecutor.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	int32 GPlanTestMutationCount = 0;

	// InferExecutionPolicy special-cases the test namespace family so explicit
	// policies survive registration; "plantest" piggybacks on real inference rules
	// instead: read-like names stay read_only, apply_* infers a mutation policy.
	void RegisterPlanTestActions()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (Registry.HasAction(TEXT("plantest"), TEXT("get_value")))
		{
			return;
		}

		Registry.RegisterAction(TEXT("plantest"), TEXT("get_value"),
			TEXT("Plan executor test fixture: returns a nested value and an item array."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
			{
				TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
				Nested->SetStringField(TEXT("name"), TEXT("alpha"));
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetObjectField(TEXT("value"), Nested);
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const TCHAR* ItemName : { TEXT("alpha"), TEXT("beta") })
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), ItemName);
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
				Result->SetArrayField(TEXT("items"), Items);
				return FMonolithActionResult::Success(Result);
			}),
			FParamSchemaBuilder().Build());

		Registry.RegisterAction(TEXT("plantest"), TEXT("get_echo"),
			TEXT("Plan executor test fixture: echoes a required string param."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Params)
			{
				FString Name;
				if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name))
				{
					return FMonolithActionResult::Error(TEXT("'name' is required"));
				}
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("echoed"), Name);
				return FMonolithActionResult::Success(Result);
			}),
			FParamSchemaBuilder()
				.Required(TEXT("name"), TEXT("string"), TEXT("Echoed back"))
				.Build());

		Registry.RegisterAction(TEXT("plantest"), TEXT("apply_mutation"),
			TEXT("Plan executor test fixture: name-inferred mutating action; counts executions."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
			{
				++GPlanTestMutationCount;
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("applied"), true);
				return FMonolithActionResult::Success(Result);
			}),
			FParamSchemaBuilder().Build());
	}

	TSharedPtr<FJsonObject> MakeStep(const FString& Id, const FString& Namespace, const FString& Action,
		const TSharedPtr<FJsonObject>& StepParams = nullptr)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		if (!Id.IsEmpty())
		{
			Step->SetStringField(TEXT("id"), Id);
		}
		Step->SetStringField(TEXT("namespace"), Namespace);
		Step->SetStringField(TEXT("action"), Action);
		if (StepParams.IsValid())
		{
			Step->SetObjectField(TEXT("params"), StepParams);
		}
		return Step;
	}

	TSharedPtr<FJsonObject> MakePlanParams(const TArray<TSharedPtr<FJsonObject>>& Steps)
	{
		TArray<TSharedPtr<FJsonValue>> StepValues;
		for (const TSharedPtr<FJsonObject>& Step : Steps)
		{
			StepValues.Add(MakeShared<FJsonValueObject>(Step));
		}
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("steps"), StepValues);
		return Params;
	}

	const TSharedPtr<FJsonObject> GetStepRow(const FMonolithActionResult& Result, int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("steps"), Rows) && Rows && Rows->IsValidIndex(Index))
		{
			return (*Rows)[Index]->AsObject();
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorDryRunTest, "Monolith.Core.PlanExecutor.DryRunValidatesWithoutExecuting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorDryRunTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();
	const int32 MutationsBefore = GPlanTestMutationCount;

	TSharedPtr<FJsonObject> EchoParams = MakeShared<FJsonObject>();
	EchoParams->SetStringField(TEXT("name"), TEXT("$steps.s1.result.value.name"));
	TSharedPtr<FJsonObject> Params = MakePlanParams({
		MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
		MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), EchoParams),
		MakeStep(TEXT("s3"), TEXT("plantest"), TEXT("apply_mutation")) });
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
	TestTrue(TEXT("dry_run succeeds"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("dry_run status ok"), Result.Result->GetStringField(TEXT("status")), FString(TEXT("ok")));
		bool bRequiresConfirm = false;
		TestTrue(TEXT("requires_confirm present"), Result.Result->TryGetBoolField(TEXT("requires_confirm"), bRequiresConfirm));
		TestTrue(TEXT("mutating step flags requires_confirm"), bRequiresConfirm);
	}
	const TSharedPtr<FJsonObject> Row1 = GetStepRow(Result, 0);
	TestTrue(TEXT("step row present"), Row1.IsValid());
	if (Row1.IsValid())
	{
		TestEqual(TEXT("dry_run steps are planned"), Row1->GetStringField(TEXT("status")), FString(TEXT("planned")));
	}
	const TSharedPtr<FJsonObject> Row2 = GetStepRow(Result, 1);
	if (Row2.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
		TestTrue(TEXT("s2 reports its reference"), Row2->TryGetArrayField(TEXT("references"), Refs) && Refs && Refs->Num() == 1);
	}
	const TSharedPtr<FJsonObject> Row3 = GetStepRow(Result, 2);
	if (Row3.IsValid())
	{
		TestTrue(TEXT("apply_mutation classified mutating"), Row3->GetBoolField(TEXT("mutating")));
	}
	TestEqual(TEXT("dry_run executed nothing"), GPlanTestMutationCount, MutationsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorReferenceChainTest, "Monolith.Core.PlanExecutor.ResolvesStepReferences", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorReferenceChainTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();

	TSharedPtr<FJsonObject> EchoParams = MakeShared<FJsonObject>();
	EchoParams->SetStringField(TEXT("name"), TEXT("$steps.s1.result.value.name"));
	TSharedPtr<FJsonObject> Params = MakePlanParams({
		MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
		MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), EchoParams) });

	const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
	TestTrue(TEXT("plan succeeds"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("plan status ok"), Result.Result->GetStringField(TEXT("status")), FString(TEXT("ok")));
		TestEqual(TEXT("both steps succeeded"), Result.Result->GetIntegerField(TEXT("succeeded")), 2);
	}
	const TSharedPtr<FJsonObject> Row2 = GetStepRow(Result, 1);
	TestTrue(TEXT("s2 row present"), Row2.IsValid());
	if (Row2.IsValid())
	{
		const TSharedPtr<FJsonObject> StepResult = Row2->GetObjectField(TEXT("result"));
		TestTrue(TEXT("s2 result present"), StepResult.IsValid());
		if (StepResult.IsValid())
		{
			TestEqual(TEXT("reference resolved through nested path"),
				StepResult->GetStringField(TEXT("echoed")), FString(TEXT("alpha")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorConfirmGateTest, "Monolith.Core.PlanExecutor.ConfirmGateBlocksMutations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorConfirmGateTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();
	const int32 MutationsBefore = GPlanTestMutationCount;

	TSharedPtr<FJsonObject> Params = MakePlanParams({
		MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("apply_mutation")) });

	const FMonolithActionResult Blocked = FMonolithPlanExecutor::HandleExecutePlan(Params);
	TestFalse(TEXT("mutating plan without confirm fails"), Blocked.bSuccess);
	TestTrue(TEXT("gate error names confirm"), Blocked.ErrorMessage.Contains(TEXT("confirm=true")));
	if (Blocked.ErrorData.IsValid())
	{
		TestEqual(TEXT("gate failure cause"),
			Blocked.ErrorData->GetStringField(TEXT("failure_cause")), FString(TEXT("plan_requires_confirm")));
	}
	TestEqual(TEXT("gate blocked execution"), GPlanTestMutationCount, MutationsBefore);

	Params->SetBoolField(TEXT("confirm"), true);
	const FMonolithActionResult Confirmed = FMonolithPlanExecutor::HandleExecutePlan(Params);
	TestTrue(TEXT("confirmed plan succeeds"), Confirmed.bSuccess);
	TestEqual(TEXT("confirmed plan executed the mutation"), GPlanTestMutationCount, MutationsBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorValidationTest, "Monolith.Core.PlanExecutor.RejectsInvalidPlans", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorValidationTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();

	// Unknown action fails at plan time with the step id in the message.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_nonexistent")) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestFalse(TEXT("unknown action rejected"), Result.bSuccess);
		TestTrue(TEXT("unknown action error names the step"), Result.ErrorMessage.Contains(TEXT("'s1'")));
	}

	// Forward references fail at plan time.
	{
		TSharedPtr<FJsonObject> EchoParams = MakeShared<FJsonObject>();
		EchoParams->SetStringField(TEXT("name"), TEXT("$steps.s2.result.value.name"));
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_echo"), EchoParams),
			MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_value")) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestFalse(TEXT("forward reference rejected"), Result.bSuccess);
		TestTrue(TEXT("forward reference error is explicit"), Result.ErrorMessage.Contains(TEXT("not an earlier step")));
	}

	// Missing required params fail at plan time even when other steps are valid.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_echo")) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestFalse(TEXT("missing required param rejected"), Result.bSuccess);
		TestTrue(TEXT("missing param error names the param"), Result.ErrorMessage.Contains(TEXT("name")));
	}

	// Nested execute_plan steps are rejected.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("monolith"), TEXT("execute_plan")) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestFalse(TEXT("nested plan rejected"), Result.bSuccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorStopOnErrorTest, "Monolith.Core.PlanExecutor.StopOnErrorSkipsRemainingSteps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorStopOnErrorTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();

	TSharedPtr<FJsonObject> BadRefParams = MakeShared<FJsonObject>();
	BadRefParams->SetStringField(TEXT("name"), TEXT("$steps.s1.result.value.bogus_field"));
	TSharedPtr<FJsonObject> OkParams = MakeShared<FJsonObject>();
	OkParams->SetStringField(TEXT("name"), TEXT("plain"));

	// Default stop_on_error=true: the bad-reference step fails, the rest are skipped.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
			MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), BadRefParams),
			MakeStep(TEXT("s3"), TEXT("plantest"), TEXT("get_echo"), OkParams) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestTrue(TEXT("plan call itself succeeds with per-step statuses"), Result.bSuccess);
		if (Result.Result.IsValid())
		{
			TestEqual(TEXT("plan reports partial"), Result.Result->GetStringField(TEXT("status")), FString(TEXT("partial")));
			TestEqual(TEXT("one step skipped"), Result.Result->GetIntegerField(TEXT("skipped")), 1);
		}
		const TSharedPtr<FJsonObject> Row2 = GetStepRow(Result, 1);
		const TSharedPtr<FJsonObject> Row3 = GetStepRow(Result, 2);
		if (Row2.IsValid())
		{
			TestEqual(TEXT("bad reference step errors"), Row2->GetStringField(TEXT("status")), FString(TEXT("error")));
			TestTrue(TEXT("bad reference error lists available fields"), Row2->GetStringField(TEXT("error")).Contains(TEXT("available")));
		}
		if (Row3.IsValid())
		{
			TestEqual(TEXT("later step skipped"), Row3->GetStringField(TEXT("status")), FString(TEXT("skipped")));
		}
	}

	// stop_on_error=false: the independent third step still runs.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
			MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), BadRefParams),
			MakeStep(TEXT("s3"), TEXT("plantest"), TEXT("get_echo"), OkParams) });
		Params->SetBoolField(TEXT("stop_on_error"), false);
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		const TSharedPtr<FJsonObject> Row3 = GetStepRow(Result, 2);
		TestTrue(TEXT("continue-mode third row present"), Row3.IsValid());
		if (Row3.IsValid())
		{
			TestEqual(TEXT("independent step still runs"), Row3->GetStringField(TEXT("status")), FString(TEXT("ok")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorArrayIndexRefTest, "Monolith.Core.PlanExecutor.ResolvesArrayIndexReferences", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorArrayIndexRefTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();

	TSharedPtr<FJsonObject> EchoParams = MakeShared<FJsonObject>();
	EchoParams->SetStringField(TEXT("name"), TEXT("$steps.s1.result.items.1.name"));
	TSharedPtr<FJsonObject> Params = MakePlanParams({
		MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
		MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), EchoParams) });

	const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
	TestTrue(TEXT("array-index plan succeeds"), Result.bSuccess);
	const TSharedPtr<FJsonObject> Row2 = GetStepRow(Result, 1);
	TestTrue(TEXT("s2 row present"), Row2.IsValid());
	if (Row2.IsValid())
	{
		const TSharedPtr<FJsonObject> StepResult = Row2->GetObjectField(TEXT("result"));
		if (StepResult.IsValid())
		{
			TestEqual(TEXT("array index resolved"), StepResult->GetStringField(TEXT("echoed")), FString(TEXT("beta")));
		}
		else
		{
			AddError(TEXT("s2 result missing"));
		}
	}

	// Out-of-range index fails with an explicit error.
	TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("name"), TEXT("$steps.s1.result.items.7.name"));
	TSharedPtr<FJsonObject> BadPlan = MakePlanParams({
		MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")),
		MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_echo"), BadParams) });
	const FMonolithActionResult BadResult = FMonolithPlanExecutor::HandleExecutePlan(BadPlan);
	const TSharedPtr<FJsonObject> BadRow2 = GetStepRow(BadResult, 1);
	TestTrue(TEXT("bad index row present"), BadRow2.IsValid());
	if (BadRow2.IsValid())
	{
		TestEqual(TEXT("bad index errors"), BadRow2->GetStringField(TEXT("status")), FString(TEXT("error")));
		TestTrue(TEXT("bad index error names range"), BadRow2->GetStringField(TEXT("error")).Contains(TEXT("out of range")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPlanExecutorTransactionTest, "Monolith.Core.PlanExecutor.TransactionWrapsAndCancels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPlanExecutorTransactionTest::RunTest(const FString& Parameters)
{
	RegisterPlanTestActions();

	auto GetTransaction = [](const FMonolithActionResult& Result) -> TSharedPtr<FJsonObject>
	{
		return Result.Result.IsValid() ? Result.Result->GetObjectField(TEXT("transaction")) : nullptr;
	};

	// Read-only plan: transaction auto but nothing mutating → state none.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("get_value")) });
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		const TSharedPtr<FJsonObject> Transaction = GetTransaction(Result);
		TestTrue(TEXT("read-only transaction present"), Transaction.IsValid());
		if (Transaction.IsValid())
		{
			TestEqual(TEXT("read-only plan opens no transaction"), Transaction->GetStringField(TEXT("state")), FString(TEXT("none")));
		}
	}

	// Successful mutating plan commits.
	{
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("apply_mutation")) });
		Params->SetBoolField(TEXT("confirm"), true);
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestTrue(TEXT("mutating plan succeeds"), Result.bSuccess);
		const TSharedPtr<FJsonObject> Transaction = GetTransaction(Result);
		if (Transaction.IsValid() && GEditor)
		{
			TestEqual(TEXT("successful mutating plan commits"), Transaction->GetStringField(TEXT("state")), FString(TEXT("committed")));
		}
	}

	// Mid-plan failure after a mutation cancels the transaction and stamps rollback markers.
	{
		TSharedPtr<FJsonObject> BadRefParams = MakeShared<FJsonObject>();
		BadRefParams->SetStringField(TEXT("name"), TEXT("$steps.s2.result.value.bogus_field"));
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("apply_mutation")),
			MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_value")),
			MakeStep(TEXT("s3"), TEXT("plantest"), TEXT("get_echo"), BadRefParams) });
		Params->SetBoolField(TEXT("confirm"), true);
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		TestTrue(TEXT("failing plan returns per-step statuses"), Result.bSuccess);
		const TSharedPtr<FJsonObject> Transaction = GetTransaction(Result);
		if (Transaction.IsValid() && GEditor)
		{
			TestEqual(TEXT("mid-plan failure cancels the transaction"), Transaction->GetStringField(TEXT("state")), FString(TEXT("cancelled")));
			TestTrue(TEXT("cancelled transaction carries the non-undoable caveat"), Transaction->HasField(TEXT("caveat")));
			const TSharedPtr<FJsonObject> Row1 = GetStepRow(Result, 0);
			if (Row1.IsValid())
			{
				TestEqual(TEXT("executed mutating step reports rollback"),
					Row1->GetStringField(TEXT("rolled_back")), FString(TEXT("editor_transaction")));
			}
			if (Result.Result.IsValid())
			{
				TestFalse(TEXT("no partial-state note when rolled back"), Result.Result->HasField(TEXT("partial_state_note")));
			}
		}
	}

	// transaction=off keeps the v1 behavior and markers.
	{
		TSharedPtr<FJsonObject> BadRefParams = MakeShared<FJsonObject>();
		BadRefParams->SetStringField(TEXT("name"), TEXT("$steps.s2.result.value.bogus_field"));
		TSharedPtr<FJsonObject> Params = MakePlanParams({
			MakeStep(TEXT("s1"), TEXT("plantest"), TEXT("apply_mutation")),
			MakeStep(TEXT("s2"), TEXT("plantest"), TEXT("get_value")),
			MakeStep(TEXT("s3"), TEXT("plantest"), TEXT("get_echo"), BadRefParams) });
		Params->SetBoolField(TEXT("confirm"), true);
		Params->SetStringField(TEXT("transaction"), TEXT("off"));
		const FMonolithActionResult Result = FMonolithPlanExecutor::HandleExecutePlan(Params);
		const TSharedPtr<FJsonObject> Transaction = GetTransaction(Result);
		TestTrue(TEXT("off transaction present"), Transaction.IsValid());
		if (Transaction.IsValid())
		{
			TestEqual(TEXT("transaction off is reported"), Transaction->GetStringField(TEXT("state")), FString(TEXT("off")));
		}
		if (Result.Result.IsValid())
		{
			TestTrue(TEXT("partial-state note present without rollback"), Result.Result->HasField(TEXT("partial_state_note")));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
