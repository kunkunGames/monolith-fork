#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorLiveCodingDiagnosticsShapeTest,
	"Monolith.Editor.LiveCoding.Diagnostics.Shape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorLiveCodingDiagnosticsShapeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("max_log_entries"), 5.0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("editor"),
		TEXT("get_live_coding_diagnostics"),
		Params);

	TestTrue(TEXT("Diagnostics action succeeds"), Result.bSuccess);
	if (!Result.Result.IsValid())
	{
		AddError(TEXT("Diagnostics action returned no JSON result."));
		return true;
	}

	TestTrue(TEXT("Availability field exists"), Result.Result->HasField(TEXT("live_coding_available")));
	TestTrue(TEXT("Normalized result field exists"), Result.Result->HasField(TEXT("compile_result_normalized")));
	TestTrue(TEXT("Diagnostic freshness field exists"), Result.Result->HasField(TEXT("diagnostic_source_fresh")));
	TestTrue(TEXT("UBT diagnostics field exists"), Result.Result->HasField(TEXT("ubt_diagnostics")));
	TestTrue(TEXT("Message field exists"), Result.Result->HasField(TEXT("message")));
	TestFalse(TEXT("Message is non-empty"), Result.Result->GetStringField(TEXT("message")).IsEmpty());
	TestEqual(TEXT("Max log entries is echoed"), Result.Result->GetNumberField(TEXT("max_log_entries")), 5.0);

	const TArray<TSharedPtr<FJsonValue>>* UbtDiagnostics = nullptr;
	TestTrue(TEXT("UBT diagnostics is an array"), Result.Result->TryGetArrayField(TEXT("ubt_diagnostics"), UbtDiagnostics));

	return true;
}
