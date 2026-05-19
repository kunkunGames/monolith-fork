#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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

	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	TestTrue(TEXT("Result should always expose actors array"), Result.Result->TryGetArrayField(TEXT("actors"), Actors));
	TestTrue(TEXT("actors array should be empty when samples are disabled"), Actors != nullptr && Actors->Num() == 0);

	TestTrue(TEXT("Result should include an operator-facing message"), Result.Result->HasField(TEXT("message")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
