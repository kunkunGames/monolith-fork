// SPDX-License-Identifier: MIT
// Contract tests for bulk_fill / describe framework actions.

#include "Misc/AutomationTest.h"

#include "MonolithBulkFillRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const FString InventorylessNamespace = TEXT("__test_inventoryless");
	const FString InventoryNamespace = TEXT("__test_inventory");

	void RegisterTestAdapter(const FString& Namespace, FMonolithBulkFillRegistry::FListTargetsAdapter ListTargets = {})
	{
		FMonolithBulkFillRegistry::Get().RegisterAdapter(
			Namespace,
			[](const FBulkFillSpec& /*Spec*/)
			{
				FDryRunReport Report;
				Report.bWouldApply = false;
				return Report;
			},
			[](const FString& Target)
			{
				FSchemaDescriptor Descriptor;
				Descriptor.FieldPath = Target;
				Descriptor.TypeName = TEXT("test");
				Descriptor.ImportTextForm = TEXT("{}");
				return Descriptor;
			},
			MoveTemp(ListTargets));
	}

	FMonolithActionResult ExecuteListTargets(const FString& Namespace)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_namespace"), Namespace);
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("list_targets"), Params);
	}

	FMonolithActionResult ExecuteSchema(const FString& Namespace, const TOptional<FString>& Target = TOptional<FString>())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_namespace"), Namespace);
		if (Target.IsSet())
		{
			Params->SetStringField(TEXT("target"), Target.GetValue());
		}
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("schema"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeListTargetsOptionalInventoryTest,
	"Monolith.Core.Describe.ListTargetsOptionalInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeListTargetsOptionalInventoryTest::RunTest(const FString& /*Parameters*/)
{
	if (!FMonolithToolRegistry::Get().HasAction(TEXT("describe"), TEXT("list_targets")))
	{
		AddError(TEXT("describe.list_targets action is not registered"));
		return false;
	}

	RegisterTestAdapter(InventorylessNamespace);
	const FMonolithActionResult Result = ExecuteListTargets(InventorylessNamespace);
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(InventorylessNamespace);

	TestTrue(TEXT("registered namespace without inventory succeeds"), Result.bSuccess);
	TestTrue(TEXT("registered namespace returns JSON payload"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	bool bInventorySupported = true;
	FString Contract;
	double Count = -1.0;
	TestTrue(TEXT("inventory_supported field exists"),
		Result.Result->TryGetBoolField(TEXT("inventory_supported"), bInventorySupported));
	TestFalse(TEXT("inventory is explicitly unsupported"), bInventorySupported);
	TestTrue(TEXT("contract field exists"), Result.Result->TryGetStringField(TEXT("contract"), Contract));
	TestEqual(TEXT("optional inventory contract"), Contract, FString(TEXT("optional_inventory_not_implemented")));
	TestTrue(TEXT("count field exists"), Result.Result->TryGetNumberField(TEXT("count"), Count));
	TestEqual(TEXT("unsupported inventory returns zero count"), Count, 0.0);
	TestTrue(TEXT("message explains empty target array"), Result.Result->HasField(TEXT("message")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeListTargetsAuthoritativeInventoryTest,
	"Monolith.Core.Describe.ListTargetsAuthoritativeInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeListTargetsAuthoritativeInventoryTest::RunTest(const FString& /*Parameters*/)
{
	RegisterTestAdapter(
		InventoryNamespace,
		[]()
		{
			return TArray<FString>{ TEXT("alpha"), TEXT("beta") };
		});
	const FMonolithActionResult Result = ExecuteListTargets(InventoryNamespace);
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(InventoryNamespace);

	TestTrue(TEXT("registered namespace with inventory succeeds"), Result.bSuccess);
	TestTrue(TEXT("registered namespace returns JSON payload"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	bool bInventorySupported = false;
	FString Contract;
	double Count = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	Result.Result->TryGetBoolField(TEXT("inventory_supported"), bInventorySupported);
	Result.Result->TryGetStringField(TEXT("contract"), Contract);
	Result.Result->TryGetNumberField(TEXT("count"), Count);
	TestTrue(TEXT("inventory is supported"), bInventorySupported);
	TestEqual(TEXT("authoritative inventory contract"), Contract, FString(TEXT("authoritative_adapter_inventory")));
	TestEqual(TEXT("target count"), Count, 2.0);
	TestTrue(TEXT("targets array exists"), Result.Result->TryGetArrayField(TEXT("targets"), Targets));
	TestTrue(TEXT("targets array has two entries"), Targets && Targets->Num() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeSchemaNamespaceLevelTest,
	"Monolith.Core.Describe.SchemaNamespaceLevelWithoutTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeSchemaNamespaceLevelTest::RunTest(const FString& /*Parameters*/)
{
	RegisterTestAdapter(InventorylessNamespace);
	const FMonolithActionResult Result = ExecuteSchema(InventorylessNamespace);
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(InventorylessNamespace);

	TestTrue(TEXT("describe.schema succeeds without target"), Result.bSuccess);
	TestTrue(TEXT("describe.schema returns JSON payload"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("namespace-level descriptor receives empty target"), Result.Result->GetStringField(TEXT("field_path")), FString());
	TestEqual(TEXT("namespace-level descriptor type"), Result.Result->GetStringField(TEXT("type_name")), FString(TEXT("test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeListTargetsMissingNamespaceTest,
	"Monolith.Core.Describe.ListTargetsMissingNamespaceErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeListTargetsMissingNamespaceTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithActionResult Result = ExecuteListTargets(TEXT("__test_missing_inventory_namespace"));
	TestFalse(TEXT("missing namespace is not a successful empty inventory"), Result.bSuccess);
	TestTrue(TEXT("error names missing adapter"), Result.ErrorMessage.Contains(TEXT("no describe adapter registered")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDescribeActionSchemaMissingParamsTest,
	"Monolith.Core.Describe.ActionSchemaMissingParamsErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDescribeActionSchemaMissingParamsTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> NullParams;
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("action_schema"), NullParams);
	TestFalse(TEXT("describe.action_schema with missing params fails"), Result.bSuccess);
	TestTrue(TEXT("error names missing params"), Result.ErrorMessage.Contains(TEXT("describe.action_schema requires params")));
	TestEqual(TEXT("error code is invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
