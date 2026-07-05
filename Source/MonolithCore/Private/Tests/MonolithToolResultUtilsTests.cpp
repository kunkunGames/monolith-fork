#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
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

	int32 ContentCount(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("content"), Content) || !Content)
		{
			return 0;
		}
		return Content->Num();
	}

	TSharedPtr<FJsonObject> ContentAt(const TSharedPtr<FJsonObject>& Result, int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("content"), Content) || !Content || !Content->IsValidIndex(Index))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Content)[Index].IsValid() || !(*Content)[Index]->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return nullptr;
		}
		return *Obj;
	}

	FMonolithActionResult MakeImageBlockResult()
	{
		FMonolithActionResult ActionResult = FMonolithActionResult::Success(MakeValueResult(1));
		FMonolithToolContentBlock Block;
		Block.Type = TEXT("image");
		Block.MimeType = TEXT("image/png");
		Block.Base64Data = TEXT("QUJD"); // "ABC"
		ActionResult.MediaBlocks.Add(Block);
		return ActionResult;
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
		TestFalse(TEXT("legacy text JSON marker is omitted"), (*Meta)->HasField(TEXT("legacy_text_json")));
		TestEqual(TEXT("content text mode"), (*Meta)->GetStringField(TEXT("content_text_mode")), TEXT("compact_status"));
	}
	TestEqual(TEXT("Structured result uses compact text"), FirstTextContent(Result), TEXT("OK; see structuredContent."));
	TestFalse(TEXT("Structured result does not duplicate JSON text"), FirstTextContent(Result).Contains(TEXT("\"value\":7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultStructuredEmptySuccessTest,
	"Monolith.Core.ToolResults.StructuredEmptySuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultStructuredEmptySuccessTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult ActionResult = FMonolithActionResult::Success(TSharedPtr<FJsonObject>());
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true);

	TestTrue(TEXT("Empty success result exists"), Result.IsValid());
	TestFalse(TEXT("Empty success is not an error"), Result->GetBoolField(TEXT("isError")));
	TestEqual(TEXT("Empty success text is compact"), FirstTextContent(Result), TEXT("OK; see structuredContent."));
	TestTrue(TEXT("Empty success has structuredContent"), Result->HasTypedField<EJson::Object>(TEXT("structuredContent")));
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

	// Legacy envelope (bCompactErrorEnvelope=false): duplicated structured error shape.
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true, false, false);
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

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	TestTrue(TEXT("_meta exists"), Result->TryGetObjectField(TEXT("_meta"), Meta));
	if (Meta && Meta->IsValid())
	{
		TestEqual(TEXT("error content text mode"), (*Meta)->GetStringField(TEXT("content_text_mode")), TEXT("error_text"));
		TestFalse(TEXT("error legacy text JSON marker is omitted"), (*Meta)->HasField(TEXT("legacy_text_json")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultLegacyErrorDataTest,
	"Monolith.Core.ToolResults.LegacyErrorData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultLegacyErrorDataTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetStringField(TEXT("failure_cause"), TEXT("invalid_param"));
	ErrorData->SetStringField(TEXT("retryability"), TEXT("retry_with_validated_param_types_or_ranges"));

	FMonolithActionResult ActionResult = FMonolithActionResult::Error(TEXT("bad params"), -32602);
	ActionResult.WithErrorData(ErrorData);

	// Legacy envelope (bCompactErrorEnvelope=false): error_data fields stay flattened at top level.
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false, false, false);
	TestTrue(TEXT("Legacy error result isError"), Result->GetBoolField(TEXT("isError")));
	TestEqual(TEXT("Legacy error keeps flattened failure cause"), Result->GetStringField(TEXT("failure_cause")), TEXT("invalid_param"));

	const TSharedPtr<FJsonObject>* ErrorDataObject = nullptr;
	TestTrue(TEXT("Legacy error exposes nested error_data object"),
		Result->TryGetObjectField(TEXT("error_data"), ErrorDataObject) && ErrorDataObject && ErrorDataObject->IsValid());
	if (ErrorDataObject && ErrorDataObject->IsValid())
	{
		TestEqual(TEXT("Nested error_data keeps failure cause"), (*ErrorDataObject)->GetStringField(TEXT("failure_cause")), TEXT("invalid_param"));
		TestEqual(TEXT("Nested error_data keeps retryability"), (*ErrorDataObject)->GetStringField(TEXT("retryability")), TEXT("retry_with_validated_param_types_or_ranges"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultEmptyMediaByteIdenticalTest,
	"Monolith.Core.ToolResults.EmptyMediaByteIdentical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultEmptyMediaByteIdenticalTest::RunTest(const FString& Parameters)
{
	// Empty MediaBlocks must produce the same content regardless of the typed-media flag.
	FMonolithActionResult ActionResult = FMonolithActionResult::Success(MakeValueResult(42));

	TSharedPtr<FJsonObject> WithoutFlag = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false, false);
	TSharedPtr<FJsonObject> WithFlag = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false, true);

	TestEqual(TEXT("Empty media, flag off → single text block"), ContentCount(WithoutFlag), 1);
	TestEqual(TEXT("Empty media, flag on → still single text block (byte-identical)"), ContentCount(WithFlag), 1);
	TestTrue(TEXT("Text JSON preserved with flag off"), FirstTextContent(WithoutFlag).Contains(TEXT("\"value\":42")));
	TestTrue(TEXT("Text JSON preserved with flag on"), FirstTextContent(WithFlag).Contains(TEXT("\"value\":42")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultMediaBlockGatingTest,
	"Monolith.Core.ToolResults.MediaBlockGating",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultMediaBlockGatingTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult ActionResult = MakeImageBlockResult();

	// Flag OFF: media block suppressed → text block only.
	TSharedPtr<FJsonObject> FlagOff = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false, false);
	TestEqual(TEXT("Media block suppressed when flag off"), ContentCount(FlagOff), 1);
	{
		TSharedPtr<FJsonObject> First = ContentAt(FlagOff, 0);
		TestTrue(TEXT("Only block is text when flag off"), First.IsValid() && First->GetStringField(TEXT("type")) == TEXT("text"));
	}

	// Flag ON: text block first, image block appended after.
	TSharedPtr<FJsonObject> FlagOn = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false, true);
	TestEqual(TEXT("Image block appended when flag on"), ContentCount(FlagOn), 2);
	{
		TSharedPtr<FJsonObject> First = ContentAt(FlagOn, 0);
		TestTrue(TEXT("Text block stays first"), First.IsValid() && First->GetStringField(TEXT("type")) == TEXT("text"));

		TSharedPtr<FJsonObject> Second = ContentAt(FlagOn, 1);
		TestTrue(TEXT("Second block exists"), Second.IsValid());
		if (Second.IsValid())
		{
			TestEqual(TEXT("Media block type is image"), Second->GetStringField(TEXT("type")), TEXT("image"));
			TestEqual(TEXT("Media block carries mimeType"), Second->GetStringField(TEXT("mimeType")), TEXT("image/png"));
			TestEqual(TEXT("Media block carries base64 data"), Second->GetStringField(TEXT("data")), TEXT("QUJD"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultCompactStructuredErrorTest,
	"Monolith.Core.ToolResults.CompactStructuredError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultCompactStructuredErrorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetStringField(TEXT("retry"), TEXT("adjust_params"));

	FMonolithActionResult ActionResult = FMonolithActionResult::Error(TEXT("bad params"), -32602);
	ActionResult
		.WithRelatedAction(TEXT("compile_blueprint"))
		.WithHint(TEXT("asset_path is required"))
		.WithErrorData(ErrorData);

	// Compact default: single machine-readable copy in structuredContent, one-line text pointer.
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true);
	TestTrue(TEXT("Compact error isError"), Result->GetBoolField(TEXT("isError")));
	TestEqual(TEXT("Compact error text is a one-line pointer"), FirstTextContent(Result), TEXT("bad params; see structuredContent."));
	TestFalse(TEXT("No top-level hints duplicate"), Result->HasField(TEXT("hints")));
	TestFalse(TEXT("No top-level error_data duplicate"), Result->HasField(TEXT("error_data")));
	TestFalse(TEXT("No top-level related_actions duplicate"), Result->HasField(TEXT("related_actions")));
	TestFalse(TEXT("No flattened error_data field"), Result->HasField(TEXT("retry")));

	const TSharedPtr<FJsonObject>* Structured = nullptr;
	TestTrue(TEXT("structuredContent exists"), Result->TryGetObjectField(TEXT("structuredContent"), Structured));
	if (Structured && Structured->IsValid())
	{
		TestFalse(TEXT("structured ok=false"), (*Structured)->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("structured error code"), (*Structured)->GetIntegerField(TEXT("error_code")), -32602);
		TestTrue(TEXT("structured keeps error_data"), (*Structured)->HasTypedField<EJson::Object>(TEXT("error_data")));
		TestTrue(TEXT("structured keeps hints"), (*Structured)->HasTypedField<EJson::Array>(TEXT("hints")));
		TestTrue(TEXT("structured keeps related_actions"), (*Structured)->HasTypedField<EJson::Array>(TEXT("related_actions")));
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	TestTrue(TEXT("_meta exists"), Result->TryGetObjectField(TEXT("_meta"), Meta));
	if (Meta && Meta->IsValid())
	{
		TestEqual(TEXT("content text mode is compact_pointer"), (*Meta)->GetStringField(TEXT("content_text_mode")), TEXT("compact_pointer"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultCompactTextErrorTest,
	"Monolith.Core.ToolResults.CompactTextError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultCompactTextErrorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetStringField(TEXT("failure_cause"), TEXT("invalid_param"));

	FMonolithActionResult ActionResult = FMonolithActionResult::Error(TEXT("bad params"), -32602);
	ActionResult
		.WithHint(TEXT("asset_path is required"))
		.WithErrorData(ErrorData);

	// Compact without structuredContent: full error text for text-only clients,
	// single top-level copies, no flattening.
	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, false);
	TestTrue(TEXT("Compact text error isError"), Result->GetBoolField(TEXT("isError")));
	TestTrue(TEXT("Text keeps full error text with hint"), FirstTextContent(Result).Contains(TEXT("asset_path is required")));
	TestTrue(TEXT("Top-level error_data kept once"), Result->HasTypedField<EJson::Object>(TEXT("error_data")));
	TestTrue(TEXT("Top-level hints kept once"), Result->HasTypedField<EJson::Array>(TEXT("hints")));
	TestFalse(TEXT("No flattened failure_cause at top level"), Result->HasField(TEXT("failure_cause")));
	TestFalse(TEXT("No structuredContent without the flag"), Result->HasField(TEXT("structuredContent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolResultCompactErrorSizeTest,
	"Monolith.Core.ToolResults.CompactErrorSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolResultCompactErrorSizeTest::RunTest(const FString& Parameters)
{
	// Representative missing-required-param failure: required_params with
	// name/type/aliases, provided keys, and next actions.
	TSharedPtr<FJsonObject> RequiredParam = MakeShared<FJsonObject>();
	RequiredParam->SetStringField(TEXT("name"), TEXT("asset_path"));
	RequiredParam->SetStringField(TEXT("type"), TEXT("string"));
	{
		TArray<TSharedPtr<FJsonValue>> Aliases;
		Aliases.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Aliases.Add(MakeShared<FJsonValueString>(TEXT("asset")));
		RequiredParam->SetArrayField(TEXT("aliases"), Aliases);
	}

	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	{
		TArray<TSharedPtr<FJsonValue>> RequiredParams;
		RequiredParams.Add(MakeShared<FJsonValueObject>(RequiredParam));
		ErrorData->SetArrayField(TEXT("required_params"), RequiredParams);

		TArray<TSharedPtr<FJsonValue>> ProvidedKeys;
		ProvidedKeys.Add(MakeShared<FJsonValueString>(TEXT("query")));
		ProvidedKeys.Add(MakeShared<FJsonValueString>(TEXT("limit")));
		ErrorData->SetArrayField(TEXT("provided_keys"), ProvidedKeys);

		TArray<TSharedPtr<FJsonValue>> NextActions;
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("monolith.discover(namespace, action) for the exact schema")));
		ErrorData->SetArrayField(TEXT("next_actions"), NextActions);
	}

	FMonolithActionResult ActionResult = FMonolithActionResult::Error(
		TEXT("Missing required param(s): [asset_path]. Provided keys: [query, limit]"), -32602);
	ActionResult
		.WithHint(TEXT("Inspect exact parameters with monolith_discover"))
		.WithHint(TEXT("Aliases path/asset map to asset_path"))
		.WithErrorData(ErrorData);

	TSharedPtr<FJsonObject> Result = FMonolithToolResultUtils::BuildMcpToolResult(ActionResult, true);
	const FString Serialized = FMonolithJsonUtils::Serialize(Result);
	TestTrue(FString::Printf(TEXT("Compact missing-param error stays <= 1536 bytes (got %d)"), Serialized.Len()),
		Serialized.Len() <= 1536);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
