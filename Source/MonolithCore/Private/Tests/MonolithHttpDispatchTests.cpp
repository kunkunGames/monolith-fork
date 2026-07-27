#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithHttpDispatch.h"
#include "MonolithParamSchema.h"
#include "MonolithTestSupport.h"

namespace
{
	TSharedRef<FJsonObject> MakeEnvelope(const FString& Mode)
	{
		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("mode"), Mode);
		return Envelope;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithHttpDispatchNormalizationTest,
	"Monolith.Core.HttpDispatch.ParamsNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpDispatchNormalizationTest::RunTest(const FString& Parameters)
{
	const TSet<FString> QueryReserved = { TEXT("namespace"), TEXT("action") };

	{
		const TSharedPtr<FJsonObject> ApplyTemplateSchema = FParamSchemaBuilder()
			.Required(TEXT("template"), TEXT("string"), TEXT(""))
			.Optional(TEXT("params"), TEXT("object"), TEXT(""))
			.Build();
		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("namespace"), TEXT("blueprint"));
		Arguments->SetStringField(TEXT("action"), TEXT("apply_template"));
		Arguments->SetStringField(TEXT("template"), TEXT("combat_character"));
		Arguments->SetObjectField(TEXT("params"), MakeEnvelope(TEXT("literal")));

		const MonolithHttpDispatch::FNormalizationResult Normalization =
			MonolithHttpDispatch::NormalizeActionArguments(Arguments, QueryReserved, ApplyTemplateSchema);
		TestTrue(TEXT("literal object params normalization succeeds"), Normalization.IsSuccess());
		const TSharedPtr<FJsonObject> Normalized = Normalization.Arguments;
		const TSharedPtr<FJsonObject>* LiteralParams = nullptr;
		TestTrue(TEXT("declared object-valued params remains an action field"),
			Normalized->TryGetObjectField(TEXT("params"), LiteralParams)
				&& LiteralParams
				&& (*LiteralParams)->GetStringField(TEXT("mode")) == TEXT("literal"));
		TestEqual(TEXT("declared params keeps sibling action fields"),
			Normalized->GetStringField(TEXT("template")), FString(TEXT("combat_character")));
		TestFalse(TEXT("transport namespace is removed"), Normalized->HasField(TEXT("namespace")));
		TestFalse(TEXT("transport action is removed"), Normalized->HasField(TEXT("action")));
	}

	{
		const TSharedPtr<FJsonObject> SearchSchema = FParamSchemaBuilder()
			.Required(TEXT("mode"), TEXT("string"), TEXT(""))
			.Build();
		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("action"), TEXT("search"));
		Arguments->SetStringField(TEXT("mode"), TEXT("top"));
		Arguments->SetObjectField(TEXT("params"), MakeEnvelope(TEXT("nested")));

		const MonolithHttpDispatch::FNormalizationResult Normalization =
			MonolithHttpDispatch::NormalizeActionArguments(
				Arguments,
				{ TEXT("action") },
				SearchSchema);
		TestTrue(TEXT("nested envelope normalization succeeds"), Normalization.IsSuccess());
		const TSharedPtr<FJsonObject> Normalized = Normalization.Arguments;
		TestEqual(TEXT("nested envelope overrides top-level compatibility extras"),
			Normalized->GetStringField(TEXT("mode")), FString(TEXT("nested")));
		TestFalse(TEXT("envelope field is consumed"), Normalized->HasField(TEXT("params")));
	}

	{
		const TSharedPtr<FJsonObject> LimitSchema = FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT(""))
			.Build();
		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("params"), TEXT("{\"limit\":7}"));
		const MonolithHttpDispatch::FNormalizationResult Normalization =
			MonolithHttpDispatch::NormalizeActionArguments(Arguments, {}, LimitSchema);
		TestTrue(TEXT("encoded envelope normalization succeeds"), Normalization.IsSuccess());
		const TSharedPtr<FJsonObject> Normalized = Normalization.Arguments;
		double Limit = 0.0;
		TestTrue(TEXT("object-encoded envelope is unwrapped"),
			Normalized->TryGetNumberField(TEXT("limit"), Limit) && Limit == 7.0);
	}

	{
		const TSharedPtr<FJsonObject> DispatcherSchema = FParamSchemaBuilder()
			.Required(TEXT("params"), TEXT("array"), TEXT(""))
			.Build();
		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("params"), TEXT("[1,2]"));
		const MonolithHttpDispatch::FNormalizationResult Normalization =
			MonolithHttpDispatch::NormalizeActionArguments(Arguments, {}, DispatcherSchema);
		TestTrue(TEXT("declared encoded array params normalization succeeds"), Normalization.IsSuccess());
		const TSharedPtr<FJsonObject> Normalized = Normalization.Arguments;
		TestEqual(TEXT("non-object encoded params is preserved for typed validation"),
			Normalized->GetStringField(TEXT("params")), FString(TEXT("[1,2]")));
	}

	{
		// Exact ambiguity regression: this is a traditional envelope whose nested
		// payload also contains an action field literally named `params`.
		const TSharedPtr<FJsonObject> DispatcherSchema = FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT(""))
			.Required(TEXT("dispatcher_name"), TEXT("string"), TEXT(""))
			.Required(TEXT("params"), TEXT("array"), TEXT(""))
			.Build();
		TSharedRef<FJsonObject> NestedEnvelope = MakeShared<FJsonObject>();
		NestedEnvelope->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
		NestedEnvelope->SetStringField(TEXT("dispatcher_name"), TEXT("OnChanged"));
		NestedEnvelope->SetArrayField(TEXT("params"), { MakeShared<FJsonValueString>(TEXT("Value")) });

		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("namespace"), TEXT("blueprint"));
		Arguments->SetStringField(TEXT("action"), TEXT("set_event_dispatcher_params"));
		Arguments->SetObjectField(TEXT("params"), NestedEnvelope);

		const MonolithHttpDispatch::FNormalizationResult Normalization =
			MonolithHttpDispatch::NormalizeActionArguments(Arguments, QueryReserved, DispatcherSchema);
		TestTrue(TEXT("nested envelope plus literal params normalization succeeds"), Normalization.IsSuccess());
		const TSharedPtr<FJsonObject> Normalized = Normalization.Arguments;
		const TArray<TSharedPtr<FJsonValue>>* DispatcherParams = nullptr;
		TestEqual(TEXT("traditional envelope retains required asset path"),
			Normalized->GetStringField(TEXT("asset_path")), FString(TEXT("/Game/BP_Test")));
		TestEqual(TEXT("traditional envelope retains required dispatcher name"),
			Normalized->GetStringField(TEXT("dispatcher_name")), FString(TEXT("OnChanged")));
		TestTrue(TEXT("nested literal params survives envelope unwrapping"),
			Normalized->TryGetArrayField(TEXT("params"), DispatcherParams)
				&& DispatcherParams
				&& DispatcherParams->Num() == 1);
	}

	{
		const TSharedPtr<FJsonObject> NoLiteralParamsSchema = FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT(""))
			.Build();

		TArray<TPair<FString, TSharedPtr<FJsonValue>>> InvalidTransportValues;
		InvalidTransportValues.Emplace(TEXT("scalar"), MakeShared<FJsonValueNumber>(7.0));
		InvalidTransportValues.Emplace(
			TEXT("array"),
			MakeShared<FJsonValueArray>(
				TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(1.0) }));
		InvalidTransportValues.Emplace(TEXT("malformed_string"), MakeShared<FJsonValueString>(TEXT("{broken")));

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Case : InvalidTransportValues)
		{
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetField(TEXT("params"), Case.Value);
			const MonolithHttpDispatch::FNormalizationResult Normalization =
				MonolithHttpDispatch::NormalizeActionArguments(Arguments, {}, NoLiteralParamsSchema);
			TestFalse(
				*FString::Printf(TEXT("undeclared %s params fails closed before dispatch"), *Case.Key),
				Normalization.IsSuccess());
			TestTrue(
				*FString::Printf(TEXT("undeclared %s params reports transport contract"), *Case.Key),
				Normalization.Error.Contains(TEXT("must be a JSON object")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithComplexParamRecoveryTest,
	"Monolith.Core.ParamSchema.StringEncodedComplexRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithComplexParamRecoveryTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("items"), TEXT("array"), TEXT(""))
		.Optional(TEXT("config"), TEXT("object|string"), TEXT(""))
		.Optional(TEXT("label"), TEXT("string"), TEXT(""))
		.Optional(TEXT("wrong_kind"), TEXT("array"), TEXT(""))
		.Optional(TEXT("malformed"), TEXT("object"), TEXT(""))
		.Optional(TEXT("native"), TEXT("array"), TEXT(""))
		.Build();

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("items"), TEXT("[1,{\"name\":\"two\"}]"));
	Params->SetStringField(TEXT("config"), TEXT("{\"enabled\":true}"));
	Params->SetStringField(TEXT("label"), TEXT("[\"must\",\"stay\",\"string\"]"));
	Params->SetStringField(TEXT("wrong_kind"), TEXT("{\"not\":\"an array\"}"));
	Params->SetStringField(TEXT("malformed"), TEXT("{broken"));
	Params->SetArrayField(TEXT("native"), { MakeShared<FJsonValueNumber>(3.0) });

	TestEqual(TEXT("only declared, matching string-encoded complex kinds recover"),
		FMonolithParamSchema::RecoverStringEncodedComplexParams(Schema, Params), 2);

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("exact array type recovers"),
		Params->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 2);
	const TSharedPtr<FJsonObject>* Config = nullptr;
	TestTrue(TEXT("union object type recovers"),
		Params->TryGetObjectField(TEXT("config"), Config)
			&& Config
			&& (*Config)->GetBoolField(TEXT("enabled")));
	TestEqual(TEXT("JSON-looking declared string is untouched"),
		Params->GetStringField(TEXT("label")), FString(TEXT("[\"must\",\"stay\",\"string\"]")));
	TestEqual(TEXT("kind mismatch remains a string"),
		Params->GetStringField(TEXT("wrong_kind")), FString(TEXT("{\"not\":\"an array\"}")));
	TestEqual(TEXT("malformed JSON remains a string"),
		Params->GetStringField(TEXT("malformed")), FString(TEXT("{broken")));

	TArray<FString> ValidationErrors;
	TestFalse(TEXT("typed validation rejects preserved kind mismatch and malformed value"),
		FMonolithParamSchema::ValidateTypedParams(Schema, Params, ValidationErrors));
	TestEqual(TEXT("both invalid complex strings are reported"), ValidationErrors.Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithActionSchemaLookupAndInputImmutabilityTest,
	"Monolith.Core.ToolRegistry.SchemaLookupAndInputImmutability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionSchemaLookupAndInputImmutabilityTest::RunTest(const FString& Parameters)
{
	const TCHAR* Namespace = TEXT("monolith_http_dispatch_test");
	FMonolithScopedTestNamespace ScopedNamespace(Namespace);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bHandlerReceivedArray = false;
	Registry.RegisterAction(
		Namespace,
		TEXT("literal_params"),
		TEXT("test action"),
		FMonolithActionHandler::CreateLambda([&bHandlerReceivedArray](const TSharedPtr<FJsonObject>& HandlerParams)
		{
			const TArray<TSharedPtr<FJsonValue>>* Recovered = nullptr;
			bHandlerReceivedArray = HandlerParams.IsValid()
				&& HandlerParams->TryGetArrayField(TEXT("params"), Recovered)
				&& Recovered
				&& Recovered->Num() == 2;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		}),
		FParamSchemaBuilder().Required(TEXT("params"), TEXT("array"), TEXT("")).Build());

	const TSharedPtr<FJsonObject> RegisteredSchema =
		Registry.GetActionParamSchema(Namespace, TEXT("literal_params"));
	TestTrue(TEXT("registered action schema is discoverable"),
		RegisteredSchema.IsValid() && RegisteredSchema->HasField(TEXT("params")));
	TestFalse(TEXT("unknown action has no schema"),
		Registry.GetActionParamSchema(Namespace, TEXT("missing")).IsValid());

	TSharedRef<FJsonObject> CallerOwnedParams = MakeShared<FJsonObject>();
	CallerOwnedParams->SetStringField(TEXT("params"), TEXT("[1,2]"));
	const FMonolithActionResult Result =
		Registry.ExecuteAction(Namespace, TEXT("literal_params"), CallerOwnedParams);
	TestTrue(TEXT("registry execution succeeds after schema-guided recovery"), Result.bSuccess);
	TestTrue(TEXT("handler receives the recovered array"), bHandlerReceivedArray);
	FString OriginalValue;
	TestTrue(TEXT("caller-owned top-level JSON remains unchanged"),
		CallerOwnedParams->TryGetStringField(TEXT("params"), OriginalValue)
			&& OriginalValue == TEXT("[1,2]"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithHttpDispatchMalformedEnvelopeCannotExecuteTest,
	"Monolith.Core.HttpDispatch.MalformedEnvelopeCannotExecute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpDispatchMalformedEnvelopeCannotExecuteTest::RunTest(const FString& Parameters)
{
	const TCHAR* Namespace = TEXT("monolith_http_dispatch_fail_closed_test");
	FMonolithScopedTestNamespace ScopedNamespace(Namespace);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bHandlerInvoked = false;
	const TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT(""))
		.Optional(TEXT("force"), TEXT("boolean"), TEXT(""))
		.Build();
	Registry.RegisterAction(
		Namespace,
		TEXT("destructive_default"),
		TEXT("fail-closed dispatch fixture"),
		FMonolithActionHandler::CreateLambda([&bHandlerInvoked](const TSharedPtr<FJsonObject>&)
		{
			bHandlerInvoked = true;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		}),
		Schema);

	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("force"), TEXT("true")); // wrong JSON kind
	TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("asset_path"), TEXT("/Game/X"));
	Arguments->SetObjectField(TEXT("params"), Envelope);

	const MonolithHttpDispatch::FNormalizationResult Normalization =
		MonolithHttpDispatch::NormalizeActionArguments(Arguments, {}, Schema);
	if (!TestTrue(TEXT("object transport envelope normalizes"), Normalization.IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("transport envelope is consumed"), Normalization.Arguments->HasField(TEXT("params")));
	TestEqual(TEXT("malformed nested field is retained for typed validation"),
		Normalization.Arguments->GetStringField(TEXT("force")), FString(TEXT("true")));

	const FMonolithActionResult Result =
		Registry.ExecuteAction(Namespace, TEXT("destructive_default"), Normalization.Arguments);
	TestFalse(TEXT("typed validation rejects the malformed envelope"), Result.bSuccess);
	TestFalse(TEXT("destructive handler is never invoked"), bHandlerInvoked);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
