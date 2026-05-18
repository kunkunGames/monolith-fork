#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithJsonUtilsParseSerializeTest,
	"Monolith.Core.JsonUtils.ParseSerialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJsonUtilsParseSerializeTest::RunTest(const FString& Parameters)
{
	// Test 1: Serialize a simple object
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("hello"), TEXT("world"));
		Obj->SetNumberField(TEXT("number"), 42);

		FString Serialized = FMonolithJsonUtils::Serialize(Obj);
		TestTrue(TEXT("Serialized contains 'hello' key"), Serialized.Contains(TEXT("\"hello\"")));
		TestTrue(TEXT("Serialized contains 'world' value"), Serialized.Contains(TEXT("\"world\"")));
		TestTrue(TEXT("Serialized contains 'number' key"), Serialized.Contains(TEXT("\"number\"")));
		TestTrue(TEXT("Serialized contains '42' value"), Serialized.Contains(TEXT("42")));
	}

	// Test 2: Parse a valid JSON string
	{
		FString ValidJson = TEXT("{\"key\":\"value\", \"count\":10}");
		TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(ValidJson);

		TestTrue(TEXT("Parse returns valid pointer for valid JSON"), Parsed.IsValid());
		if (Parsed.IsValid())
		{
			TestEqual(TEXT("Parsed key is correct"), Parsed->GetStringField(TEXT("key")), TEXT("value"));
			TestEqual(TEXT("Parsed count is correct"), Parsed->GetNumberField(TEXT("count")), 10.0);
		}
	}

	// Test 3: Parse a malformed JSON string
	{
		FString MalformedJson = TEXT("{\"key\":\"value\", \"count\":10"); // missing closing brace
		TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(MalformedJson);

		TestFalse(TEXT("Parse returns nullptr for malformed JSON"), Parsed.IsValid());
	}

	// Test 4: Parse an empty JSON string
	{
		FString EmptyJson = TEXT("");
		TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(EmptyJson);

		TestFalse(TEXT("Parse returns nullptr for empty JSON"), Parsed.IsValid());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithJsonUtilsResponseTest,
	"Monolith.Core.JsonUtils.Response",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJsonUtilsResponseTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonValueNumber> Id = MakeShared<FJsonValueNumber>(1);

	// Test 1: SuccessResponse
	{
		TSharedPtr<FJsonValueString> ResultStr = MakeShared<FJsonValueString>(TEXT("OK"));
		TSharedPtr<FJsonObject> Response = FMonolithJsonUtils::SuccessResponse(Id, ResultStr);

		TestTrue(TEXT("SuccessResponse is valid"), Response.IsValid());
		TestEqual(TEXT("SuccessResponse jsonrpc is 2.0"), Response->GetStringField(TEXT("jsonrpc")), TEXT("2.0"));
		TestEqual(TEXT("SuccessResponse id is 1"), Response->GetNumberField(TEXT("id")), 1.0);
		TestEqual(TEXT("SuccessResponse result is 'OK'"), Response->GetStringField(TEXT("result")), TEXT("OK"));
	}

	// Test 2: SuccessResponse with null result
	{
		TSharedPtr<FJsonObject> Response = FMonolithJsonUtils::SuccessResponse(Id, nullptr);

		TestTrue(TEXT("SuccessResponse with null result is valid"), Response.IsValid());
		TestTrue(TEXT("SuccessResponse fallback to empty object result"), Response->HasField(TEXT("result")));
		TestTrue(TEXT("SuccessResponse empty object result type"), Response->HasTypedField<EJson::Object>(TEXT("result")));
	}

	// Test 3: SuccessObject
	{
		TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
		DataObj->SetStringField(TEXT("status"), TEXT("success"));
		TSharedPtr<FJsonObject> Response = FMonolithJsonUtils::SuccessObject(Id, DataObj);

		TestTrue(TEXT("SuccessObject is valid"), Response.IsValid());
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		TestTrue(TEXT("SuccessObject result is object"), Response->TryGetObjectField(TEXT("result"), ResultObj));
		if (ResultObj)
		{
			TestEqual(TEXT("SuccessObject result data is correct"), (*ResultObj)->GetStringField(TEXT("status")), TEXT("success"));
		}
	}

	// Test 4: ErrorResponse
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("detail"), TEXT("some details"));

		TSharedPtr<FJsonObject> Response = FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Invalid params"), MakeShared<FJsonValueObject>(ErrorData));

		TestTrue(TEXT("ErrorResponse is valid"), Response.IsValid());
		TestEqual(TEXT("ErrorResponse jsonrpc is 2.0"), Response->GetStringField(TEXT("jsonrpc")), TEXT("2.0"));
		TestEqual(TEXT("ErrorResponse id is 1"), Response->GetNumberField(TEXT("id")), 1.0);
		TestFalse(TEXT("ErrorResponse should not have result"), Response->HasField(TEXT("result")));

		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		TestTrue(TEXT("ErrorResponse has error object"), Response->TryGetObjectField(TEXT("error"), ErrorObj));
		if (ErrorObj)
		{
			TestEqual(TEXT("ErrorResponse code"), (*ErrorObj)->GetNumberField(TEXT("code")), (double)FMonolithJsonUtils::ErrInvalidParams);
			TestEqual(TEXT("ErrorResponse message"), (*ErrorObj)->GetStringField(TEXT("message")), TEXT("Invalid params"));

			const TSharedPtr<FJsonObject>* DataObj = nullptr;
			TestTrue(TEXT("ErrorResponse has data"), (*ErrorObj)->TryGetObjectField(TEXT("data"), DataObj));
			if (DataObj)
			{
				TestEqual(TEXT("ErrorResponse detail data"), (*DataObj)->GetStringField(TEXT("detail")), TEXT("some details"));
			}
		}
	}

	// Test 5: StringArrayToJson
	{
		TArray<FString> InputStrings = { TEXT("one"), TEXT("two"), TEXT("three") };
		TSharedRef<FJsonValueArray> JsonArr = FMonolithJsonUtils::StringArrayToJson(InputStrings);

		const TArray<TSharedPtr<FJsonValue>>& OutArray = JsonArr->AsArray();
		TestEqual(TEXT("StringArrayToJson has 3 elements"), OutArray.Num(), 3);
		if (OutArray.Num() == 3)
		{
			TestEqual(TEXT("StringArrayToJson element 0"), OutArray[0]->AsString(), TEXT("one"));
			TestEqual(TEXT("StringArrayToJson element 1"), OutArray[1]->AsString(), TEXT("two"));
			TestEqual(TEXT("StringArrayToJson element 2"), OutArray[2]->AsString(), TEXT("three"));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
