#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithCoreTools.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FMonolithActionResult MakePolicyTestResult(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionExecutionPolicy MakeExplicitMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_required");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = false;
		return Policy;
	}

	void RegisterPolicyTestNamespace()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("default_action"),
			TEXT("Policy metadata default test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicyTestResult));

		Registry.RegisterAction(
			TEXT("policytest"),
			TEXT("explicit_action"),
			TEXT("Policy metadata explicit test action."),
			FMonolithActionHandler::CreateStatic(&MakePolicyTestResult),
			nullptr,
			TEXT("Test"),
			MakeExplicitMutationPolicy());
	}

	const TSharedPtr<FJsonObject>* FindActionRow(const TArray<TSharedPtr<FJsonValue>>* Rows, const FString& ActionName)
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
	RegisterPolicyTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	FMonolithActionResult Result = FMonolithCoreTools::HandleDiscover(Params);
	TestTrue(TEXT("Discover succeeds"), Result.bSuccess);
	TestTrue(TEXT("Discover result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("Actions array exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));

		const TSharedPtr<FJsonObject>* DefaultRow = FindActionRow(Actions, TEXT("default_action"));
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

		const TSharedPtr<FJsonObject>* ExplicitRow = FindActionRow(Actions, TEXT("explicit_action"));
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
				TestFalse(TEXT("Explicit policy is not enforced"), (*Policy)->GetBoolField(TEXT("enforced")));
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

	RegisterPolicyTestNamespace();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("policytest"));
	FMonolithActionResult Result = FMonolithCoreTools::HandleDescribeDomain(Params);
	TestTrue(TEXT("Describe domain succeeds"), Result.bSuccess);
	TestTrue(TEXT("Describe result valid"), Result.Result.IsValid());

	if (Result.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("Actions array exists"), Result.Result->TryGetArrayField(TEXT("actions"), Actions));

		const TSharedPtr<FJsonObject>* ExplicitRow = FindActionRow(Actions, TEXT("explicit_action"));
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
	RegisterPolicyTestNamespace();
	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();

	FMonolithActionExecutionGuard::FExecutionScope Scope = Guard.BeginAction(TEXT("policytest"), TEXT("explicit_action"));
	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetBoolField(TEXT("ok"), true);
	Guard.SetActionOutcome(Scope, true, 0, ResultObject, FString());
	Guard.EndAction(Scope);

	TSharedPtr<FJsonObject> Audit = Guard.GetRecentAuditJson(1);
	TestTrue(TEXT("Audit object valid"), Audit.IsValid());
	if (Audit.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("Audit rows exist"), Audit->TryGetArrayField(TEXT("rows"), Rows));
		TestTrue(TEXT("One audit row returned"), Rows && Rows->Num() == 1);
		if (Rows && Rows->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			TestTrue(TEXT("Audit row is object"), (*Rows)[0]->TryGetObject(Row));
			if (Row && Row->IsValid())
			{
				const TSharedPtr<FJsonObject>* Policy = nullptr;
				TestTrue(TEXT("Audit row has execution policy"), (*Row)->TryGetObjectField(TEXT("execution_policy"), Policy));
				if (Policy && Policy->IsValid())
				{
					TestEqual(TEXT("Audit policy id"), (*Policy)->GetStringField(TEXT("policy_id")), TEXT("transaction_required"));
					TestFalse(TEXT("Audit policy not defaulted"), (*Policy)->GetBoolField(TEXT("defaulted")));
					TestTrue(TEXT("Audit policy dirty tracking metadata"), (*Policy)->GetBoolField(TEXT("dirty_package_tracking")));
				}
			}
		}
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("policytest"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
