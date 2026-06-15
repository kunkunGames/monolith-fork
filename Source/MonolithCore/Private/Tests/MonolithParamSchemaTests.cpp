#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithCoreTools.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithTestSupport.h"
#include "MonolithToolProfileActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

void RegisterMonolithExecutionGuardActions();

static FMonolithActionResult MonolithFindScoringNoop(const TSharedPtr<FJsonObject>&)
{
	return FMonolithActionResult::Success(MakeShared<FJsonObject>());
}

static FMonolithActionResult MonolithGuidanceFailureNoop(const TSharedPtr<FJsonObject>&)
{
	return FMonolithActionResult::Error(TEXT("forced handler failure"), FMonolithJsonUtils::ErrInvalidParams);
}

// FMonolithParamSchema alias rewriting test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaAliasesTest,
	"Monolith.ParamSchema.ApplyAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaAliasesTest::RunTest(const FString& Parameters)
{
	// Build a mock schema
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT(""), { TEXT("path") })
		.Optional(TEXT("count"), TEXT("number"), TEXT(""), TEXT("1"), { TEXT("limit") })
		.Build();

	// Test 1: canonical only
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(TEXT("ApplyAliases succeeds with canonical params"), bResult);
		TestEqual(TEXT("No collision string"), Collision, TEXT(""));
		TestEqual(TEXT("asset_path preserved"), Params->GetStringField(TEXT("asset_path")), TEXT("/Game/Map"));
		TestFalse(TEXT("path not created"), Params->HasField(TEXT("path")));
	}

	// Test 2: alias rewritten to canonical
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/Map"));
		Params->SetNumberField(TEXT("limit"), 5);

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(TEXT("ApplyAliases succeeds with alias params"), bResult);
		TestTrue(TEXT("asset_path created from alias"), Params->HasField(TEXT("asset_path")));
		TestEqual(TEXT("asset_path value matches alias"), Params->GetStringField(TEXT("asset_path")), TEXT("/Game/Map"));
		TestTrue(TEXT("count created from alias"), Params->HasField(TEXT("count")));
		TestEqual(TEXT("count value matches alias"), Params->GetNumberField(TEXT("count")), 5.0);
	}

	// Test 3: collision
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Other"));

		FString Collision;
		bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestFalse(TEXT("ApplyAliases fails on collision"), bResult);
		TestTrue(TEXT("Collision string populated"), Collision.Contains(TEXT("Param collision: both canonical 'asset_path' and alias 'path' supplied")));
	}

	return true;
}

// FMonolithParamSchema unknown keys test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaUnknownKeysTest,
	"Monolith.ParamSchema.FindUnknownKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaUnknownKeysTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT(""), { TEXT("path") })
		.Build();

	// Test 1: valid params
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("No unknown keys for canonical"), Unknown.Num(), 0);
	}

	// Test 2: valid alias
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("No unknown keys for valid alias"), Unknown.Num(), 0);
	}

	// Test 3: unknown key
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));
		Params->SetStringField(TEXT("wbp_path"), TEXT("/Game/WBP")); // Not in this schema
		Params->SetStringField(TEXT("typo_path"), TEXT("value"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Params);
		TestEqual(TEXT("Found unknown keys"), Unknown.Num(), 2);
		TestTrue(TEXT("Contains wbp_path"), Unknown.Contains(TEXT("wbp_path")));
		TestTrue(TEXT("Contains typo_path"), Unknown.Contains(TEXT("typo_path")));
	}

	// Test 4: global allowlist (asset_path is globally allowed even if not in schema)
	{
		TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Map"));

		TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(EmptySchema, Params);
		TestEqual(TEXT("asset_path is globally allowed"), Unknown.Num(), 0);
	}

	return true;
}


// FMonolithParamSchema STRICT_PARAMS test
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaStrictParamsTest,
	"Monolith.ParamSchema.StrictParamsBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaStrictParamsTest::RunTest(const FString& Parameters)
{
	// Store original value to restore later
	const FString OriginalVal = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));

	// Test 1: Set to "1"
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("1"));
	TestTrue(TEXT("IsStrictParamsEnabled returns true when STRICT_PARAMS=1"), FMonolithParamSchema::IsStrictParamsEnabled());

	// Test 2: Set to "0"
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("0"));
	TestFalse(TEXT("IsStrictParamsEnabled returns false when STRICT_PARAMS=0"), FMonolithParamSchema::IsStrictParamsEnabled());

	// Test 3: Set to empty
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT(""));
	TestFalse(TEXT("IsStrictParamsEnabled returns false when STRICT_PARAMS is empty"), FMonolithParamSchema::IsStrictParamsEnabled());

	// Test 4: Set to arbitrary string
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("true"));
	TestFalse(TEXT("IsStrictParamsEnabled returns false when STRICT_PARAMS=true"), FMonolithParamSchema::IsStrictParamsEnabled());

	// Restore original value
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *OriginalVal);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamSchemaTypedValidationTest,
	"Monolith.ParamSchema.TypedValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamSchemaTypedValidationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("mode"), TEXT("string"), TEXT(""))
		.Enum(TEXT("mode"), { TEXT("summary"), TEXT("actions"), TEXT("schema") })
		.Optional(TEXT("limit"), TEXT("integer"), TEXT(""), TEXT("10"))
		.Range(TEXT("limit"), 1, 100)
		.Optional(TEXT("fields"), TEXT("array|string"), TEXT(""))
		.Build();

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mode"), TEXT("actions"));
		Params->SetNumberField(TEXT("limit"), 25);
		Params->SetStringField(TEXT("fields"), TEXT("name,path"));

		TArray<FString> Errors;
		TestTrue(TEXT("Valid typed params pass"), FMonolithParamSchema::ValidateTypedParams(Schema, Params, Errors));
		TestEqual(TEXT("No validation errors"), Errors.Num(), 0);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mode"), TEXT("verbose"));
		Params->SetNumberField(TEXT("limit"), 0);

		TArray<FString> Errors;
		TestFalse(TEXT("Invalid enum and range fail"), FMonolithParamSchema::ValidateTypedParams(Schema, Params, Errors));
		TestEqual(TEXT("Two validation errors"), Errors.Num(), 2);
		const FString JoinedErrors = FString::Join(Errors, TEXT("\n"));
		TestTrue(TEXT("Enum error includes allowed values"), JoinedErrors.Contains(TEXT("summary")));
		TestTrue(TEXT("Range error mentions minimum"), JoinedErrors.Contains(TEXT(">= 1")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mode"), TEXT("summary"));
		Params->SetNumberField(TEXT("limit"), 1.5);

		TArray<FString> Errors;
		TestFalse(TEXT("Non-integral integer fails"), FMonolithParamSchema::ValidateTypedParams(Schema, Params, Errors));
		TestTrue(TEXT("Integer error reports expected type"), Errors.Num() > 0 && Errors[0].Contains(TEXT("integer")));
	}

	{
		TSharedPtr<FJsonObject> OptOutSchema = FParamSchemaBuilder()
			.DisableValidation()
			.Required(TEXT("mode"), TEXT("string"), TEXT(""))
			.Build();
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("mode"), 42.0);

		TArray<FString> Errors;
		TestTrue(TEXT("Explicit validation opt-out bypasses typed validation"), FMonolithParamSchema::ValidateTypedParams(OptOutSchema, Params, Errors));
		TestEqual(TEXT("Opt-out produces no validation errors"), Errors.Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionFailureGuidanceTest,
	"Monolith.ParamValidation.ActionFailureGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionFailureGuidanceTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("guidance"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterAction(
		TEXT("guidance"),
		TEXT("needs_target"),
		TEXT("Fails after schema validation so registry guidance can be verified."),
		FMonolithActionHandler::CreateStatic(&MonolithGuidanceFailureNoop),
		FParamSchemaBuilder()
			.Required(TEXT("target"), TEXT("string"), TEXT("Target identifier"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum result count"))
			.Build());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("Example"));
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("guidance"), TEXT("needs_target"), Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("Action fails as intended"), Result.bSuccess);
	bOk &= TestTrue(TEXT("Discover is a related recovery action"), Result.RelatedActions.Contains(TEXT("monolith.discover")));
	bOk &= TestTrue(TEXT("Find is a related recovery action"), Result.RelatedActions.Contains(TEXT("monolith.find")));
	bOk &= TestTrue(TEXT("Guidance hint points to schema discovery"),
		Result.Hints.ContainsByPredicate([](const FString& Hint)
		{
			return Hint.Contains(TEXT("monolith_discover")) && Hint.Contains(TEXT("needs_target"));
		}));
	bOk &= TestTrue(TEXT("Structured error data exists"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		bOk &= TestEqual(TEXT("Action id included"), Result.ErrorData->GetStringField(TEXT("action_id")), TEXT("guidance.needs_target"));
		bOk &= TestEqual(TEXT("Failure stage is handler"), Result.ErrorData->GetStringField(TEXT("failure_stage")), TEXT("handler"));
		bOk &= TestEqual(TEXT("MCP tool included"), Result.ErrorData->GetStringField(TEXT("mcp_tool")), TEXT("guidance_query"));
		bOk &= TestEqual(TEXT("Fallback skill included"), Result.ErrorData->GetStringField(TEXT("skill")), TEXT("monolith-mcp"));
		bOk &= TestEqual(TEXT("Failure cause is handler error"), Result.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("handler_error"));
		bOk &= TestEqual(TEXT("Retryability is handler-dependent"), Result.ErrorData->GetStringField(TEXT("retryability")), TEXT("depends_on_handler_error"));

		const TArray<TSharedPtr<FJsonValue>>* RequiredParams = nullptr;
		bOk &= TestTrue(TEXT("Required params are included"),
			Result.ErrorData->TryGetArrayField(TEXT("required_params"), RequiredParams) && RequiredParams && RequiredParams->Num() == 1);
		if (RequiredParams && RequiredParams->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Required = (*RequiredParams)[0]->AsObject();
			bOk &= TestTrue(TEXT("Required param object is valid"), Required.IsValid());
			if (Required.IsValid())
			{
				bOk &= TestEqual(TEXT("Required param name"), Required->GetStringField(TEXT("name")), TEXT("target"));
				bOk &= TestEqual(TEXT("Required param type"), Required->GetStringField(TEXT("type")), TEXT("string"));
			}
		}
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionFailureMissingRequiredDiagnosticShapeTest,
	"Monolith.ParamValidation.ActionFailureDiagnosticShape.MissingRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionFailureMissingRequiredDiagnosticShapeTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("guidance_missing"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterAction(
		TEXT("guidance_missing"),
		TEXT("needs_target"),
		TEXT("Requires a target so missing-param diagnostic shape can be verified."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		FParamSchemaBuilder()
			.Required(TEXT("target"), TEXT("string"), TEXT("Target identifier"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum result count"))
			.Build());

	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("guidance_missing"), TEXT("needs_target"), MakeShared<FJsonObject>());

	bool bOk = true;
	bOk &= TestFalse(TEXT("Missing required param fails before handler dispatch"), Result.bSuccess);
	bOk &= TestTrue(TEXT("Discover is a related recovery action"), Result.RelatedActions.Contains(TEXT("monolith.discover")));
	bOk &= TestTrue(TEXT("Find is a related recovery action"), Result.RelatedActions.Contains(TEXT("monolith.find")));
	bOk &= TestTrue(TEXT("Structured error data exists"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		bOk &= TestEqual(TEXT("Action id included"), Result.ErrorData->GetStringField(TEXT("action_id")), TEXT("guidance_missing.needs_target"));
		bOk &= TestEqual(TEXT("Namespace included"), Result.ErrorData->GetStringField(TEXT("namespace")), TEXT("guidance_missing"));
		bOk &= TestEqual(TEXT("Action included"), Result.ErrorData->GetStringField(TEXT("action")), TEXT("needs_target"));
		bOk &= TestEqual(TEXT("MCP tool included"), Result.ErrorData->GetStringField(TEXT("mcp_tool")), TEXT("guidance_missing_query"));
		bOk &= TestEqual(TEXT("Fallback skill included"), Result.ErrorData->GetStringField(TEXT("skill")), TEXT("monolith-mcp"));
		bOk &= TestEqual(TEXT("Failure stage is schema"), Result.ErrorData->GetStringField(TEXT("failure_stage")), TEXT("schema"));
		bOk &= TestEqual(TEXT("Failure cause is missing required param"), Result.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("missing_required_param"));
		bOk &= TestEqual(TEXT("Retryability explains required params"), Result.ErrorData->GetStringField(TEXT("retryability")), TEXT("retry_with_required_params"));
		bOk &= TestTrue(TEXT("Type validation flag included"), Result.ErrorData->HasField(TEXT("type_validation")));

		const TSharedPtr<FJsonObject>* DiscoverArgs = nullptr;
		bOk &= TestTrue(TEXT("Discover args object exists"),
			Result.ErrorData->TryGetObjectField(TEXT("discover_args"), DiscoverArgs) && DiscoverArgs && DiscoverArgs->IsValid());
		if (DiscoverArgs && DiscoverArgs->IsValid())
		{
			bOk &= TestEqual(TEXT("Discover namespace"), (*DiscoverArgs)->GetStringField(TEXT("namespace")), TEXT("guidance_missing"));
			bOk &= TestEqual(TEXT("Discover action"), (*DiscoverArgs)->GetStringField(TEXT("action")), TEXT("needs_target"));
			bOk &= TestEqual(TEXT("Discover mode"), (*DiscoverArgs)->GetStringField(TEXT("mode")), TEXT("schema"));
		}

		const TArray<TSharedPtr<FJsonValue>>* RequiredParams = nullptr;
		bOk &= TestTrue(TEXT("Required params included"),
			Result.ErrorData->TryGetArrayField(TEXT("required_params"), RequiredParams) && RequiredParams && RequiredParams->Num() == 1);
		if (RequiredParams && RequiredParams->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Required = (*RequiredParams)[0]->AsObject();
			bOk &= TestTrue(TEXT("Required param object valid"), Required.IsValid());
			if (Required.IsValid())
			{
				bOk &= TestEqual(TEXT("Required param name"), Required->GetStringField(TEXT("name")), TEXT("target"));
				bOk &= TestEqual(TEXT("Required param type"), Required->GetStringField(TEXT("type")), TEXT("string"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* OptionalParams = nullptr;
		bOk &= TestTrue(TEXT("Optional params included"),
			Result.ErrorData->TryGetArrayField(TEXT("optional_params"), OptionalParams) && OptionalParams && OptionalParams->Num() == 1);

		const TArray<TSharedPtr<FJsonValue>>* MissingParams = nullptr;
		bOk &= TestTrue(TEXT("Missing required params included"),
			Result.ErrorData->TryGetArrayField(TEXT("missing_required_params"), MissingParams) && MissingParams && MissingParams->Num() == 1);
		if (MissingParams && MissingParams->Num() == 1)
		{
			FString MissingParam;
			bOk &= TestTrue(TEXT("Missing param is string"), (*MissingParams)[0]->TryGetString(MissingParam));
			bOk &= TestEqual(TEXT("Missing param name"), MissingParam, TEXT("target"));
		}

		const TArray<TSharedPtr<FJsonValue>>* PlanningSignals = nullptr;
		bOk &= TestTrue(TEXT("Generated planning signals included"),
			Result.ErrorData->TryGetArrayField(TEXT("planning_signals"), PlanningSignals) && PlanningSignals && PlanningSignals->Num() > 0);
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionFailureUnknownParamGuidanceTest,
	"Monolith.ParamValidation.ActionFailureUnknownParamGuidance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionFailureUnknownParamGuidanceTest::RunTest(const FString& Parameters)
{
	const FString OriginalStrictParams = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("0"));

	FMonolithScopedTestNamespace Scope(TEXT("guidance_unknown"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterAction(
		TEXT("guidance_unknown"),
		TEXT("needs_target"),
		TEXT("Fails after schema validation while carrying an unknown param."),
		FMonolithActionHandler::CreateStatic(&MonolithGuidanceFailureNoop),
		FParamSchemaBuilder()
			.Required(TEXT("target"), TEXT("string"), TEXT("Target identifier"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum result count"))
			.Build());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("Example"));
	Params->SetNumberField(TEXT("limt"), 5.0);
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("guidance_unknown"), TEXT("needs_target"), Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("Action fails as intended"), Result.bSuccess);
	bOk &= TestTrue(TEXT("Structured error data exists"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		bOk &= TestEqual(TEXT("Failure stage is handler"), Result.ErrorData->GetStringField(TEXT("failure_stage")), TEXT("handler"));
		bOk &= TestEqual(TEXT("Failure cause is handler error"), Result.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("handler_error"));
		const TArray<TSharedPtr<FJsonValue>>* UnknownParams = nullptr;
		bOk &= TestTrue(TEXT("Unknown params are included on failed handler result"),
			Result.ErrorData->TryGetArrayField(TEXT("unknown_params"), UnknownParams) && UnknownParams && UnknownParams->Num() == 1);
		if (UnknownParams && UnknownParams->Num() == 1)
		{
			FString UnknownName;
			bOk &= TestTrue(TEXT("Unknown param string"), (*UnknownParams)[0]->TryGetString(UnknownName));
			bOk &= TestEqual(TEXT("Unknown param name"), UnknownName, TEXT("limt"));
		}
		bOk &= TestFalse(TEXT("STRICT_PARAMS was disabled for soft warning path"), Result.ErrorData->GetBoolField(TEXT("strict_params")));
		const TArray<TSharedPtr<FJsonValue>>* ContributingCauses = nullptr;
		bOk &= TestTrue(TEXT("Unknown param is reported as possible contributing cause"),
			Result.ErrorData->TryGetArrayField(TEXT("possible_contributing_causes"), ContributingCauses) && ContributingCauses && ContributingCauses->Num() == 1);
	}
	bOk &= TestTrue(TEXT("Unknown-param hint is included"),
		Result.Hints.ContainsByPredicate([](const FString& Hint)
		{
			return Hint.Contains(TEXT("Remove unknown params")) && Hint.Contains(TEXT("limt"));
		}));

	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *OriginalStrictParams);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionFailureValidationErrorsStructuredTest,
	"Monolith.ParamValidation.ActionFailureValidationErrorsStructured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionFailureValidationErrorsStructuredTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("guidance_validation"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterAction(
		TEXT("guidance_validation"),
		TEXT("needs_integer"),
		TEXT("Succeeds only if schema validation allows dispatch."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		FParamSchemaBuilder()
			.Required(TEXT("target"), TEXT("string"), TEXT("Target identifier"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum result count"))
			.Build());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("Example"));
	Params->SetStringField(TEXT("limit"), TEXT("not_an_integer"));
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("guidance_validation"), TEXT("needs_integer"), Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("Validation failure prevents dispatch"), Result.bSuccess);
	bOk &= TestTrue(TEXT("Structured error data exists"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		bOk &= TestEqual(TEXT("Failure stage is schema"), Result.ErrorData->GetStringField(TEXT("failure_stage")), TEXT("schema"));
		bOk &= TestEqual(TEXT("Failure cause is invalid param"), Result.ErrorData->GetStringField(TEXT("failure_cause")), TEXT("invalid_param"));
		bOk &= TestEqual(TEXT("Retryability explains typed params"), Result.ErrorData->GetStringField(TEXT("retryability")), TEXT("retry_with_validated_param_types_or_ranges"));
		const TArray<TSharedPtr<FJsonValue>>* ValidationErrors = nullptr;
		bOk &= TestTrue(TEXT("Validation errors are structured"),
			Result.ErrorData->TryGetArrayField(TEXT("validation_errors"), ValidationErrors) && ValidationErrors && ValidationErrors->Num() == 1);
		if (ValidationErrors && ValidationErrors->Num() == 1)
		{
			FString ValidationError;
			bOk &= TestTrue(TEXT("Validation error string"), (*ValidationErrors)[0]->TryGetString(ValidationError));
			bOk &= TestTrue(TEXT("Validation error names param and type"),
				ValidationError.Contains(TEXT("limit")) && ValidationError.Contains(TEXT("integer")));
		}
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFindWeightedScoringTest,
	"Monolith.Core.FindWeightedScoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFindWeightedScoringTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("findscore"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FMonolithActionSearchMetadata LayoutMetadata;
	LayoutMetadata.Keywords = { TEXT("format graph"), TEXT("arrange nodes"), TEXT("niagara graph") };
	LayoutMetadata.Aliases = { TEXT("formatter"), TEXT("layout"), TEXT("blueprint assist"), TEXT("ba") };
	LayoutMetadata.Examples = { TEXT("format vfx graph"), TEXT("auto layout particle graph") };

	Registry.RegisterAction(
		TEXT("findscore"),
		TEXT("auto_layout"),
		TEXT("Auto-layout nodes in a Niagara graph using Blueprint Assist fallback."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		nullptr,
		TEXT("layout"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		LayoutMetadata);
	Registry.RegisterAction(
		TEXT("findscore"),
		TEXT("list_emitters"),
		TEXT("List Niagara emitter handles and display names."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		nullptr,
		TEXT("emitters"));
	Registry.RegisterAction(
		TEXT("findscore"),
		TEXT("preview_system"),
		TEXT("Capture a preview image of a Niagara system."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		nullptr,
		TEXT("preview"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT("format vfx graph"));
	Params->SetStringField(TEXT("namespace"), TEXT("findscore"));
	Params->SetNumberField(TEXT("limit"), 3.0);

	const FMonolithActionResult Result = FMonolithCoreTools::HandleFind(Params);
	bool bOk = TestTrue(TEXT("find succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("scoring version is reported"), Result.Result->GetStringField(TEXT("scoring_version")), TEXT("weighted_tokens_v3"));

	const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
	bOk &= TestTrue(TEXT("matches array exists"), Result.Result->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0);
	if (!Matches || Matches->Num() == 0)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> First = (*Matches)[0]->AsObject();
	bOk &= TestTrue(TEXT("first match is an object"), First.IsValid());
	if (!First.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("natural language aliases rank layout first"), First->GetStringField(TEXT("action")), TEXT("auto_layout"));
	bOk &= TestTrue(TEXT("match exposes available status"), First->GetStringField(TEXT("status")) == TEXT("available"));
	bOk &= TestTrue(TEXT("match reports useful reason"), First->GetStringField(TEXT("reason")).Contains(TEXT("action_tokens")));

	const TArray<TSharedPtr<FJsonValue>>* MatchedTokens = nullptr;
	bOk &= TestTrue(TEXT("matched tokens array exists"), First->TryGetArrayField(TEXT("matched_tokens"), MatchedTokens) && MatchedTokens && MatchedTokens->Num() > 0);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFindGoldenQueriesTest,
	"Monolith.Core.FindGoldenQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFindGoldenQueriesTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("findgold"));
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FMonolithActionSearchMetadata CallerMetadata;
	CallerMetadata.Keywords = { TEXT("who calls"), TEXT("call sites"), TEXT("incoming calls") };
	CallerMetadata.Aliases = { TEXT("caller"), TEXT("callers") };
	CallerMetadata.Examples = { TEXT("who calls UObject"), TEXT("find callers for function") };

	Registry.RegisterAction(
		TEXT("findgold"),
		TEXT("find_callers"),
		TEXT("Find functions and methods that call a C++ symbol."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("C++ symbol, function, class, or method name"))
			.Build(),
		TEXT("source"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		CallerMetadata);

	FMonolithActionSearchMetadata ReferenceMetadata;
	ReferenceMetadata.Keywords = { TEXT("where used"), TEXT("usages"), TEXT("references") };
	ReferenceMetadata.Aliases = { TEXT("refs"), TEXT("usage") };
	ReferenceMetadata.Examples = { TEXT("where is UObject used"), TEXT("find references to symbol") };

	Registry.RegisterAction(
		TEXT("findgold"),
		TEXT("find_references"),
		TEXT("Find references and usages of a C++ symbol."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("C++ symbol, function, class, or method name"))
			.Build(),
		TEXT("source"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		ReferenceMetadata);

	FMonolithActionSearchMetadata SymbolContextMetadata;
	SymbolContextMetadata.Keywords = { TEXT("symbol context"), TEXT("definition"), TEXT("surrounding source"), TEXT("c++ code") };
	SymbolContextMetadata.Examples = { TEXT("show symbol context"), TEXT("read source around function") };

	Registry.RegisterAction(
		TEXT("findgold"),
		TEXT("get_symbol_context"),
		TEXT("Read definition and surrounding source context for a C++ symbol."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("C++ symbol, function, class, or method name"))
			.Optional(TEXT("context_lines"), TEXT("integer"), TEXT("Number of source lines around the symbol"))
			.Build(),
		TEXT("source"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		SymbolContextMetadata);

	FMonolithActionSearchMetadata LayoutMetadata;
	LayoutMetadata.Keywords = { TEXT("formatter"), TEXT("format graph"), TEXT("niagara graph"), TEXT("vfx graph") };
	LayoutMetadata.Aliases = { TEXT("arrange nodes"), TEXT("auto layout"), TEXT("blueprint assist") };
	LayoutMetadata.Examples = { TEXT("format niagara graph"), TEXT("arrange vfx nodes") };

	Registry.RegisterAction(
		TEXT("findgold"),
		TEXT("auto_layout"),
		TEXT("Format and arrange nodes in a Niagara graph."),
		FMonolithActionHandler::CreateStatic(&MonolithFindScoringNoop),
		nullptr,
		TEXT("layout"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		LayoutMetadata);

	auto FindFirst = [this](const FString& Query, FString& OutReason) -> FString
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), Query);
		Params->SetStringField(TEXT("namespace"), TEXT("findgold"));
		Params->SetNumberField(TEXT("limit"), 5.0);

		const FMonolithActionResult Result = FMonolithCoreTools::HandleFind(Params);
		if (!TestTrue(*FString::Printf(TEXT("find succeeds for '%s'"), *Query), Result.bSuccess)
			|| !Result.Result.IsValid())
		{
			return FString();
		}

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("matches exist for '%s'"), *Query),
			Result.Result->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0))
		{
			return FString();
		}

		const TSharedPtr<FJsonObject> First = (*Matches)[0]->AsObject();
		if (!TestTrue(*FString::Printf(TEXT("first match object for '%s'"), *Query), First.IsValid()))
		{
			return FString();
		}

		First->TryGetStringField(TEXT("reason"), OutReason);
		return First->GetStringField(TEXT("action"));
	};

	bool bOk = true;
	FString Reason;
	bOk &= TestEqual(TEXT("intent metadata routes who-calls query"), FindFirst(TEXT("who calls UObject"), Reason), TEXT("find_callers"));
	bOk &= TestTrue(TEXT("who-calls query uses metadata"), Reason.Contains(TEXT("metadata_tokens")));

	bOk &= TestEqual(TEXT("schema and metadata route symbol context query"), FindFirst(TEXT("symbol context c++"), Reason), TEXT("get_symbol_context"));
	bOk &= TestTrue(TEXT("symbol context query uses schema or metadata"), Reason.Contains(TEXT("schema_tokens")) || Reason.Contains(TEXT("metadata_tokens")));

	bOk &= TestEqual(TEXT("typo-tolerant metadata routes formatter query"), FindFirst(TEXT("formater niagra graph"), Reason), TEXT("auto_layout"));
	bOk &= TestTrue(TEXT("typo query reports fuzzy metadata match"), Reason.Contains(TEXT("metadata_tokens_fuzzy")));

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCoreTypedParamsTest,
	"Monolith.ParamValidation.MonolithCore.TypedParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCoreTypedParamsTest::RunTest(const FString& Parameters)
{
	auto RegisterCoreActions = [](FMonolithToolRegistry&)
	{
		FMonolithCoreTools::RegisterAll();
		RegisterMonolithExecutionGuardActions();
		FMonolithToolProfileActions::RegisterAll();
	};

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("monolith"),
		RegisterCoreActions,
		{
			{ TEXT("find"), true, TEXT("monolith.find registers") },
			{ TEXT("discover"), true, TEXT("monolith.discover registers") },
			{ TEXT("status"), true, TEXT("monolith.status registers") },
			{ TEXT("update"), true, TEXT("monolith.update registers") },
			{ TEXT("reindex"), true, TEXT("monolith.reindex registers") },
			{ TEXT("get_mcp_server_status"), true, TEXT("monolith.get_mcp_server_status registers") },
			{ TEXT("list_mcp_sessions"), true, TEXT("monolith.list_mcp_sessions registers") },
			{ TEXT("terminate_mcp_session"), true, TEXT("monolith.terminate_mcp_session registers") },
			{ TEXT("set_mcp_compatibility_options"), true, TEXT("monolith.set_mcp_compatibility_options registers") },
			{ TEXT("get_mcp_discovery_state"), true, TEXT("monolith.get_mcp_discovery_state registers") },
			{ TEXT("get_action_metadata_coverage"), true, TEXT("monolith.get_action_metadata_coverage registers") },
			{ TEXT("get_onboarding_state"), true, TEXT("monolith.get_onboarding_state registers") },
			{ TEXT("set_onboarding_state"), true, TEXT("monolith.set_onboarding_state registers") },
			{ TEXT("get_readiness_status"), true, TEXT("monolith.get_readiness_status registers") },
			{ TEXT("get_readiness_help"), true, TEXT("monolith.get_readiness_help registers") },
			{ TEXT("get_notification_settings"), true, TEXT("monolith.get_notification_settings registers") },
			{ TEXT("set_notification_settings"), true, TEXT("monolith.set_notification_settings registers") },
			{ TEXT("test_notification"), true, TEXT("monolith.test_notification registers") },
			{ TEXT("get_execution_guard_status"), true, TEXT("monolith.get_execution_guard_status registers") },
			{ TEXT("list_recent_action_audit"), true, TEXT("monolith.list_recent_action_audit registers") },
			{ TEXT("get_last_rollback"), true, TEXT("monolith.get_last_rollback registers") },
			{ TEXT("set_action_execution_policy"), true, TEXT("monolith.set_action_execution_policy registers") },
			{ TEXT("list_tool_profiles"), true, TEXT("monolith.list_tool_profiles registers") },
			{ TEXT("get_tool_profile"), true, TEXT("monolith.get_tool_profile registers") },
			{ TEXT("create_tool_profile"), true, TEXT("monolith.create_tool_profile registers") },
			{ TEXT("update_tool_profile"), true, TEXT("monolith.update_tool_profile registers") },
			{ TEXT("delete_tool_profile"), true, TEXT("monolith.delete_tool_profile registers") },
			{ TEXT("set_active_tool_profile"), true, TEXT("monolith.set_active_tool_profile registers") },
			{ TEXT("set_action_enabled"), true, TEXT("monolith.set_action_enabled registers") },
			{ TEXT("set_namespace_enabled"), true, TEXT("monolith.set_namespace_enabled registers") },
			{ TEXT("set_action_description_override"), true, TEXT("monolith.set_action_description_override registers") },
			{ TEXT("get_effective_discovery"), true, TEXT("monolith.get_effective_discovery registers") },
			{ TEXT("validate_tool_profile"), true, TEXT("monolith.validate_tool_profile registers") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("monolith"),
		RegisterCoreActions,
		{
			{
				TEXT("find"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("query"), TEXT("discover"));
					Params->SetNumberField(TEXT("limit"), 0.0);
				},
				TEXT("limit"),
				TEXT("monolith.find rejects out-of-range limit")
			},
			{
				TEXT("discover"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("namespace"), 1.0);
				},
				TEXT("namespace"),
				TEXT("monolith.discover rejects non-string namespace")
			},
			{
				TEXT("update"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("action"), TEXT("upgrade"));
				},
				TEXT("action"),
				TEXT("monolith.update rejects unknown action")
			},
			{
				TEXT("reindex"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("force"), TEXT("yes"));
				},
				TEXT("force"),
				TEXT("monolith.reindex rejects non-bool force")
			},
			{
				TEXT("list_mcp_sessions"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 1001.0);
				},
				TEXT("limit"),
				TEXT("monolith.list_mcp_sessions rejects out-of-range limit")
			},
			{
				TEXT("terminate_mcp_session"),
				[](TSharedRef<FJsonObject>)
				{
				},
				TEXT("session_id"),
				TEXT("monolith.terminate_mcp_session rejects missing session_id")
			},
			{
				TEXT("set_mcp_compatibility_options"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("options"), TEXT("loopback_only"));
				},
				TEXT("options"),
				TEXT("monolith.set_mcp_compatibility_options rejects non-object options")
			},
			{
				TEXT("get_action_metadata_coverage"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("sample_limit"), 51.0);
				},
				TEXT("sample_limit"),
				TEXT("monolith.get_action_metadata_coverage rejects out-of-range sample_limit")
			},
			{
				TEXT("set_onboarding_state"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("action"), TEXT("finish"));
				},
				TEXT("action"),
				TEXT("monolith.set_onboarding_state rejects unknown action")
			},
			{
				TEXT("set_notification_settings"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("settings"), TEXT("enabled"));
				},
				TEXT("settings"),
				TEXT("monolith.set_notification_settings rejects non-object settings")
			},
			{
				TEXT("test_notification"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("message"), 7.0);
				},
				TEXT("message"),
				TEXT("monolith.test_notification rejects non-string message")
			},
			{
				TEXT("list_recent_action_audit"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 101.0);
				},
				TEXT("limit"),
				TEXT("monolith.list_recent_action_audit rejects out-of-range limit")
			},
			{
				TEXT("set_action_execution_policy"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("action"), TEXT("monolith.status"));
					Params->SetStringField(TEXT("policy"), TEXT("read_only"));
				},
				TEXT("policy"),
				TEXT("monolith.set_action_execution_policy rejects non-object policy")
			},
			{
				TEXT("get_tool_profile"),
				[](TSharedRef<FJsonObject>)
				{
				},
				TEXT("profile_id"),
				TEXT("monolith.get_tool_profile rejects missing profile_id")
			},
			{
				TEXT("create_tool_profile"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("profile_id"), TEXT("test-profile"));
					Params->SetStringField(TEXT("mode"), TEXT("all"));
				},
				TEXT("mode"),
				TEXT("monolith.create_tool_profile rejects unknown mode")
			},
			{
				TEXT("update_tool_profile"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("profile_id"), TEXT("test-profile"));
					Params->SetStringField(TEXT("enabled_actions"), TEXT("monolith.status"));
				},
				TEXT("enabled_actions"),
				TEXT("monolith.update_tool_profile rejects non-array enabled_actions")
			},
			{
				TEXT("set_action_enabled"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("action_id"), TEXT("monolith.status"));
					Params->SetStringField(TEXT("enabled"), TEXT("true"));
				},
				TEXT("enabled"),
				TEXT("monolith.set_action_enabled rejects non-bool enabled")
			},
			{
				TEXT("set_namespace_enabled"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("namespace"), TEXT("source"));
					Params->SetStringField(TEXT("enabled"), TEXT("false"));
				},
				TEXT("enabled"),
				TEXT("monolith.set_namespace_enabled rejects non-bool enabled")
			},
			{
				TEXT("set_action_description_override"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("action_id"), TEXT("monolith.status"));
					Params->SetNumberField(TEXT("description_override"), 1.0);
				},
				TEXT("description_override"),
				TEXT("monolith.set_action_description_override rejects non-string override")
			},
			{
				TEXT("get_effective_discovery"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("namespace"), 1.0);
				},
				TEXT("namespace"),
				TEXT("monolith.get_effective_discovery rejects non-string namespace")
			}
		});

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
