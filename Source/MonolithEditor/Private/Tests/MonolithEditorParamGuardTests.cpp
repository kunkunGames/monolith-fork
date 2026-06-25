#include "Misc/AutomationTest.h"
#include "MonolithPieTimeseries.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorSamplePieTimeseriesMalformedTest, "Monolith.ParamGuard.Editor.SamplePieTimeseriesMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSamplePieTimeseriesMalformedTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("actor"), TEXT("SomeActor"));
		TArray<TSharedPtr<FJsonValue>> VarPaths;
		VarPaths.Add(MakeShared<FJsonValueString>(TEXT("SomeVar")));
		Payload->SetArrayField(TEXT("variables"), VarPaths);

		// Malformed parameter: string instead of number
		Payload->SetStringField(TEXT("duration_seconds"), TEXT("not_a_number"));

		FMonolithActionResult Result = FMonolithPieTimeseries::HandleSamplePieTimeseries(Payload);

		TestFalse(TEXT("Malformed duration_seconds should return an error, not crash"), Result.bSuccess);
		TestTrue(TEXT("Error should come from parameter validation"), Result.ErrorMessage.Contains(TEXT("duration_seconds")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("actor"), TEXT("SomeActor"));
		TArray<TSharedPtr<FJsonValue>> VarPaths;
		VarPaths.Add(MakeShared<FJsonValueString>(TEXT("SomeVar")));
		Payload->SetArrayField(TEXT("variables"), VarPaths);

		// Malformed parameter: string instead of number
		Payload->SetStringField(TEXT("sample_interval"), TEXT("not_a_number"));

		FMonolithActionResult Result = FMonolithPieTimeseries::HandleSamplePieTimeseries(Payload);

		TestFalse(TEXT("Malformed sample_interval should return an error, not crash"), Result.bSuccess);
		TestTrue(TEXT("Error should come from parameter validation"), Result.ErrorMessage.Contains(TEXT("sample_interval")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("actor"), TEXT("SomeActor"));
		TArray<TSharedPtr<FJsonValue>> VarPaths;
		VarPaths.Add(MakeShared<FJsonValueString>(TEXT("SomeVar")));
		Payload->SetArrayField(TEXT("variables"), VarPaths);

		// Malformed parameter: string instead of number
		Payload->SetStringField(TEXT("max_samples"), TEXT("not_a_number"));

		FMonolithActionResult Result = FMonolithPieTimeseries::HandleSamplePieTimeseries(Payload);

		TestFalse(TEXT("Malformed max_samples should return an error, not crash"), Result.bSuccess);
		TestTrue(TEXT("Error should come from parameter validation"), Result.ErrorMessage.Contains(TEXT("max_samples")));
	}

	return true;
}

#include "MonolithEditorActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorGetBuildErrorsMalformedTest, "Monolith.ParamGuard.Editor.GetBuildErrorsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorGetBuildErrorsMalformedTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("since_marker"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleGetBuildErrors(Payload);

		TestFalse(TEXT("Malformed since_marker should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter since_marker"), Result.ErrorMessage.Contains(TEXT("since_marker")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("since_iso"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleGetBuildErrors(Payload);

		TestFalse(TEXT("Malformed since_iso should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter since_iso"), Result.ErrorMessage.Contains(TEXT("since_iso")));
	}

	return true;
}
