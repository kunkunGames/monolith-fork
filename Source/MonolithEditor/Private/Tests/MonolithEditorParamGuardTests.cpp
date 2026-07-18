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
#include "MonolithPieObjectActions.h"

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

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("exclude_categories"), TEXT("not_an_array"));

		FMonolithActionResult Result = FMonolithEditorActions::HandleGetBuildErrors(Payload);

		TestFalse(TEXT("Malformed exclude_categories should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter exclude_categories"), Result.ErrorMessage.Contains(TEXT("exclude_categories")));
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorSearchLogsMalformedTest, "Monolith.ParamGuard.Editor.SearchLogsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSearchLogsMalformedTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("pattern"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleSearchLogs(Payload);

		TestFalse(TEXT("Malformed pattern should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter pattern"), Result.ErrorMessage.Contains(TEXT("pattern")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("category"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleSearchLogs(Payload);

		TestFalse(TEXT("Malformed category should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter category"), Result.ErrorMessage.Contains(TEXT("category")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("verbosity"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleSearchLogs(Payload);

		TestFalse(TEXT("Malformed verbosity should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter verbosity"), Result.ErrorMessage.Contains(TEXT("verbosity")));
	}

	return true;
}


#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorGetRecentLogsAliasTest, "Monolith.Registry.Editor.GetRecentLogsAcceptsMaxAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorGetRecentLogsAliasTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Test malformed type for canonical param
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("count"), TEXT("not_a_number"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("editor"), TEXT("get_recent_logs"), Payload);

		TestFalse(TEXT("Malformed count should return an error"), Result.bSuccess);
	}

	// Test malformed type for alias param
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("max"), TEXT("not_a_number"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("editor"), TEXT("get_recent_logs"), Payload);

		TestFalse(TEXT("Malformed max alias should return an error"), Result.bSuccess);
	}

	// Test collision
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("count"), 10);
		Payload->SetNumberField(TEXT("max"), 20);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("editor"), TEXT("get_recent_logs"), Payload);

		TestFalse(TEXT("Providing both count and max alias should return an error"), Result.bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPieObjectParamGuardTest, "Monolith.Editor.PieObjectParamGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithPieObjectParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("actor_label"), 123.0);
	Payload->SetStringField(TEXT("object_name"), TEXT("MyObject"));
	Payload->SetStringField(TEXT("class_name"), TEXT("MyClass"));
	Payload->SetNumberField(TEXT("component_name"), 123.0);
	Payload->SetStringField(TEXT("anim_instance"), TEXT("true"));

	FMonolithActionResult Result = FMonolithPieObjectActions::HandleGetObjectProperties(Payload);
	TestFalse(TEXT("Should reject malformed actor_label type"), Result.bSuccess);
	TestEqual(TEXT("Should return invalid param error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	Payload->RemoveField(TEXT("actor_label"));
	Payload->SetStringField(TEXT("actor_label"), TEXT("MyActor"));
	Result = FMonolithPieObjectActions::HandleGetObjectProperties(Payload);
	TestFalse(TEXT("Should reject malformed component_name type"), Result.bSuccess);
	TestEqual(TEXT("Should return invalid param error"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	return true;
}
