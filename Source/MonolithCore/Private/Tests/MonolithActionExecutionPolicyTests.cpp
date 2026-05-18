#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithCoreTools.h"
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

		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("explicit_action"),
			TEXT("Policy metadata explicit test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicySliceTestResult),
			nullptr,
			TEXT("Test"),
			MakePolicySliceExplicitMutationPolicy());

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
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	Settings->bEnableDeferredDomainCatalog = bOriginalCatalog;
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
