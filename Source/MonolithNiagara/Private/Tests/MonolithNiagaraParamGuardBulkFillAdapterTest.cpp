#include "Misc/AutomationTest.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithNiagaraBulkFillAdapter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	static bool ReportHasReason(const FDryRunReport& Report, const FString& ExpectedReason)
	{
		for (const FBulkFillFieldWrite& Write : Report.FieldWrites)
		{
			if (Write.Reason.Contains(ExpectedReason))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardBulkFillAdapterTest, "Monolith.Niagara.ParamGuard.BulkFillAdapter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardBulkFillAdapterTest::RunTest(const FString& Parameters)
{
	FMonolithNiagaraBulkFillAdapter Adapter;

	// Test missing fill_kind
	{
		FBulkFillSpec Spec;
		Spec.TargetAsset = TEXT("/Game/Missing");
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TEXT("{}"));
		TSharedPtr<FJsonObject> JsonObj;
		FJsonSerializer::Deserialize(Reader, JsonObj);
		Spec.Tree = JsonObj;

		FDryRunReport Report = Adapter.NiagaraBulkFill(Spec);
		TestFalse(TEXT("Should fail on missing fill_kind"), Report.bWouldApply);
		TestTrue(TEXT("Should complain about fill_kind"),
			Report.Errors > 0 && ReportHasReason(Report, TEXT("fill_kind required")));
	}

	// Test missing user_param for DataInterfaceArray
	{
		FBulkFillSpec Spec;
		Spec.TargetAsset = TEXT("/Game/Missing");
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TEXT("{\"fill_kind\":\"DataInterfaceArray\"}"));
		TSharedPtr<FJsonObject> JsonObj;
		FJsonSerializer::Deserialize(Reader, JsonObj);
		Spec.Tree = JsonObj;

		FDryRunReport Report = Adapter.NiagaraBulkFill(Spec);
		TestFalse(TEXT("Should fail on missing user_param"), Report.bWouldApply);
		TestTrue(TEXT("Should complain about user_param"),
			Report.Errors > 0 && ReportHasReason(Report, TEXT("requires string 'user_param'")));
	}

	// Test missing curve_name for Curve
	{
		FBulkFillSpec Spec;
		Spec.TargetAsset = TEXT("/Game/Missing");
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TEXT("{\"fill_kind\":\"Curve\"}"));
		TSharedPtr<FJsonObject> JsonObj;
		FJsonSerializer::Deserialize(Reader, JsonObj);
		Spec.Tree = JsonObj;

		FDryRunReport Report = Adapter.NiagaraBulkFill(Spec);
		TestFalse(TEXT("Should fail on missing curve_name"), Report.bWouldApply);
		TestTrue(TEXT("Should complain about curve_name"),
			Report.Errors > 0 && ReportHasReason(Report, TEXT("requires string 'curve_name'")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
