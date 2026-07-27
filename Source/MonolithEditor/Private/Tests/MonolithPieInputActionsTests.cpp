#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithJsonUtils.h"
#include "MonolithPieInputActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieInjectKeyContractTest,
	"Monolith.Editor.PieInput.InjectKeyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieInjectKeyContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TestTrue(TEXT("editor.pie_inject_key is registered"), Registry.HasAction(TEXT("editor"), TEXT("pie_inject_key")));

	const TArray<FMonolithActionInfo> EditorActions = Registry.GetActions(TEXT("editor"));
	const FMonolithActionInfo* InjectKeyInfo = EditorActions.FindByPredicate([](const FMonolithActionInfo& Info)
	{
		return Info.Action == TEXT("pie_inject_key");
	});
	if (!TestNotNull(TEXT("pie_inject_key action info is available"), InjectKeyInfo))
	{
		return false;
	}
	if (!TestTrue(TEXT("pie_inject_key exposes a parameter schema"), InjectKeyInfo->ParamSchema.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* KeySchema = nullptr;
	if (TestTrue(TEXT("key schema exists"), InjectKeyInfo->ParamSchema->TryGetObjectField(TEXT("key"), KeySchema)
		&& KeySchema && KeySchema->IsValid()))
	{
		TestEqual(TEXT("key is a string"), (*KeySchema)->GetStringField(TEXT("type")), FString(TEXT("string")));
		TestTrue(TEXT("key is required"), (*KeySchema)->GetBoolField(TEXT("required")));
	}

	const TSharedPtr<FJsonObject>* EventSchema = nullptr;
	if (TestTrue(TEXT("event schema exists"), InjectKeyInfo->ParamSchema->TryGetObjectField(TEXT("event"), EventSchema)
		&& EventSchema && EventSchema->IsValid()))
	{
		TestEqual(TEXT("event defaults to tap"), (*EventSchema)->GetStringField(TEXT("default")), FString(TEXT("tap")));
		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		if (TestTrue(TEXT("event enum exists"), (*EventSchema)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues))
		{
			TestEqual(TEXT("event enum has three modes"), EnumValues->Num(), 3);
			TestEqual(TEXT("event enum[0]"), (*EnumValues)[0]->AsString(), FString(TEXT("tap")));
			TestEqual(TEXT("event enum[1]"), (*EnumValues)[1]->AsString(), FString(TEXT("pressed")));
			TestEqual(TEXT("event enum[2]"), (*EnumValues)[2]->AsString(), FString(TEXT("released")));
		}
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("key"), TEXT("Monolith_Not_A_Registered_Key"));
		const FMonolithActionResult Result = FMonolithPieInputActions::HandleInjectKey(Payload);
		TestFalse(TEXT("unknown FKey is rejected before PIE lookup"), Result.bSuccess);
		TestEqual(TEXT("unknown FKey is invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("unknown FKey error names the key"), Result.ErrorMessage.Contains(TEXT("Monolith_Not_A_Registered_Key")));
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("key"), TEXT("Escape"));
		Payload->SetStringField(TEXT("event"), TEXT("down"));
		const FMonolithActionResult Result = FMonolithPieInputActions::HandleInjectKey(Payload);
		TestFalse(TEXT("unsupported event mode is rejected before PIE lookup"), Result.bSuccess);
		TestEqual(TEXT("unsupported event mode is invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("key"), TEXT("Escape"));
		Payload->SetNumberField(TEXT("player_index"), 0.5);
		const FMonolithActionResult Result = FMonolithPieInputActions::HandleInjectKey(Payload);
		TestFalse(TEXT("fractional player_index is rejected before PIE lookup"), Result.bSuccess);
		TestEqual(TEXT("fractional player_index is invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPieStartWithUrlOptionsContractTest,
	"Monolith.Editor.PieInput.StartWithUrlOptionsContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPieStartWithUrlOptionsContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TestTrue(
		TEXT("editor.pie_start_with_url_options is registered"),
		Registry.HasAction(TEXT("editor"), TEXT("pie_start_with_url_options")));

	const TArray<FMonolithActionInfo> EditorActions = Registry.GetActions(TEXT("editor"));
	const FMonolithActionInfo* StartInfo = EditorActions.FindByPredicate([](const FMonolithActionInfo& Info)
	{
		return Info.Action == TEXT("pie_start_with_url_options");
	});
	if (!TestNotNull(TEXT("pie_start_with_url_options action info is available"), StartInfo))
	{
		return false;
	}
	if (!TestTrue(TEXT("pie_start_with_url_options exposes a parameter schema"), StartInfo->ParamSchema.IsValid()))
	{
		return false;
	}
	TestTrue(
		TEXT("description uses valid JSON double quotes in its example"),
		StartInfo->Description.Contains(TEXT("{\"url_options\":[\"TagChaseDirectStart=1\"]}")));
	TestTrue(
		TEXT("description declares offline/synchronous scope"),
		StartInfo->Description.Contains(TEXT("OFFLINE/SYNCHRONOUS PIE ONLY")));

	const TSharedPtr<FJsonObject>* OptionsSchema = nullptr;
	if (TestTrue(
		TEXT("url_options schema exists"),
		StartInfo->ParamSchema->TryGetObjectField(TEXT("url_options"), OptionsSchema)
			&& OptionsSchema && OptionsSchema->IsValid()))
	{
		TestEqual(TEXT("url_options is an array"), (*OptionsSchema)->GetStringField(TEXT("type")), FString(TEXT("array")));
		TestTrue(TEXT("url_options is required"), (*OptionsSchema)->GetBoolField(TEXT("required")));
		TestTrue(
			TEXT("url_options schema description uses valid JSON double quotes"),
			(*OptionsSchema)->GetStringField(TEXT("description")).Contains(TEXT("[\"TagChaseDirectStart=1\"]")));
	}

	TestFalse(
		TEXT("Online PIE unsupported keeps the canonical request synchronous"),
		FMonolithPieInputActions::WouldUseAsyncOnlinePieLogin(1, false, 1));
	TestFalse(
		TEXT("insufficient PIE logins select the engine's offline branch"),
		FMonolithPieInputActions::WouldUseAsyncOnlinePieLogin(2, true, 1));
	TestTrue(
		TEXT("equal desired clients and PIE logins select async Online PIE"),
		FMonolithPieInputActions::WouldUseAsyncOnlinePieLogin(2, true, 2));
	TestTrue(
		TEXT("surplus PIE logins select async Online PIE"),
		FMonolithPieInputActions::WouldUseAsyncOnlinePieLogin(1, true, 2));
	TestFalse(
		TEXT("empty PIE lifecycle state has no start conflict"),
		FMonolithPieInputActions::HasExistingPieStartConflict(false, false, false, false));
	TestTrue(
		TEXT("engine play-session-in-progress state blocks URL-option start"),
		FMonolithPieInputActions::HasExistingPieStartConflict(true, false, false, false));
	TestTrue(
		TEXT("queued play-session request blocks URL-option start"),
		FMonolithPieInputActions::HasExistingPieStartConflict(false, true, false, false));
	TestTrue(
		TEXT("existing PIE session info blocks URL-option start"),
		FMonolithPieInputActions::HasExistingPieStartConflict(false, false, true, false));
	TestTrue(
		TEXT("active PIE world blocks URL-option start"),
		FMonolithPieInputActions::HasExistingPieStartConflict(false, false, false, true));

	auto MakePayload = [](TArray<TSharedPtr<FJsonValue>> Values)
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetArrayField(TEXT("url_options"), MoveTemp(Values));
		return Payload;
	};

	auto ExpectInvalid = [this](
		const TCHAR* Label,
		const TSharedPtr<FJsonObject>& Payload,
		const TCHAR* ExpectedMessageFragment)
	{
		const FMonolithActionResult Result = FMonolithPieInputActions::HandleStartWithUrlOptions(Payload);
		const FString RejectionLabel = FString::Printf(TEXT("%s is rejected before PIE start"), Label);
		const FString ErrorCodeLabel = FString::Printf(TEXT("%s is invalid params"), Label);
		const FString SpecificErrorLabel = FString::Printf(TEXT("%s error is specific"), Label);
		TestFalse(RejectionLabel, Result.bSuccess);
		TestEqual(
			*ErrorCodeLabel,
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(
			SpecificErrorLabel,
			Result.ErrorMessage.Contains(ExpectedMessageFragment));
	};

	ExpectInvalid(
		TEXT("missing url_options"),
		MakeShared<FJsonObject>(),
		TEXT("array of strings"));

	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("url_options"), TEXT("TagChaseDirectStart=1"));
		ExpectInvalid(TEXT("non-array url_options"), Payload, TEXT("array of strings"));
	}

	ExpectInvalid(TEXT("empty url_options"), MakePayload({}), TEXT("between 1 and 32"));

	{
		TArray<TSharedPtr<FJsonValue>> TooMany;
		for (int32 Index = 0; Index < 33; ++Index)
		{
			TooMany.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Option%d=1"), Index)));
		}
		ExpectInvalid(TEXT("url_options over max count"), MakePayload(MoveTemp(TooMany)), TEXT("between 1 and 32"));
	}

	ExpectInvalid(
		TEXT("non-string option"),
		MakePayload({MakeShared<FJsonValueNumber>(1.0)}),
		TEXT("url_options[0] must be a string"));
	ExpectInvalid(
		TEXT("empty option"),
		MakePayload({MakeShared<FJsonValueString>(TEXT("   "))}),
		TEXT("must not be empty"));
	ExpectInvalid(
		TEXT("oversized option"),
		MakePayload({MakeShared<FJsonValueString>(FString::ChrN(257, TEXT('A')))}),
		TEXT("256-character limit"));
	ExpectInvalid(
		TEXT("control character"),
		MakePayload({MakeShared<FJsonValueString>(TEXT("TagChase\nDirectStart=1"))}),
		TEXT("control characters"));
	ExpectInvalid(
		TEXT("question-mark delimiter"),
		MakePayload({MakeShared<FJsonValueString>(TEXT("?TagChaseDirectStart=1"))}),
		TEXT("must not contain '?' or '#'"));
	ExpectInvalid(
		TEXT("fragment delimiter"),
		MakePayload({MakeShared<FJsonValueString>(TEXT("TagChaseDirectStart=1#fragment"))}),
		TEXT("must not contain '?' or '#'"));
	ExpectInvalid(
		TEXT("invalid option key"),
		MakePayload({MakeShared<FJsonValueString>(TEXT("1TagChaseDirectStart=1"))}),
		TEXT("invalid key"));
	ExpectInvalid(
		TEXT("case-insensitive duplicate keys"),
		MakePayload({
			MakeShared<FJsonValueString>(TEXT("TagChaseDirectStart=1")),
			MakeShared<FJsonValueString>(TEXT("tagchasedirectstart=0"))}),
		TEXT("duplicates option key"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
