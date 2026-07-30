#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithDataflowActions.h"
#include "MonolithDataflowCommon.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithToolRegistry& DataflowParamGuardRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("dataflow"), TEXT("get_status")))
		{
			FMonolithDataflowActions::RegisterActions(Registry);
		}
		return Registry;
	}

	void ExpectDataflowInvalidParams(
		FAutomationTestBase& Test,
		FMonolithToolRegistry& Registry,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FString& Label)
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("dataflow"),
			Action,
			Params);
		Test.TestFalse(*FString::Printf(TEXT("%s fails"), *Label), Result.bSuccess);
		Test.TestEqual(
			*FString::Printf(TEXT("%s uses invalid-param code"), *Label),
			Result.ErrorCode,
			-32602);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports a detail"), *Label),
			!Result.ErrorMessage.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataflowParamGuardTest,
	"Monolith.Dataflow.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = DataflowParamGuardRegistry();

	TSharedPtr<FJsonObject> WrongPackageType = MakeShared<FJsonObject>();
	WrongPackageType->SetNumberField(TEXT("package_path"), 7);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		WrongPackageType,
		TEXT("numeric package path"));

	TSharedPtr<FJsonObject> PackageBoundary = MakeShared<FJsonObject>();
	PackageBoundary->SetStringField(TEXT("package_path"), TEXT("/GameX"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		PackageBoundary,
		TEXT("/GameX package boundary"));

	TSharedPtr<FJsonObject> StringLimit = MakeShared<FJsonObject>();
	StringLimit->SetStringField(TEXT("limit"), TEXT("10"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		StringLimit,
		TEXT("string-encoded asset limit"));

	TSharedPtr<FJsonObject> FractionalLimit = MakeShared<FJsonObject>();
	FractionalLimit->SetNumberField(TEXT("limit"), 1.5);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		FractionalLimit,
		TEXT("fractional asset limit"));

	TSharedPtr<FJsonObject> OversizedLimit = MakeShared<FJsonObject>();
	OversizedLimit->SetNumberField(TEXT("limit"), 501);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		OversizedLimit,
		TEXT("out-of-range asset limit"));

	TSharedPtr<FJsonObject> UnknownParam = MakeShared<FJsonObject>();
	UnknownParam->SetStringField(TEXT("unexpected"), TEXT("value"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_status"),
		UnknownParam,
		TEXT("unknown status param"));

	TSharedPtr<FJsonObject> UniversalShapingParams = MakeShared<FJsonObject>();
	UniversalShapingParams->SetArrayField(
		TEXT("_fields"),
		{MakeShared<FJsonValueString>(TEXT("namespace"))});
	UniversalShapingParams->SetArrayField(
		TEXT("_omit"),
		{MakeShared<FJsonValueString>(TEXT("domain"))});
	UniversalShapingParams->SetBoolField(TEXT("_compact_json"), true);
	UniversalShapingParams->SetArrayField(
		TEXT("_row_fields"),
		{MakeShared<FJsonValueString>(TEXT("name"))});
	UniversalShapingParams->SetArrayField(
		TEXT("_path_fields"),
		{MakeShared<FJsonValueString>(TEXT("namespace"))});
	const FMonolithActionResult ShapedStatus = Registry.ExecuteAction(
		TEXT("dataflow"),
		TEXT("get_status"),
		UniversalShapingParams);
	TestTrue(
		TEXT("all universal response-shaping params pass action-local strict validation"),
		ShapedStatus.bSuccess);
	if (ShapedStatus.bSuccess && ShapedStatus.Result.IsValid())
	{
		TestTrue(
			TEXT("universal response shaping still runs after Dataflow dispatch"),
			ShapedStatus.Result->HasField(TEXT("namespace")));
	}

	TSharedPtr<FJsonObject> ShorthandAsset = MakeShared<FJsonObject>();
	ShorthandAsset->SetStringField(
		TEXT("asset_path"),
		TEXT("MonolithDataflowAutomation/DF_Fixture"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_dataflow_graph"),
		ShorthandAsset,
		TEXT("shorthand asset path"));

	TSharedPtr<FJsonObject> FileAssetPath = MakeShared<FJsonObject>();
	FileAssetPath->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/MonolithDataflowAutomation/DF_Fixture.uasset"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_dataflow_graph"),
		FileAssetPath,
		TEXT("filesystem asset path"));

	TSharedPtr<FJsonObject> StringBool = MakeShared<FJsonObject>();
	StringBool->SetStringField(TEXT("include_pins"), TEXT("false"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_dataflow_node_types"),
		StringBool,
		TEXT("string-encoded include_pins"));

	TSharedPtr<FJsonObject> ExcessiveCommentWork = MakeShared<FJsonObject>();
	ExcessiveCommentWork->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/MonolithDataflowAutomation/DF_Fixture.DF_Fixture"));
	ExcessiveCommentWork->SetNumberField(TEXT("comment_limit"), 1000);
	ExcessiveCommentWork->SetNumberField(TEXT("graph_node_scan_limit"), 50000);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_dataflow_comments"),
		ExcessiveCommentWork,
		TEXT("excessive comment membership work"));

	MonolithDataflow::FOutputBudget RowBudget;
	for (int32 Index = 0;
		Index < MonolithDataflow::MaxOutputRows;
		++Index)
	{
		TestTrue(
			TEXT("aggregate output row budget accepts rows through its limit"),
			RowBudget.TryReserveRow());
	}
	TestFalse(
		TEXT("aggregate output row budget rejects the first excess row"),
		RowBudget.TryReserveRow());
	TestEqual(
		TEXT("aggregate output row count is capped"),
		RowBudget.GetReturnedRowCount(),
		MonolithDataflow::MaxOutputRows);
	TestTrue(
		TEXT("aggregate output row exhaustion is explicit"),
		RowBudget.AreRowsTruncated());

	MonolithDataflow::FOutputBudget TextBudget;
	const FString FullTextChunk =
		FString::ChrN(MonolithDataflow::MaxTextChars, TEXT('x'));
	const int32 FullChunkCount = static_cast<int32>(
		MonolithDataflow::MaxOutputTextCharacters
		/ MonolithDataflow::MaxTextChars);
	for (int32 Index = 0; Index < FullChunkCount; ++Index)
	{
		TestEqual(
			TEXT("aggregate text budget returns each in-budget chunk"),
			TextBudget.Bound(
				FullTextChunk,
				MonolithDataflow::MaxTextChars).Len(),
			MonolithDataflow::MaxTextChars);
	}
	TestTrue(
		TEXT("aggregate text budget omits the first excess chunk"),
		TextBudget.Bound(
			FullTextChunk,
			MonolithDataflow::MaxTextChars).IsEmpty());
	TestEqual(
		TEXT("aggregate returned text count is capped"),
		TextBudget.GetReturnedTextCharacterCount(),
		MonolithDataflow::MaxOutputTextCharacters);
	TestTrue(
		TEXT("aggregate text exhaustion is explicit"),
		TextBudget.IsTextTruncatedByAggregateBudget());
	TSharedPtr<FJsonObject> BudgetMetadata = MakeShared<FJsonObject>();
	MonolithDataflow::AddOutputBudgetFields(
		BudgetMetadata,
		TextBudget);
	TestEqual(
		TEXT("bounded-text character limit is published"),
		static_cast<int64>(
			BudgetMetadata->GetNumberField(
				TEXT("output_bounded_text_character_limit"))),
		MonolithDataflow::MaxOutputTextCharacters);
	TestTrue(
		TEXT("bounded-text aggregate truncation is published"),
		BudgetMetadata->GetBoolField(
			TEXT("output_bounded_text_truncated")));
	TestFalse(
		TEXT("ambiguous all-text budget field is not published"),
		BudgetMetadata->HasField(TEXT("output_text_character_limit")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataflowSurrogateTruncationTest,
	"Monolith.Dataflow.SurrogateTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowSurrogateTruncationTest::RunTest(const FString& Parameters)
{
	// U+1F600 GRINNING FACE is a supplementary character stored as a surrogate
	// pair when TCHAR is UTF-16. Truncating between the two code units would
	// leave an unpaired surrogate that cannot be encoded as UTF-8.
	const FString Emoji = FString(TEXT("\U0001F600"));
	const FString Value = Emoji + Emoji + Emoji;

	auto ContainsUnpairedSurrogate = [](const FString& Text)
	{
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const uint32 CodeUnit = static_cast<uint32>(Text[Index]);
			const bool bHigh = CodeUnit >= 0xD800u && CodeUnit <= 0xDBFFu;
			const bool bLow = CodeUnit >= 0xDC00u && CodeUnit <= 0xDFFFu;
			if (bLow)
			{
				return true; // A low surrogate must be consumed with its high half.
			}
			if (bHigh)
			{
				if (Index + 1 >= Text.Len())
				{
					return true;
				}
				const uint32 NextUnit = static_cast<uint32>(Text[Index + 1]);
				if (NextUnit < 0xDC00u || NextUnit > 0xDFFFu)
				{
					return true;
				}
				++Index; // Skip the paired low surrogate.
			}
		}
		return false;
	};

	// Sweep every cut point so both the ellipsis and non-ellipsis branches are
	// exercised at boundaries that fall inside a surrogate pair.
	for (int32 MaxChars = 0; MaxChars <= Value.Len(); ++MaxChars)
	{
		MonolithDataflow::FOutputBudget Budget;
		const FString Bounded = Budget.Bound(Value, MaxChars);
		TestFalse(
			*FString::Printf(
				TEXT("bounded value at max_chars=%d has no unpaired surrogate"),
				MaxChars),
			ContainsUnpairedSurrogate(Bounded));
		TestTrue(
			*FString::Printf(
				TEXT("bounded value at max_chars=%d respects the cap"),
				MaxChars),
			Bounded.Len() <= FMath::Max(MaxChars, 3));
	}

	MonolithDataflow::FOutputBudget UntruncatedBudget;
	TestEqual(
		TEXT("a value within the cap is returned verbatim"),
		UntruncatedBudget.Bound(Value, Value.Len()),
		Value);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
