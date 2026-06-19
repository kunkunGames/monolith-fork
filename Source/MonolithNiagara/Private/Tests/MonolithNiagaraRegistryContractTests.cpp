#include "CoreMinimal.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolRegistry.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ParseLastJsonLine(const FString& Contents, TSharedPtr<FJsonObject>& OutRecord)
{
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines, false);
	for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
	{
		const FString Trimmed = Lines[Index].TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		return FJsonSerializer::Deserialize(Reader, OutRecord) && OutRecord.IsValid();
	}
	return false;
}

void EnsureNiagaraActionsRegistered(FMonolithToolRegistry& Registry)
{
	if (!Registry.HasAction(TEXT("niagara"), TEXT("list_system_data_interfaces")))
	{
		FMonolithNiagaraActions::RegisterActions(Registry);
	}
}

TSharedPtr<FJsonObject> FindNiagaraActionSchema(const FString& ActionName)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("niagara")))
	{
		if (Info.Action == ActionName)
		{
			return Info.ParamSchema;
		}
	}
	return nullptr;
}

bool TryGetParamSchema(const TSharedPtr<FJsonObject>& Schema, const FString& ParamName, TSharedPtr<FJsonObject>& OutParam)
{
	if (!Schema.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Param = nullptr;
	if (!Schema->TryGetObjectField(ParamName, Param) || !Param)
	{
		return false;
	}

	OutParam = *Param;
	return OutParam.IsValid();
}

bool ParamHasAlias(const TSharedPtr<FJsonObject>& Param, const FString& Alias)
{
	const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
	if (!Param.IsValid() || !Param->TryGetArrayField(TEXT("aliases"), Aliases) || !Aliases)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Aliases)
	{
		FString AliasValue;
		if (Value.IsValid() && Value->TryGetString(AliasValue) && AliasValue == Alias)
		{
			return true;
		}
	}
	return false;
}

bool MatchResultContainsAction(
	const TSharedPtr<FJsonObject>& Result,
	const FString& ExpectedAction,
	int32& OutRank,
	FString& OutReason)
{
	OutRank = INDEX_NONE;
	OutReason.Reset();

	const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
	if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("matches"), Matches) || !Matches)
	{
		return false;
	}

	for (int32 Index = 0; Index < Matches->Num(); ++Index)
	{
		TSharedPtr<FJsonObject> Match;
		if ((*Matches)[Index].IsValid())
		{
			Match = (*Matches)[Index]->AsObject();
		}
		if (!Match.IsValid())
		{
			continue;
		}

		FString Action;
		Match->TryGetStringField(TEXT("action"), Action);
		if (Action == ExpectedAction)
		{
			OutRank = Index + 1;
			Match->TryGetStringField(TEXT("reason"), OutReason);
			return true;
		}
	}
	return false;
}

bool FocusedDiscoverParamsForAction(
	const FString& ActionName,
	TSharedPtr<FJsonObject>& OutParams)
{
	OutParams.Reset();

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("namespace"), TEXT("niagara"));
	Request->SetStringField(TEXT("action"), ActionName);
	Request->SetStringField(TEXT("mode"), TEXT("schema"));

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), Request);
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Schema = nullptr;
	if (!Result.Result->TryGetObjectField(TEXT("schema"), Schema) || !Schema || !Schema->IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (!(*Schema)->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
	{
		return false;
	}

	OutParams = *Params;
	return true;
}

bool DescribeActionSchemaParamsForAction(
	const FString& ActionName,
	TSharedPtr<FJsonObject>& OutParams)
{
	OutParams.Reset();

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("target_namespace"), TEXT("niagara"));
	Request->SetStringField(TEXT("target_action"), ActionName);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("action_schema"), Request);
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (!Result.Result->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
	{
		return false;
	}

	OutParams = *Params;
	return true;
}

bool CompareOptionalStringField(
	FAutomationTestBase& Test,
	const FString& Context,
	const TSharedPtr<FJsonObject>& Left,
	const TSharedPtr<FJsonObject>& Right,
	const TCHAR* FieldName)
{
	if (!Left.IsValid() || !Right.IsValid())
	{
		return false;
	}

	const bool bLeftHasField = Left->HasField(FieldName);
	const bool bRightHasField = Right->HasField(FieldName);
	bool bOk = Test.TestEqual(*FString::Printf(TEXT("%s %s presence matches"), *Context, FieldName), bLeftHasField, bRightHasField);
	if (bLeftHasField && bRightHasField)
	{
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("%s %s matches"), *Context, FieldName),
			Left->GetStringField(FieldName),
			Right->GetStringField(FieldName));
	}
	return bOk;
}

bool CompareOptionalNumberField(
	FAutomationTestBase& Test,
	const FString& Context,
	const TSharedPtr<FJsonObject>& Left,
	const TSharedPtr<FJsonObject>& Right,
	const TCHAR* FieldName)
{
	if (!Left.IsValid() || !Right.IsValid())
	{
		return false;
	}

	const bool bLeftHasField = Left->HasField(FieldName);
	const bool bRightHasField = Right->HasField(FieldName);
	bool bOk = Test.TestEqual(*FString::Printf(TEXT("%s %s presence matches"), *Context, FieldName), bLeftHasField, bRightHasField);
	if (bLeftHasField && bRightHasField)
	{
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("%s %s matches"), *Context, FieldName),
			Left->GetNumberField(FieldName),
			Right->GetNumberField(FieldName));
	}
	return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraRequestCompileSchemaTest, "Monolith.Registry.Niagara.RequestCompileHasOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraRequestCompileSchemaTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	TSharedPtr<FJsonObject> Schema;
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("niagara")))
	{
		if (Info.Action == TEXT("request_compile"))
		{
			Schema = Info.ParamSchema;
			break;
		}
	}
	TestNotNull(TEXT("Schema should exist"), Schema.Get());

	if (Schema)
	{
		const TSharedPtr<FJsonObject>* ForceParam = nullptr;
		bool bFoundForce = Schema->TryGetObjectField(TEXT("force"), ForceParam);
		TestTrue(TEXT("force param should exist in schema"), bFoundForce);

		const TSharedPtr<FJsonObject>* SyncParam = nullptr;
		bool bFoundSync = Schema->TryGetObjectField(TEXT("synchronous"), SyncParam);
		TestTrue(TEXT("synchronous param should exist in schema"), bFoundSync);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTranche2SchemaValidationTest, "Monolith.Registry.Niagara.Tranche2SchemasValidateTypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTranche2SchemaValidationTest::RunTest(const FString& Parameters)
{
	const TArray<FString> ActionsWithLimit = {
		TEXT("search_by_parameter"),
		TEXT("search_by_data_interface"),
		TEXT("query_niagara"),
		TEXT("find_similar_systems"),
		TEXT("search_by_material"),
		TEXT("find_niagara_references"),
	};

	for (const FString& ActionName : ActionsWithLimit)
	{
		const TSharedPtr<FJsonObject> Schema = FindNiagaraActionSchema(ActionName);
		TestNotNull(*FString::Printf(TEXT("%s schema exists"), *ActionName), Schema.Get());
		if (!Schema.IsValid())
		{
			continue;
		}

		bool bValidateTypes = false;
		TestTrue(*FString::Printf(TEXT("%s enables typed validation"), *ActionName),
			Schema->TryGetBoolField(TEXT("_validate_types"), bValidateTypes) && bValidateTypes);

		TSharedPtr<FJsonObject> LimitParam;
		TestTrue(*FString::Printf(TEXT("%s has limit param"), *ActionName),
			TryGetParamSchema(Schema, TEXT("limit"), LimitParam));
		if (LimitParam.IsValid())
		{
			TestEqual(*FString::Printf(TEXT("%s limit minimum"), *ActionName), LimitParam->GetNumberField(TEXT("minimum")), 1.0);
			TestEqual(*FString::Printf(TEXT("%s limit maximum"), *ActionName), LimitParam->GetNumberField(TEXT("maximum")), 1000.0);
		}
	}

	const TSharedPtr<FJsonObject> ListDISchema = FindNiagaraActionSchema(TEXT("list_system_data_interfaces"));
	TestNotNull(TEXT("list_system_data_interfaces schema exists"), ListDISchema.Get());
	if (ListDISchema.IsValid())
	{
		bool bValidateTypes = false;
		TestTrue(TEXT("list_system_data_interfaces enables typed validation"),
			ListDISchema->TryGetBoolField(TEXT("_validate_types"), bValidateTypes) && bValidateTypes);
	}

	const TSharedPtr<FJsonObject> SimilarSchema = FindNiagaraActionSchema(TEXT("find_similar_systems"));
	TSharedPtr<FJsonObject> ThresholdParam;
	TestTrue(TEXT("find_similar_systems has threshold param"),
		TryGetParamSchema(SimilarSchema, TEXT("threshold"), ThresholdParam));
	if (ThresholdParam.IsValid())
	{
		TestEqual(TEXT("threshold minimum"), ThresholdParam->GetNumberField(TEXT("minimum")), 0.0);
		TestEqual(TEXT("threshold maximum"), ThresholdParam->GetNumberField(TEXT("maximum")), 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTranche2AssetPathAliasTest, "Monolith.Registry.Niagara.Tranche2AssetPathAliases", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTranche2AssetPathAliasTest::RunTest(const FString& Parameters)
{
	const TArray<FString> ActionsWithSystemPathAlias = {
		TEXT("find_similar_systems"),
		TEXT("list_system_data_interfaces"),
	};

	for (const FString& ActionName : ActionsWithSystemPathAlias)
	{
		const TSharedPtr<FJsonObject> Schema = FindNiagaraActionSchema(ActionName);
		TestNotNull(*FString::Printf(TEXT("%s schema exists"), *ActionName), Schema.Get());

		TSharedPtr<FJsonObject> AssetPathParam;
		TestTrue(*FString::Printf(TEXT("%s has asset_path param"), *ActionName),
			TryGetParamSchema(Schema, TEXT("asset_path"), AssetPathParam));
		if (AssetPathParam.IsValid())
		{
			TestEqual(*FString::Printf(TEXT("%s asset_path kind"), *ActionName), AssetPathParam->GetStringField(TEXT("kind")), TEXT("AssetPath"));
			TestTrue(*FString::Printf(TEXT("%s accepts system_path alias"), *ActionName),
				ParamHasAlias(AssetPathParam, TEXT("system_path")));
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("system_path"), TEXT("/Game/VFX/NS_A.NS_A"));
		FString Collision;
		TestTrue(*FString::Printf(TEXT("%s rewrites system_path alias"), *ActionName),
			FMonolithParamSchema::ApplyAliases(Schema, Params, Collision));
		TestTrue(*FString::Printf(TEXT("%s canonical asset_path exists after alias rewrite"), *ActionName),
			Params->HasField(TEXT("asset_path")));
	}

	const TSharedPtr<FJsonObject> SimilarSchema = FindNiagaraActionSchema(TEXT("find_similar_systems"));
	TSharedPtr<FJsonObject> InvalidSimilarParams = MakeShared<FJsonObject>();
	InvalidSimilarParams->SetStringField(TEXT("asset_path"), TEXT("/Game/VFX/NS_A.NS_A"));
	InvalidSimilarParams->SetNumberField(TEXT("threshold"), 2.0);
	InvalidSimilarParams->SetNumberField(TEXT("limit"), 0.0);

	TArray<FString> Errors;
	TestFalse(TEXT("find_similar_systems rejects out-of-range threshold and limit"),
		FMonolithParamSchema::ValidateTypedParams(SimilarSchema, InvalidSimilarParams, Errors));
	TestTrue(TEXT("find_similar_systems reports multiple validation errors"), Errors.Num() >= 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTranche2RoutingQualityTest, "Monolith.Registry.Niagara.Tranche2RoutingQuality", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTranche2RoutingQualityTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	TestTrue(TEXT("monolith.find is registered"), Registry.HasAction(TEXT("monolith"), TEXT("find")));
	if (!Registry.HasAction(TEXT("monolith"), TEXT("find")))
	{
		return false;
	}

	struct FRepresentativeRoutingCase
	{
		const TCHAR* Query;
		const TCHAR* ExpectedAction;
	};

	const FRepresentativeRoutingCase Cases[] = {
		{ TEXT("find niagara systems by user parameter"), TEXT("search_by_parameter") },
		{ TEXT("find niagara systems using data interface curve"), TEXT("search_by_data_interface") },
		{ TEXT("query niagara systems with gpu renderer filters"), TEXT("query_niagara") },
		{ TEXT("find similar niagara systems"), TEXT("find_similar_systems") },
		{ TEXT("find niagara systems using material"), TEXT("search_by_material") },
		{ TEXT("find niagara references for asset"), TEXT("find_niagara_references") },
		{ TEXT("list data interfaces used by niagara system"), TEXT("list_system_data_interfaces") },
	};

	bool bOk = true;
	for (const FRepresentativeRoutingCase& Case : Cases)
	{
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("query"), Case.Query);
		Request->SetStringField(TEXT("namespace"), TEXT("niagara"));
		Request->SetNumberField(TEXT("limit"), 8.0);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("monolith"), TEXT("find"), Request);
		bOk &= TestTrue(*FString::Printf(TEXT("monolith.find succeeds for '%s'"), Case.Query), Result.bSuccess);
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			continue;
		}

		int32 Rank = INDEX_NONE;
		FString Reason;
		const bool bFound = MatchResultContainsAction(Result.Result, Case.ExpectedAction, Rank, Reason);
		bOk &= TestTrue(
			*FString::Printf(TEXT("'%s' returns %s in top 8"), Case.Query, Case.ExpectedAction),
			bFound);
		if (bFound)
		{
			AddInfo(FString::Printf(TEXT("Routing query '%s' found %s at rank %d via %s"), Case.Query, Case.ExpectedAction, Rank, *Reason));
		}
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTranche2FocusedSchemaDiscoveryTest, "Monolith.Registry.Niagara.Tranche2FocusedSchemaDiscovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTranche2FocusedSchemaDiscoveryTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	TestTrue(TEXT("monolith.discover is registered"), Registry.HasAction(TEXT("monolith"), TEXT("discover")));
	TestTrue(TEXT("describe.action_schema is registered"), Registry.HasAction(TEXT("describe"), TEXT("action_schema")));
	if (!Registry.HasAction(TEXT("monolith"), TEXT("discover")) || !Registry.HasAction(TEXT("describe"), TEXT("action_schema")))
	{
		return false;
	}

	struct FFocusedSchemaCase
	{
		const TCHAR* Action;
		TArray<const TCHAR*> RequiredParams;
	};

	const FFocusedSchemaCase Cases[] = {
		{ TEXT("search_by_parameter"), { TEXT("parameter_name"), TEXT("limit") } },
		{ TEXT("search_by_data_interface"), { TEXT("di_class"), TEXT("limit") } },
		{ TEXT("query_niagara"), { TEXT("query_string"), TEXT("limit") } },
		{ TEXT("find_similar_systems"), { TEXT("asset_path"), TEXT("folder"), TEXT("threshold"), TEXT("limit") } },
		{ TEXT("search_by_material"), { TEXT("material_path"), TEXT("limit") } },
		{ TEXT("find_niagara_references"), { TEXT("asset_path"), TEXT("limit") } },
		{ TEXT("list_system_data_interfaces"), { TEXT("asset_path") } },
	};

	bool bOk = true;
	for (const FFocusedSchemaCase& Case : Cases)
	{
		TSharedPtr<FJsonObject> DiscoverParams;
		const bool bDiscovered = FocusedDiscoverParamsForAction(Case.Action, DiscoverParams);
		bOk &= TestTrue(*FString::Printf(TEXT("focused schema discovery succeeds for niagara.%s"), Case.Action), bDiscovered);

		TSharedPtr<FJsonObject> DescribeParams;
		const bool bDescribed = DescribeActionSchemaParamsForAction(Case.Action, DescribeParams);
		bOk &= TestTrue(*FString::Printf(TEXT("describe.action_schema succeeds for niagara.%s"), Case.Action), bDescribed);
		if (!bDiscovered || !DiscoverParams.IsValid() || !bDescribed || !DescribeParams.IsValid())
		{
			continue;
		}

		bool bDiscoverValidateTypes = false;
		bOk &= TestTrue(
			*FString::Printf(TEXT("niagara.%s focused discover schema exposes _validate_types"), Case.Action),
			DiscoverParams->TryGetBoolField(TEXT("_validate_types"), bDiscoverValidateTypes) && bDiscoverValidateTypes);
		bool bDescribeValidateTypes = false;
		bOk &= TestTrue(
			*FString::Printf(TEXT("niagara.%s describe.action_schema exposes _validate_types"), Case.Action),
			DescribeParams->TryGetBoolField(TEXT("_validate_types"), bDescribeValidateTypes) && bDescribeValidateTypes);

		for (const TCHAR* ParamName : Case.RequiredParams)
		{
			bOk &= TestTrue(
				*FString::Printf(TEXT("niagara.%s focused discover schema includes %s"), Case.Action, ParamName),
				DiscoverParams->HasField(ParamName));
			bOk &= TestTrue(
				*FString::Printf(TEXT("niagara.%s describe.action_schema includes %s"), Case.Action, ParamName),
				DescribeParams->HasField(ParamName));

			TSharedPtr<FJsonObject> DiscoverParam;
			TSharedPtr<FJsonObject> DescribeParam;
			if (TryGetParamSchema(DiscoverParams, ParamName, DiscoverParam) && TryGetParamSchema(DescribeParams, ParamName, DescribeParam))
			{
				const FString Context = FString::Printf(TEXT("niagara.%s.%s discover/describe schema"), Case.Action, ParamName);
				bOk &= CompareOptionalStringField(*this, Context, DiscoverParam, DescribeParam, TEXT("type"));
				bOk &= CompareOptionalStringField(*this, Context, DiscoverParam, DescribeParam, TEXT("kind"));
				bOk &= CompareOptionalNumberField(*this, Context, DiscoverParam, DescribeParam, TEXT("minimum"));
				bOk &= CompareOptionalNumberField(*this, Context, DiscoverParam, DescribeParam, TEXT("maximum"));
			}
		}
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraTranche2DailyLogSchemaRejectionTest, "Monolith.Registry.Niagara.Tranche2DailyLogSchemaRejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraTranche2DailyLogSchemaRejectionTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalDailyLog = Settings->bEnableDailyLog;
	const FString OriginalLogDir = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_DIR"));
	const FString TempLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithNativeRoutingNiagaraDailyLogTest"));
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	IFileManager::Get().MakeDirectory(*TempLogDir, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *TempLogDir);
	Settings->bEnableDailyLog = true;

	ON_SCOPE_EXIT
	{
		Settings->bEnableDailyLog = bOriginalDailyLog;
		FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *OriginalLogDir);
		IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	};

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	TSharedPtr<FJsonObject> RoutingContext = MakeShared<FJsonObject>();
	RoutingContext->SetStringField(TEXT("intent"), TEXT("native_routing_schema_rejection_smoke"));
	RoutingContext->SetStringField(TEXT("source"), TEXT("automation"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material_path"), TEXT("/Game/VFX/MissingMaterial.MissingMaterial"));
	Params->SetNumberField(TEXT("limit"), 0.0);

	FMonolithActionResult Result;
	{
		FMonolithToolInvocationLogger::FScopedTrace Trace(
			TEXT("native-routing-niagara-schema-rejection-trace"),
			FString(),
			FString(),
			TEXT("automation-session"),
			RoutingContext);
		Result = Registry.ExecuteAction(TEXT("niagara"), TEXT("search_by_material"), Params);
	}

	TestFalse(TEXT("search_by_material rejects invalid limit before handler dispatch"), Result.bSuccess);
	TestEqual(TEXT("schema rejection uses invalid params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("schema rejection mentions limit"), Result.ErrorMessage.Contains(TEXT("limit")));

	const FString DailyActionLogPath = FPaths::Combine(TempLogDir, FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT("action.jsonl"));
	TestTrue(TEXT("Niagara schema rejection action log exists"), FPaths::FileExists(DailyActionLogPath));

	FString Contents;
	if (TestTrue(TEXT("Niagara schema rejection action log can be read"), FFileHelper::LoadFileToString(Contents, *DailyActionLogPath)))
	{
		TSharedPtr<FJsonObject> Record;
		if (TestTrue(TEXT("Niagara schema rejection log line parses as JSON"), ParseLastJsonLine(Contents, Record)) && Record.IsValid())
		{
			TestEqual(TEXT("Niagara schema rejection status"), Record->GetStringField(TEXT("status")), TEXT("error"));

			const TSharedPtr<FJsonObject>* Call = nullptr;
			if (TestTrue(TEXT("Niagara schema rejection call object exists"), Record->TryGetObjectField(TEXT("call"), Call)) && Call && Call->IsValid())
			{
				TestEqual(TEXT("Niagara schema rejection namespace"), (*Call)->GetStringField(TEXT("namespace")), TEXT("niagara"));
				TestEqual(TEXT("Niagara schema rejection action"), (*Call)->GetStringField(TEXT("action")), TEXT("search_by_material"));
				TestEqual(TEXT("Niagara schema rejection validation phase"), (*Call)->GetStringField(TEXT("validation_phase")), TEXT("schema"));
			}

			const TSharedPtr<FJsonObject>* LoggedRoutingContext = nullptr;
			if (TestTrue(TEXT("Niagara schema rejection routing_context exists"), Record->TryGetObjectField(TEXT("routing_context"), LoggedRoutingContext)) && LoggedRoutingContext && LoggedRoutingContext->IsValid())
			{
				TestEqual(TEXT("Niagara schema rejection routing intent"), (*LoggedRoutingContext)->GetStringField(TEXT("intent")), TEXT("native_routing_schema_rejection_smoke"));
				TestEqual(TEXT("Niagara schema rejection routing source"), (*LoggedRoutingContext)->GetStringField(TEXT("source")), TEXT("automation"));
			}

			const TSharedPtr<FJsonObject>* AgentSignal = nullptr;
			if (TestTrue(TEXT("Niagara schema rejection agent_signal exists"), Record->TryGetObjectField(TEXT("agent_signal"), AgentSignal)) && AgentSignal && AgentSignal->IsValid())
			{
				TestEqual(TEXT("Niagara schema rejection outcome"), (*AgentSignal)->GetStringField(TEXT("outcome")), TEXT("validation_rejected"));
				TestEqual(TEXT("Niagara schema rejection error code"), static_cast<int32>((*AgentSignal)->GetNumberField(TEXT("error_code"))), FMonolithJsonUtils::ErrInvalidParams);
				TestEqual(TEXT("Niagara schema rejection error class"), (*AgentSignal)->GetStringField(TEXT("error_class")), TEXT("missing_param"));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraDuplicateEmitterAliasTest, "Monolith.Registry.Niagara.DuplicateEmitterAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraDuplicateEmitterAliasTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	EnsureNiagaraActionsRegistered(Registry);

	TSharedPtr<FJsonObject> DescribeParams;
	bool bDescribed = DescribeActionSchemaParamsForAction(TEXT("duplicate_emitter"), DescribeParams);
	TestTrue(TEXT("describe.action_schema succeeds for niagara.duplicate_emitter"), bDescribed);
	if (bDescribed && DescribeParams.IsValid())
	{
		TSharedPtr<FJsonObject> SourceEmitterParam;
		if (TestTrue(TEXT("source_emitter param schema found"), TryGetParamSchema(DescribeParams, TEXT("source_emitter"), SourceEmitterParam)))
		{
			TestTrue(TEXT("source_emitter has 'emitter' alias"), ParamHasAlias(SourceEmitterParam, TEXT("emitter")));
		}
	}

	return true;
}
