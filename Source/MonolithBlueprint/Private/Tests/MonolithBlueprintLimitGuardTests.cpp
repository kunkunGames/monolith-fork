#include "Misc/AutomationTest.h"
#include "MonolithBlueprintActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FMonolithActionResult ExecuteSearchFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithBlueprintActions::RegisterActions();
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("search_functions"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintSearchFunctionsLimitTest, "Monolith.LimitGuard.Blueprint.SearchFunctionsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBlueprintSearchFunctionsLimitTest::RunTest(const FString& Parameters)
{
	// Test very large limit upper bound
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // Query param to match many native functions (e.g., Get)
        Params->SetStringField(TEXT("query"), TEXT("Get"));
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteSearchFunctions(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("returned_count"), CountVal))
		{
			if (CountVal > 1000.0)
			{
				AddError(FString::Printf(TEXT("Huge limit was not clamped to 1000. Count was %f"), CountVal));
			}
		}

		FString DetailLevel;
		if (Result.Result.IsValid() && (!Result.Result->TryGetStringField(TEXT("detail_level"), DetailLevel) || DetailLevel != TEXT("minimal")))
		{
			AddError(TEXT("search_functions default detail_level should be minimal"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstResult = (*Results)[0]->Type == EJson::Object ? (*Results)[0]->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
			if (!FirstResult.IsValid() || FirstResult->TryGetArrayField(TEXT("inputs"), InputsArr) || FirstResult->TryGetArrayField(TEXT("outputs"), OutputsArr))
			{
				AddError(TEXT("search_functions minimal detail should omit parameter arrays"));
			}
		}
	}

	// Test standard detail preserves parameter arrays for callers that need full pin signatures.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), TEXT("Get"));
		Params->SetNumberField(TEXT("limit"), 1.0);
		Params->SetStringField(TEXT("detail_level"), TEXT("standard"));
		FMonolithActionResult Result = ExecuteSearchFunctions(Params);

		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstResult = (*Results)[0]->Type == EJson::Object ? (*Results)[0]->AsObject() : nullptr;
			if (!FirstResult.IsValid() || !FirstResult->HasTypedField(TEXT("inputs"), EJson::Array) || !FirstResult->HasTypedField(TEXT("outputs"), EJson::Array))
			{
				AddError(TEXT("search_functions detail_level=standard should include parameter arrays"));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintSearchFunctionsPagingTest, "Monolith.LimitGuard.Blueprint.SearchFunctionsPagesAndProjects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBlueprintSearchFunctionsPagingTest::RunTest(const FString& Parameters)
{
	auto GetString = [](const FMonolithActionResult& Result, const TCHAR* Key) -> FString
	{
		FString Value;
		if (Result.Result.IsValid()) Result.Result->TryGetStringField(Key, Value);
		return Value;
	};
	auto GetFirstRow = [](const FMonolithActionResult& Result) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() > 0)
		{
			return (*Results)[0]->Type == EJson::Object ? (*Results)[0]->AsObject() : nullptr;
		}
		return nullptr;
	};

	// Page 1 and page 2 must chain through next_cursor without overlapping rows.
	TSharedPtr<FJsonObject> Page1Params = MakeShared<FJsonObject>();
	Page1Params->SetStringField(TEXT("query"), TEXT("Get"));
	Page1Params->SetNumberField(TEXT("limit"), 1.0);
	FMonolithActionResult Page1 = ExecuteSearchFunctions(Page1Params);
	double Total1 = 0.0, Returned1 = 0.0;
	TestTrue(TEXT("page1 total present"), Page1.Result.IsValid() && Page1.Result->TryGetNumberField(TEXT("total"), Total1));
	TestTrue(TEXT("page1 returned present"), Page1.Result.IsValid() && Page1.Result->TryGetNumberField(TEXT("returned"), Returned1));
	TestEqual(TEXT("page1 returned one row"), (int32)Returned1, 1);
	TestTrue(TEXT("page1 matches more than one function"), Total1 > 1.0);
	TestEqual(TEXT("page1 next_cursor is the next offset"), GetString(Page1, TEXT("next_cursor")), FString(TEXT("1")));

	TSharedPtr<FJsonObject> Page2Params = MakeShared<FJsonObject>();
	Page2Params->SetStringField(TEXT("query"), TEXT("Get"));
	Page2Params->SetNumberField(TEXT("limit"), 1.0);
	Page2Params->SetNumberField(TEXT("offset"), 1.0);
	FMonolithActionResult Page2 = ExecuteSearchFunctions(Page2Params);
	TestEqual(TEXT("page2 next_cursor is the next offset"), GetString(Page2, TEXT("next_cursor")), FString(TEXT("2")));
	const TSharedPtr<FJsonObject> Row1 = GetFirstRow(Page1);
	const TSharedPtr<FJsonObject> Row2 = GetFirstRow(Page2);
	TestTrue(TEXT("page rows present"), Row1.IsValid() && Row2.IsValid());
	if (Row1.IsValid() && Row2.IsValid())
	{
			FString Class1, Func1, Class2, Func2;
			Row1->TryGetStringField(TEXT("class_name"), Class1);
			Row1->TryGetStringField(TEXT("function_name"), Func1);
			Row2->TryGetStringField(TEXT("class_name"), Class2);
			Row2->TryGetStringField(TEXT("function_name"), Func2);

			const FString Key1 = Class1 + TEXT("::") + Func1;
			const FString Key2 = Class2 + TEXT("::") + Func2;
		TestNotEqual(TEXT("pages do not overlap"), Key1, Key2);
	}
	bool bTruncated2 = false;
	if (Page2.Result.IsValid() && Page2.Result->TryGetBoolField(TEXT("truncated"), bTruncated2))
	{
		TestTrue(TEXT("page2 mid-stream is truncated"), bTruncated2);
	}

	// A cursor from a prior next_cursor must land on the same page as the equivalent offset.
	TSharedPtr<FJsonObject> CursorParams = MakeShared<FJsonObject>();
	CursorParams->SetStringField(TEXT("query"), TEXT("Get"));
	CursorParams->SetNumberField(TEXT("limit"), 1.0);
	CursorParams->SetStringField(TEXT("cursor"), TEXT("1"));
	FMonolithActionResult CursorPage = ExecuteSearchFunctions(CursorParams);
	const TSharedPtr<FJsonObject> CursorRow = GetFirstRow(CursorPage);
	TestTrue(TEXT("cursor page row present"), CursorRow.IsValid());
	if (CursorRow.IsValid() && Row2.IsValid())
	{
			FString CursorFunc, Row2Func;
			CursorRow->TryGetStringField(TEXT("function_name"), CursorFunc);
			Row2->TryGetStringField(TEXT("function_name"), Row2Func);
			TestEqual(TEXT("cursor page matches offset page"), CursorFunc, Row2Func);
	}

	// Fields projection keeps only requested keys and warns on unknown ones.
	TSharedPtr<FJsonObject> ProjectedParams = MakeShared<FJsonObject>();
	ProjectedParams->SetStringField(TEXT("query"), TEXT("Get"));
	ProjectedParams->SetNumberField(TEXT("limit"), 3.0);
	ProjectedParams->SetStringField(TEXT("fields"), TEXT("function_name,class_name,bogus_field"));
	FMonolithActionResult Projected = ExecuteSearchFunctions(ProjectedParams);
	const TSharedPtr<FJsonObject> ProjectedRow = GetFirstRow(Projected);
	TestTrue(TEXT("projected row present"), ProjectedRow.IsValid());
	if (ProjectedRow.IsValid())
	{
		FString FunctionName;
		TestTrue(TEXT("projected row keeps function_name"), ProjectedRow->TryGetStringField(TEXT("function_name"), FunctionName));
		FString ClassName;
		TestTrue(TEXT("projected row keeps class_name"), ProjectedRow->TryGetStringField(TEXT("class_name"), ClassName));
		FString Category;
		TestFalse(TEXT("projected row drops category"), ProjectedRow->TryGetStringField(TEXT("category"), Category));
		bool bIsPure = false;
		TestFalse(TEXT("projected row drops is_pure"), ProjectedRow->TryGetBoolField(TEXT("is_pure"), bIsPure));
	}
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	bool bUnknownFieldWarning = false;
	if (Projected.Result.IsValid() && Projected.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
	{
		for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
		{
			FString Warning;
			if (WarningValue->TryGetString(Warning) && Warning.Contains(TEXT("bogus_field")))
			{
				bUnknownFieldWarning = true;
			}
		}
	}
	TestTrue(TEXT("unknown field produces a warning"), bUnknownFieldWarning);

	// Offset=0 keeps the legacy shape intact for existing consumers.
	TSharedPtr<FJsonObject> LegacyParams = MakeShared<FJsonObject>();
	LegacyParams->SetStringField(TEXT("query"), TEXT("Get"));
	LegacyParams->SetNumberField(TEXT("limit"), 2.0);
	FMonolithActionResult Legacy = ExecuteSearchFunctions(LegacyParams);
	double LegacyMatchedCount = 0;
	TestTrue(TEXT("legacy matched_count still present"), Legacy.Result.IsValid() && Legacy.Result->TryGetNumberField(TEXT("matched_count"), LegacyMatchedCount));
	double LegacyReturnedCount = 0;
	TestTrue(TEXT("legacy returned_count still present"), Legacy.Result.IsValid() && Legacy.Result->TryGetNumberField(TEXT("returned_count"), LegacyReturnedCount));
	const TSharedPtr<FJsonObject>* LimitsObj = nullptr;
	TestTrue(TEXT("limits contract present"), Legacy.Result.IsValid() && Legacy.Result->TryGetObjectField(TEXT("limits"), LimitsObj));
	const TSharedPtr<FJsonObject>* ProjectionObj = nullptr;
	TestTrue(TEXT("projection contract present"), Legacy.Result.IsValid() && Legacy.Result->TryGetObjectField(TEXT("projection"), ProjectionObj));

	return true;
}

namespace
{
FMonolithActionResult ExecuteBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("blueprint"), TEXT("batch_execute")))
	{
		FMonolithBlueprintActions::RegisterActions();
	}
	return Registry.ExecuteAction(TEXT("blueprint"), TEXT("batch_execute"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintBatchExecuteLimitTest, "Monolith.LimitGuard.Blueprint.BatchExecuteRejectsOversizedArray", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBlueprintBatchExecuteLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent.NonExistent"));

	TArray<TSharedPtr<FJsonValue>> OpsArray;
	for (int32 i = 0; i < 501; ++i)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), TEXT("add_node"));
		OpsArray.Add(MakeShared<FJsonValueObject>(Op));
	}
	Params->SetArrayField(TEXT("operations"), OpsArray);

	FMonolithActionResult Result = ExecuteBatchExecute(Params);

	TestTrue(TEXT("Should fail on oversized array"), !Result.bSuccess);
	TestTrue(TEXT("Error message should mention max 500"), Result.ErrorMessage.Contains(TEXT("max 500")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
