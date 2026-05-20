#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithProjectionUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithProjectionUtilsReadProjectionTest,
	"Monolith.Core.ProjectionUtils.ReadProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectionUtilsReadProjectionTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("max_results"), 12);
	Params->SetStringField(TEXT("cursor"), TEXT("5"));
	Params->SetStringField(TEXT("fields"), TEXT("name,path,line"));
	Params->SetStringField(TEXT("detail_level"), TEXT("standard"));
	Params->SetBoolField(TEXT("include_diagnostics"), true);
	Params->SetNumberField(TEXT("max_chars"), 4096);

	FMonolithProjectionSpec Projection;
	FString Error;
	TestTrue(TEXT("Projection parses valid params"), FMonolithProjectionUtils::ReadProjection(Params, Projection, Error, 20, 100, 0, 10000));
	TestEqual(TEXT("max_results alias maps to limit"), Projection.Limit, 12);
	TestEqual(TEXT("cursor maps to offset"), Projection.Offset, 5);
	TestTrue(TEXT("fields include name"), Projection.Fields.Contains(TEXT("name")));
	TestTrue(TEXT("fields include path"), Projection.Fields.Contains(TEXT("path")));
	TestTrue(TEXT("fields include line"), Projection.Fields.Contains(TEXT("line")));
	TestEqual(TEXT("detail level lowercases"), Projection.DetailLevel, TEXT("standard"));
	TestTrue(TEXT("include diagnostics parses"), Projection.bIncludeDiagnostics);
	TestEqual(TEXT("max_chars parses"), Projection.MaxChars, 4096);

	TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("detail_level"), TEXT("verbose"));
	TestFalse(TEXT("Invalid detail level is rejected"), FMonolithProjectionUtils::ReadProjection(BadParams, Projection, Error, 20, 100));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithProjectionUtilsProjectRowsTest,
	"Monolith.Core.ProjectionUtils.ProjectRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectionUtilsProjectRowsTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonObject>> Rows;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), FString::Printf(TEXT("row_%d"), Index));
		Row->SetNumberField(TEXT("line"), Index + 1);
		Rows.Add(Row);
	}

	TSet<FString> Fields;
	Fields.Add(TEXT("name"));

	bool bTruncated = false;
	FString NextCursor;
	const TArray<TSharedPtr<FJsonValue>> Values = FMonolithProjectionUtils::ObjectsToValues(Rows, 1, 2, Fields, bTruncated, NextCursor);

	TestEqual(TEXT("Returns requested page size"), Values.Num(), 2);
	TestTrue(TEXT("Reports truncation"), bTruncated);
	TestEqual(TEXT("Next cursor points after page"), NextCursor, TEXT("3"));

	const TSharedPtr<FJsonObject>* FirstObj = nullptr;
	TestTrue(TEXT("First row is object"), Values[0]->TryGetObject(FirstObj));
	TestTrue(TEXT("Projected field is present"), FirstObj && (*FirstObj)->HasField(TEXT("name")));
	TestFalse(TEXT("Unrequested field is absent"), FirstObj && (*FirstObj)->HasField(TEXT("line")));

	TSharedPtr<FJsonObject> Result = FMonolithProjectionUtils::MakeResult(TEXT("ok"), nullptr, FMonolithProjectionSpec(), Values.Num(), bTruncated, NextCursor, { TEXT("monolith.discover") });
	TestEqual(TEXT("Result status"), Result->GetStringField(TEXT("status")), TEXT("ok"));
	TestTrue(TEXT("Result success"), Result->GetBoolField(TEXT("success")));
	TestTrue(TEXT("Result has limits"), Result->HasTypedField<EJson::Object>(TEXT("limits")));
	TestTrue(TEXT("Result has next actions"), Result->HasTypedField<EJson::Array>(TEXT("next_actions")));

	return true;
}

#endif
