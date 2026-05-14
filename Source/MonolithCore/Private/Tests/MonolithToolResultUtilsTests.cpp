#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithToolResultUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonObject> MakeValueResult(int32 Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("value"), Value);
		return Obj;
	}

	FString FirstTextContent(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("content"), Content) || !Content || Content->Num() == 0)
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Content)[0].IsValid() || !(*Content)[0]->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return FString();
		}

		FString Text;
		(*Obj)->TryGetStringField(TEXT("text"), Text);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultLegacyShapeTest,
	"Monolith.Core.ToolResults.LegacyShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultLegacyShapeTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult ActionResult = FMonolithActionResult::Success(MakeValueResult(42));
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false);

	TestTrue(TEXT("Result exists"), Result.IsValid());
	TestFalse(TEXT("Legacy result is not an error"), Result->GetBoolField(TEXT("isError")));
	TestFalse(TEXT("Legacy result has no structuredContent"), Result->HasField(TEXT("structuredContent")));
	TestFalse(TEXT("Legacy result has no _meta"), Result->HasField(TEXT("_meta")));
	TestTrue(TEXT("Legacy text JSON remains available"), FirstTextContent(Result).Contains(TEXT("\"value\":42")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultStructuredShapeTest,
	"Monolith.Core.ToolResults.StructuredShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultStructuredShapeTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult ActionResult = FMonolithActionResult::Success(MakeValueResult(7));
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true);

	const TSharedPtr<FJsonObject>* Structured = nullptr;
	TestTrue(TEXT("structuredContent exists"), Result->TryGetObjectField(TEXT("structuredContent"), Structured));
	if (Structured && Structured->IsValid())
	{
		TestEqual(TEXT("structuredContent carries value"), (*Structured)->GetIntegerField(TEXT("value")), 7);
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	TestTrue(TEXT("_meta exists"), Result->TryGetObjectField(TEXT("_meta"), Meta));
	if (Meta && Meta->IsValid())
	{
		TestEqual(TEXT("result kind"), (*Meta)->GetStringField(TEXT("result_kind")), TEXT("structured"));
		TestTrue(TEXT("legacy text JSON marker"), (*Meta)->GetBoolField(TEXT("legacy_text_json")));
	}
	TestTrue(TEXT("Structured result preserves legacy text JSON"), FirstTextContent(Result).Contains(TEXT("\"value\":7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultStructuredErrorTest,
	"Monolith.Core.ToolResults.StructuredError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultStructuredErrorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetStringField(TEXT("retry"), TEXT("adjust_params"));

	FMonolithActionResult ActionResult = FMonolithActionResult::Error(TEXT("bad params"), -32602);
	ActionResult
		.WithRelatedAction(TEXT("compile_blueprint"))
		.WithHint(TEXT("asset_path is required"))
		.WithErrorData(ErrorData);

	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true);
	TestTrue(TEXT("Error result isError"), Result->GetBoolField(TEXT("isError")));
	TestTrue(TEXT("Error text keeps hint"), FirstTextContent(Result).Contains(TEXT("asset_path is required")));

	const TSharedPtr<FJsonObject>* Structured = nullptr;
	TestTrue(TEXT("structuredContent exists for errors"), Result->TryGetObjectField(TEXT("structuredContent"), Structured));
	if (Structured && Structured->IsValid())
	{
		TestFalse(TEXT("structured error ok=false"), (*Structured)->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("structured error code"), (*Structured)->GetIntegerField(TEXT("error_code")), -32602);
		TestTrue(TEXT("structured error data exists"), (*Structured)->HasTypedField<EJson::Object>(TEXT("error_data")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
